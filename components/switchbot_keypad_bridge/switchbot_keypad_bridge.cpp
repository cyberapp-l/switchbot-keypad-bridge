#include "switchbot_keypad_bridge.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <vector>

#include <cJSON.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <psa/crypto.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#include "ble_utils.h"
#include "keypad_advert.h"
#include "mac_utils.h"

namespace esphome {
namespace switchbot_keypad_bridge {

namespace {

const char *const TAG = "switchbot_keypad_bridge";

// BLE service / characteristic UUIDs as exposed by a real SwitchBot Lock.
constexpr const char *SERVICE_UUID = "cba20d00-224d-11e6-9fb8-0002a5d5c51b";
constexpr const char *RX_CHAR_UUID = "cba20002-224d-11e6-9fb8-0002a5d5c51b";
constexpr const char *TX_CHAR_UUID = "cba20003-224d-11e6-9fb8-0002a5d5c51b";

// SwitchBot's OUI. Applied to the chip's factory MAC at boot so the bridge
// advertises within SwitchBot's address range — the Keypad Vision filters
// scan results on this prefix.
constexpr uint8_t SPOOFED_OUI[3] = {0xB0, 0xE9, 0xFE};

// Manufacturer-specific advertising blob published by the genuine lock.
constexpr uint8_t ADVERTISING_MFG_DATA[] = {0x27, 0x09, 0x00, 0x10,
                                            0xA5, 0xB8, 0x00, 0x00, 0x00};

// Encrypted response payloads sent back to the keypad on lock/unlock.
constexpr uint8_t RESPONSE_LOCK[5]   = {0x90, 0x0A, 0x7F, 0x7F, 0x00};
constexpr uint8_t RESPONSE_UNLOCK[5] = {0x98, 0x08, 0x7F, 0x7F, 0x00};

// Trailing 13 bytes appended to the lock-state byte when answering a state poll.
constexpr uint8_t STATE_PAYLOAD_TAIL[13] = {0x08, 0x08, 0x41, 0x00, 0x00, 0x00, 0x00,
                                            0x80, 0xF2, 0xFB, 0x00, 0x00, 0x00};

constexpr size_t AES_KEY_SIZE = 16;

// Battery advert scan: one short active window per interval.
constexpr uint32_t BATTERY_SCAN_DURATION_MS = 5000;
constexpr uint32_t BATTERY_SCAN_RETRY_MS    = 30000;

}  // namespace

// ---------------------------------------------------------------------------
// NimBLE callback bridges
// ---------------------------------------------------------------------------

class SwitchbotKeypadBridge::ServerCallbacks : public NimBLEServerCallbacks {
 public:
  explicit ServerCallbacks(SwitchbotKeypadBridge *parent) : parent_(parent) {}

  void onConnect(NimBLEServer *server, NimBLEConnInfo &info) override {
    this->parent_->push_connect_();
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &info, int reason) override {
    this->parent_->push_disconnect_();
  }

 private:
  SwitchbotKeypadBridge *parent_;
};

class SwitchbotKeypadBridge::RxCharCallbacks : public NimBLECharacteristicCallbacks {
 public:
  explicit RxCharCallbacks(SwitchbotKeypadBridge *parent) : parent_(parent) {}

  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &info) override {
    const std::string value = characteristic->getValue();
    if (value.empty())
      return;
    this->parent_->push_rx_(value);
  }

 private:
  SwitchbotKeypadBridge *parent_;
};

class SwitchbotKeypadBridge::BatteryScanCallbacks : public NimBLEScanCallbacks {
 public:
  explicit BatteryScanCallbacks(SwitchbotKeypadBridge *parent) : parent_(parent) {}

  void onResult(const NimBLEAdvertisedDevice *adv) override {
    this->parent_->handle_battery_advert_(adv);
  }

 private:
  SwitchbotKeypadBridge *parent_;
};

// ---------------------------------------------------------------------------
// Thread-safe event queueing
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::push_connect_() {
  std::lock_guard<std::mutex> lk(this->rx_mutex_);
  this->rx_queue_.push_back({QueuedEvent::CONNECT, ""});
}

void SwitchbotKeypadBridge::push_disconnect_() {
  std::lock_guard<std::mutex> lk(this->rx_mutex_);
  this->rx_queue_.push_back({QueuedEvent::DISCONNECT, ""});
}

void SwitchbotKeypadBridge::push_rx_(const std::string &frame) {
  std::lock_guard<std::mutex> lk(this->rx_mutex_);
  this->rx_queue_.push_back({QueuedEvent::RX, frame});
}

// ---------------------------------------------------------------------------
// Component lifecycle
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::setup() {
  // Load — or, on first boot, generate — the 16-byte AES-128 session key.
  // unpair() rotates it; it is never part of the YAML config.
  this->shared_key_pref_ =
      global_preferences->make_preference<std::array<uint8_t, 16>>(0x534B4559UL /* 'SKEY' */);
  if (!this->shared_key_pref_.load(&this->shared_key_)) {
    this->create_shared_key_();
    ESP_LOGI(TAG, "Generated a fresh shared key");
  }

  // Restore the paired keypad name (if any) and publish it to the sensor.
  this->keypad_name_pref_ = global_preferences->make_preference<char[KEYPAD_NAME_MAX]>(
      0x534B5042UL /* 'SKPB' */);
  char stored_name[KEYPAD_NAME_MAX] = {};
  bool have_keypad = false;
  if (this->keypad_name_pref_.load(&stored_name) && stored_name[0] != '\0') {
    stored_name[KEYPAD_NAME_MAX - 1] = '\0';
    have_keypad = true;
    if (this->keypad_text_sensor_ != nullptr) {
      this->keypad_text_sensor_->publish_state(stored_name);
    }
    ESP_LOGI(TAG, "Paired keypad: '%s'", stored_name);
  } else {
    if (this->keypad_text_sensor_ != nullptr) {
      this->keypad_text_sensor_->publish_state("Unpaired");
    }
  }
  this->keypad_paired_ = have_keypad;

  // Restore the keypad MAC + family used by the battery advert scan. Missing
  // for keypads paired before this field existed — the scan learns it then.
  this->keypad_info_pref_ =
      global_preferences->make_preference<KeypadInfo>(0x534B4D43UL /* 'SKMC' */);
  if (!this->keypad_info_pref_.load(&this->keypad_info_)) {
    this->keypad_info_ = KeypadInfo{};
  }
  this->battery_scan_callbacks_ = new BatteryScanCallbacks(this);
  this->next_battery_scan_at_ = millis() + BATTERY_SCAN_RETRY_MS;

  // Restore user-name mappings edited in the web UI (persisted to NVS).
  this->load_web_users_();

  // Restore web-edited settings (e.g. battery scan interval). Overrides the
  // YAML default, which was already applied via set_battery_scan_interval().
  this->load_settings_();

  if (!this->prepare_keys_() || !this->prepare_ble_()) {
    this->mark_failed();
    return;
  }
  ESP_LOGI(TAG, "Ready. Advertising on %s",
           NimBLEDevice::getAddress().toString().c_str());

  this->pairing_ui_.set_shared_key(this->shared_key_);
  this->pairing_ui_.set_credentials(this->web_user_, this->web_pass_);
  this->pairing_ui_.set_events_provider([this]() { return this->events_json_(); });
  this->pairing_ui_.set_users_get_provider([this]() { return this->web_users_json_(); });
  this->pairing_ui_.set_users_set_handler(
      [this](const std::string &body) { return this->set_web_users_json_(body); });
  this->pairing_ui_.set_settings_get_provider([this]() { return this->web_settings_json_(); });
  this->pairing_ui_.set_settings_set_handler(
      [this](const std::string &body) { return this->set_web_settings_json_(body); });
  this->pairing_ui_.set_on_paired_callback(
      [this](const std::string &name, const std::string &mac,
             KeypadFamily family) {
        // Runs on the HTTP-server task — keep it minimal: stash the fields and
        // raise a flag. loop() (the main task) publishes the text sensor and
        // writes NVS. The server stays up as the config console.
        this->pending_keypad_name_ = name;
        this->pending_keypad_mac_ = mac;
        this->pending_keypad_family_ = family;
        this->pending_pair_apply_.store(true, std::memory_order_release);
      });
  // The web server now runs permanently: it is both the first-boot pairing
  // wizard and the ongoing config console. When a web_password is set every
  // endpoint is behind HTTP Basic Auth; otherwise it stays open as before.
  if (this->pairing_ui_.start()) {
    ESP_LOGI(TAG, "Web UI listening on port 80%s",
             this->web_pass_.empty() ? " (open — set web_password to require a login)"
                                      : " (Basic Auth enabled)");
  } else {
    ESP_LOGW(TAG, "Web UI failed to start; check port 80 is free");
  }
}

void SwitchbotKeypadBridge::loop() {
  // Apply a pairing that completed on the HTTP-server task. The text-sensor
  // publish and the NVS write run here, on the main task.
  if (this->pending_pair_apply_.exchange(false, std::memory_order_acquire)) {
    const std::string &name = this->pending_keypad_name_;

    // Persist the name so it survives a reboot and can be re-published.
    char buf[KEYPAD_NAME_MAX] = {};
    name.copy(buf, sizeof(buf) - 1);
    const bool saved = this->keypad_name_pref_.save(&buf);

    // Persist the keypad's MAC + family for the battery advert scan.
    KeypadInfo info{};
    if (parse_mac_pretty(this->pending_keypad_mac_, info.mac)) {
      info.family = static_cast<uint8_t>(this->pending_keypad_family_);
      info.valid = 1;
    }
    this->keypad_info_ = info;
    this->keypad_info_pref_.save(&this->keypad_info_);
    global_preferences->sync();

    this->keypad_paired_ = true;
    this->last_battery_ = -1;
    // Pick the new keypad's battery up shortly, not a full interval away.
    this->next_battery_scan_at_ = millis() + 10000;

    if (this->keypad_text_sensor_ != nullptr) {
      this->keypad_text_sensor_->publish_state(name);
    }
    ESP_LOGI(TAG, "Paired keypad: '%s' (nvs save %s)", name.c_str(),
             saved ? "ok" : "FAILED");

    // The web server stays up after pairing — it is the ongoing config console
    // now, not just the one-shot wizard. (Previously it was stopped here.)
  }

  // Flush web-edited user names to NVS on the main task (the HTTP handler only
  // stages them + raises the flag, mirroring the pairing hand-off).
  if (this->web_users_dirty_.exchange(false, std::memory_order_acquire)) {
    this->save_web_users_();
  }
  if (this->settings_dirty_.exchange(false, std::memory_order_acquire)) {
    this->save_settings_();
  }

  // Drain the RX queue. Swapping the vector out keeps the critical section
  // free of allocation and copying, so the NimBLE task never waits on it.
  std::vector<QueuedEvent> pending;
  {
    std::lock_guard<std::mutex> lk(this->rx_mutex_);
    pending.swap(this->rx_queue_);
  }

  for (const auto &ev : pending) {
    if (ev.type == QueuedEvent::CONNECT) {
      ESP_LOGI(TAG, "Keypad connected");
      this->session_.reset();
      if (this->connected_sensor_ != nullptr) this->connected_sensor_->publish_state(true);
      this->publish_last_seen_();  // a connection is proof of life
    } else if (ev.type == QueuedEvent::DISCONNECT) {
      ESP_LOGI(TAG, "Keypad disconnected, restarting advertising");
      this->session_.reset();
      NimBLEDevice::startAdvertising();
      if (this->connected_sensor_ != nullptr) this->connected_sensor_->publish_state(false);
    } else if (ev.type == QueuedEvent::RX) {
      this->on_rx_frame_(ev.frame);
    }
  }

  // The advert scan feeds battery, RSSI, last-seen and the alarm/status flags —
  // run it if any advert-derived entity is wired.
  if (this->battery_level_sensor_ != nullptr || this->rssi_sensor_ != nullptr ||
      this->last_seen_sensor_ != nullptr || this->tamper_sensor_ != nullptr ||
      this->duress_sensor_ != nullptr || this->lockout_sensor_ != nullptr ||
      this->motion_sensor_ != nullptr || this->charging_sensor_ != nullptr) {
    this->apply_pending_battery_();
    this->maybe_start_battery_scan_();
  }
}

void SwitchbotKeypadBridge::unpair() {
  ESP_LOGI(TAG, "Unpairing — rotating the shared key and re-opening the pairing wizard");

  // Rotate the key and swap it into the live crypto slot: the previously
  // paired keypad can no longer command the bridge, with no reboot needed.
  this->create_shared_key_();
  if (!this->import_aes_key_()) {
    ESP_LOGE(TAG, "Key rotation failed — unpair aborted");
    return;
  }

  // Stop an in-flight battery scan window: the wizard's own blocking scans
  // share the NimBLE scan singleton and must not race it.
  if (this->battery_scan_active_.exchange(false)) {
    NimBLEScan *scan = NimBLEDevice::getScan();
    if (scan->isScanning()) {
      scan->stop();
    }
    scan->clearResults();
  }

  // Forget the paired keypad name and its battery-scan identity.
  char empty[KEYPAD_NAME_MAX] = {};
  this->keypad_name_pref_.save(&empty);
  this->keypad_info_ = KeypadInfo{};
  this->keypad_info_pref_.save(&this->keypad_info_);
  global_preferences->sync();
  if (this->keypad_text_sensor_ != nullptr) {
    this->keypad_text_sensor_->publish_state("Unpaired");
  }
  this->keypad_paired_ = false;
  this->last_battery_ = -1;
  if (this->battery_level_sensor_ != nullptr) {
    this->battery_level_sensor_->publish_state(NAN);
  }

  // Invalidate any in-flight BLE session — it is keyed to the old secret —
  // and forget the learned token slot along with it.
  this->session_.reset();
  this->session_.forget_slot();

  // Re-enter pairing mode straight away with the rotated key.
  this->pairing_ui_.set_shared_key(this->shared_key_);
  if (this->pairing_ui_.start()) {
    ESP_LOGI(TAG, "Unpaired — re-entering pairing mode");
  } else {
    ESP_LOGW(TAG, "Pairing UI failed to start; check port 80 is free");
  }
}

void UnpairButton::press_action() {
  if (this->parent_->is_pairing_active()) {
    ESP_LOGW(TAG, "Pairing is already active, ignoring Unpair action");
    return;
  }
  this->parent_->unpair();
}

void SwitchbotKeypadBridge::dump_config() {
  ESP_LOGCONFIG(TAG, "SwitchBot Keypad Bridge:");
  ESP_LOGCONFIG(TAG, "  BLE address: %s", NimBLEDevice::getAddress().toString().c_str());
  ESP_LOGCONFIG(TAG, "  Pairing UI: port 80");
  if (this->battery_level_sensor_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Battery scan interval: %us",
                  static_cast<unsigned>(this->battery_scan_interval_ms_ / 1000));
  }
  if (this->is_failed()) {
    ESP_LOGE(TAG, "  Initialization failed - see previous errors");
  }
}

void SwitchbotKeypadBridge::create_shared_key_() {
  esp_fill_random(this->shared_key_.data(), this->shared_key_.size());
  this->shared_key_pref_.save(&this->shared_key_);
  global_preferences->sync();
}

bool SwitchbotKeypadBridge::import_aes_key_() {
  // Drop any previously imported handle so the slot can be re-keyed in
  // place — unpair() rotates the key without rebooting.
  if (this->aes_key_handle_ != PSA_KEY_ID_NULL) {
    psa_destroy_key(this->aes_key_handle_);
    this->aes_key_handle_ = PSA_KEY_ID_NULL;
  }

  psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
  psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT);
  psa_set_key_algorithm(&attrs, PSA_ALG_CTR);
  psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
  psa_set_key_bits(&attrs, 128);
  psa_status_t status = psa_import_key(&attrs, this->shared_key_.data(), AES_KEY_SIZE,
                                       &this->aes_key_handle_);
  psa_reset_key_attributes(&attrs);

  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "AES key import failed (%d)", static_cast<int>(status));
    return false;
  }
  this->session_.set_aes_key(this->aes_key_handle_);
  return true;
}

bool SwitchbotKeypadBridge::prepare_keys_() {
  psa_status_t status = psa_crypto_init();
  if (status != PSA_SUCCESS) {
    ESP_LOGE(TAG, "PSA Crypto init failed (%d)", static_cast<int>(status));
    return false;
  }
  return this->import_aes_key_();
}

bool SwitchbotKeypadBridge::prepare_ble_() {
  // Spoof a SwitchBot-OUI BLE address: keep the chip-unique tail, replace
  // the leading three bytes. The Vision keypad filters scan results on this
  // prefix and would otherwise ignore the bridge. Must run before NimBLE
  // init so the controller picks up the patched base MAC.
  uint8_t base[6];
  esp_err_t err = esp_efuse_mac_get_default(base);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_efuse_mac_get_default failed (err=0x%X)", err);
    return false;
  }
  std::memcpy(base, SPOOFED_OUI, sizeof(SPOOFED_OUI));
  err = esp_base_mac_addr_set(base);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_base_mac_addr_set failed (err=0x%X)", err);
    return false;
  }
  ESP_LOGD(TAG, "Base MAC set to %02X:%02X:%02X:%02X:%02X:%02X (SwitchBot OUI spoof)",
           base[0], base[1], base[2], base[3], base[4], base[5]);

  NimBLEDevice::init("WoLock");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  this->server_ = NimBLEDevice::createServer();
  this->server_->setCallbacks(new ServerCallbacks(this));

  NimBLEService *service = this->server_->createService(SERVICE_UUID);
  this->tx_characteristic_ = service->createCharacteristic(TX_CHAR_UUID, NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *rx = service->createCharacteristic(
      RX_CHAR_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCharCallbacks(this));

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(service->getUUID());
  advertising->setManufacturerData(
      std::string(reinterpret_cast<const char *>(ADVERTISING_MFG_DATA), sizeof(ADVERTISING_MFG_DATA)));
  advertising->start();
  return true;
}

// ---------------------------------------------------------------------------
// RX path
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::on_rx_frame_(const std::string &frame) {
  switch (this->session_.process_frame(frame)) {
    case LockSession::Action::SEND_IV:
      this->notify_(this->session_.iv_response(), this->session_.iv_response_size());
      return;
    case LockSession::Action::COMMAND:
      this->handle_command_(this->session_.header(), this->session_.command());
      return;
    case LockSession::Action::NONE:
    default:
      return;  // dropped — the session logged why
  }
}

void SwitchbotKeypadBridge::handle_command_(const FrameHeader &header, const DecodedCommand &command) {
  switch (command.type) {
    case CommandType::LOCK:
      ESP_LOGI(TAG, "Lock");
      this->lock_state_ = LockState::LOCKED;
      this->publish_lock_();
      this->send_encrypted_response_(header, RESPONSE_LOCK, sizeof(RESPONSE_LOCK));
      return;

    case CommandType::UNLOCK:
      ESP_LOGI(TAG, "Unlock: method=%s (0x%02X) index=%d",
               unlock_method_name(command.method),
               static_cast<uint8_t>(command.method), command.credential_index);
      this->lock_state_ = LockState::UNLOCKED;
      this->publish_unlock_(command.method, command.credential_index);
      this->send_encrypted_response_(header, RESPONSE_UNLOCK, sizeof(RESPONSE_UNLOCK));
      return;

    case CommandType::STATE_POLL:
      ESP_LOGV(TAG, "State poll");
      this->handle_state_poll_(header);
      return;

    case CommandType::DOORBELL:
      ESP_LOGI(TAG, "Doorbell");
      this->publish_doorbell_();
      // No lock-state change; the keypad only needs the transport ACK.
      this->send_ack_(header);
      return;

    case CommandType::UNKNOWN:
    default:
      this->send_ack_(header);
      return;
  }
}

void SwitchbotKeypadBridge::handle_state_poll_(const FrameHeader &header) {
  uint8_t state_payload[1 + sizeof(STATE_PAYLOAD_TAIL)];
  state_payload[0] = static_cast<uint8_t>(this->lock_state_);
  std::memcpy(state_payload + 1, STATE_PAYLOAD_TAIL, sizeof(STATE_PAYLOAD_TAIL));
  this->send_encrypted_response_(header, state_payload, sizeof(state_payload));
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::send_ack_(const FrameHeader &header) {
  const uint8_t ack[LockSession::HEADER_LEN] = {0x01, header.key_id, header.seq_a, header.seq_b};
  this->notify_(ack, sizeof(ack));
}

void SwitchbotKeypadBridge::send_encrypted_response_(const FrameHeader &header,
                                                    const uint8_t *plaintext, size_t length) {
  uint8_t packet[LockSession::MAX_PACKET];
  const size_t n = this->session_.encrypt_response(header, plaintext, length, packet);
  if (n == 0) {
    return;  // error already logged
  }
  this->notify_(packet, n);
}

void SwitchbotKeypadBridge::notify_(const uint8_t *data, size_t length) {
  this->tx_characteristic_->setValue(data, length);
  this->tx_characteristic_->notify();
}

// ---------------------------------------------------------------------------
// Eventing
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::publish_lock_() {
  if (this->keypad_event_ != nullptr) {
    this->keypad_event_->trigger("Lock");
  }
  this->log_event_(EventType::LOCK, UnlockMethod::UNKNOWN, -1, "");
  this->on_lock_callbacks_.call();
}

size_t SwitchbotKeypadBridge::method_slot_(UnlockMethod method) {
  switch (method) {
    case UnlockMethod::PIN:         return 0;
    case UnlockMethod::NFC:         return 1;
    case UnlockMethod::FINGERPRINT: return 2;
    case UnlockMethod::FACE:        return 3;
    default:                        return 4;  // UNKNOWN
  }
}

void SwitchbotKeypadBridge::set_unlock_count_sensor(uint8_t method, sensor::Sensor *s) {
  this->unlock_count_sensors_[method_slot_(static_cast<UnlockMethod>(method))] = s;
}

std::string SwitchbotKeypadBridge::lookup_user_(UnlockMethod method, int index) const {
  const uint8_t m = static_cast<uint8_t>(method);
  // Prefer the most specific match: exact (method, index) beats a
  // method-with-any-slot rule, which beats an any-method rule. Two sources
  // feed this — the web-managed list and the YAML list — and web entries win
  // ties so a live edit takes precedence over a compile-time default.
  int best_score = -1;
  std::string best_name;
  auto scan = [&](const std::vector<UserEntry> &list, bool win_ties) {
    for (const auto &u : list) {
      const bool method_ok = (u.method == 0xFF) || (u.method == m);
      const bool index_ok = (u.index < 0) || (u.index == index);
      if (!method_ok || !index_ok) continue;
      const int score = (u.method != 0xFF ? 2 : 0) + (u.index >= 0 ? 1 : 0);
      if (score > best_score || (win_ties && score == best_score)) {
        best_score = score;
        best_name = u.name;
      }
    }
  };
  {
    std::lock_guard<std::mutex> lk(this->web_users_mutex_);
    scan(this->web_users_, /*win_ties=*/true);
  }
  scan(this->users_, /*win_ties=*/false);
  return best_name;
}

// ---------------------------------------------------------------------------
// Web-managed user names (editable in the UI, persisted to NVS)
// ---------------------------------------------------------------------------

static const char *web_method_str(uint8_t m) {
  switch (m) {
    case 0x04: return "pin";
    case 0x08: return "nfc";
    case 0x0C: return "fingerprint";
    case 0x18: return "face";
    case 0xFF: return "any";
    default:   return "unknown";
  }
}

static uint8_t web_method_byte(const char *s) {
  if (s == nullptr) return 0xFF;
  if (std::strcmp(s, "pin") == 0) return 0x04;
  if (std::strcmp(s, "nfc") == 0) return 0x08;
  if (std::strcmp(s, "fingerprint") == 0) return 0x0C;
  if (std::strcmp(s, "face") == 0) return 0x18;
  if (std::strcmp(s, "unknown") == 0) return 0x00;
  return 0xFF;  // "any" or unrecognised
}

std::string SwitchbotKeypadBridge::web_users_json_() {
  cJSON *arr = cJSON_CreateArray();
  {
    std::lock_guard<std::mutex> lk(this->web_users_mutex_);
    for (const auto &u : this->web_users_) {
      cJSON *o = cJSON_CreateObject();
      cJSON_AddStringToObject(o, "method", web_method_str(u.method));
      cJSON_AddNumberToObject(o, "index", u.index);
      cJSON_AddStringToObject(o, "name", u.name.c_str());
      cJSON_AddItemToArray(arr, o);
    }
  }
  std::string out = "[]";
  char *s = cJSON_PrintUnformatted(arr);
  if (s != nullptr) {
    out = s;
    cJSON_free(s);
  }
  cJSON_Delete(arr);
  return out;
}

bool SwitchbotKeypadBridge::set_web_users_json_(const std::string &json) {
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  if (root == nullptr) return false;
  // Accept either a bare array or {"users": [...]}.
  cJSON *arr =
      cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "users");
  if (!cJSON_IsArray(arr)) {
    cJSON_Delete(root);
    return false;
  }
  std::vector<UserEntry> parsed;
  cJSON *it = nullptr;
  cJSON_ArrayForEach(it, arr) {
    if (!cJSON_IsObject(it)) continue;
    cJSON *jn = cJSON_GetObjectItemCaseSensitive(it, "name");
    if (!cJSON_IsString(jn) || jn->valuestring == nullptr) continue;
    std::string name = jn->valuestring;
    if (name.empty()) continue;  // skip blank rows
    if (name.size() >= WEB_USER_NAME_MAX) name.resize(WEB_USER_NAME_MAX - 1);
    cJSON *jm = cJSON_GetObjectItemCaseSensitive(it, "method");
    cJSON *ji = cJSON_GetObjectItemCaseSensitive(it, "index");
    const uint8_t method = web_method_byte(cJSON_IsString(jm) ? jm->valuestring : "any");
    const int index = cJSON_IsNumber(ji) ? ji->valueint : -1;
    parsed.push_back({method, static_cast<int16_t>(index), std::move(name)});
    if (parsed.size() >= WEB_USERS_MAX) break;
  }
  cJSON_Delete(root);

  const size_t n = parsed.size();
  {
    std::lock_guard<std::mutex> lk(this->web_users_mutex_);
    this->web_users_ = std::move(parsed);
  }
  this->web_users_dirty_.store(true, std::memory_order_release);
  ESP_LOGI(TAG, "Web users updated: %u entries, persisting", static_cast<unsigned>(n));
  return true;
}

void SwitchbotKeypadBridge::load_web_users_() {
  this->web_users_pref_ =
      global_preferences->make_preference<WebUsersBlob>(0x53574255UL /* 'SWBU' */);
  WebUsersBlob blob{};
  if (!this->web_users_pref_.load(&blob) || blob.version != 1) return;
  const uint8_t n = blob.count <= WEB_USERS_MAX ? blob.count : WEB_USERS_MAX;
  std::lock_guard<std::mutex> lk(this->web_users_mutex_);
  this->web_users_.clear();
  for (uint8_t i = 0; i < n; ++i) {
    const WebUserRec &r = blob.entries[i];
    char nm[WEB_USER_NAME_MAX];
    std::memcpy(nm, r.name, WEB_USER_NAME_MAX);
    nm[WEB_USER_NAME_MAX - 1] = '\0';
    this->web_users_.push_back({r.method, r.index, std::string(nm)});
  }
  ESP_LOGI(TAG, "Loaded %u web user(s) from NVS", static_cast<unsigned>(n));
}

void SwitchbotKeypadBridge::save_web_users_() {
  WebUsersBlob blob{};
  blob.version = 1;
  {
    std::lock_guard<std::mutex> lk(this->web_users_mutex_);
    const size_t n =
        this->web_users_.size() > WEB_USERS_MAX ? WEB_USERS_MAX : this->web_users_.size();
    blob.count = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; ++i) {
      const UserEntry &u = this->web_users_[i];
      blob.entries[i].method = u.method;
      blob.entries[i].index = u.index;
      std::strncpy(blob.entries[i].name, u.name.c_str(), WEB_USER_NAME_MAX - 1);
      blob.entries[i].name[WEB_USER_NAME_MAX - 1] = '\0';
    }
  }
  this->web_users_pref_.save(&blob);
  global_preferences->sync();
}

// ---------------------------------------------------------------------------
// Web-managed settings (editable in the UI, persisted to NVS)
// ---------------------------------------------------------------------------

std::string SwitchbotKeypadBridge::web_settings_json_() {
  cJSON *o = cJSON_CreateObject();
  cJSON_AddNumberToObject(
      o, "battery_scan_interval_s",
      static_cast<double>(this->battery_scan_interval_ms_.load() / 1000));
  cJSON_AddNumberToObject(
      o, "min_unlock_interval_s",
      static_cast<double>(this->min_unlock_interval_ms_.load() / 1000));
  cJSON_AddBoolToObject(o, "scan_enabled", this->scan_enabled_.load());
  std::string out = "{}";
  char *s = cJSON_PrintUnformatted(o);
  if (s != nullptr) {
    out = s;
    cJSON_free(s);
  }
  cJSON_Delete(o);
  return out;
}

bool SwitchbotKeypadBridge::set_web_settings_json_(const std::string &json) {
  cJSON *root = cJSON_ParseWithLength(json.data(), json.size());
  if (root == nullptr) return false;
  bool changed = false;

  cJSON *bi = cJSON_GetObjectItemCaseSensitive(root, "battery_scan_interval_s");
  if (cJSON_IsNumber(bi)) {
    long secs = static_cast<long>(bi->valuedouble);
    if (secs < 30) secs = 30;         // same floor as the YAML validator
    if (secs > 86400) secs = 86400;   // cap at 24h
    this->battery_scan_interval_ms_.store(static_cast<uint32_t>(secs) * 1000UL);
    changed = true;
  }

  cJSON *mu = cJSON_GetObjectItemCaseSensitive(root, "min_unlock_interval_s");
  if (cJSON_IsNumber(mu)) {
    long secs = static_cast<long>(mu->valuedouble);
    if (secs < 0) secs = 0;           // 0 = debounce off
    if (secs > 60) secs = 60;         // a debounce beyond a minute makes no sense
    this->min_unlock_interval_ms_.store(static_cast<uint32_t>(secs) * 1000UL);
    changed = true;
  }

  cJSON *se = cJSON_GetObjectItemCaseSensitive(root, "scan_enabled");
  if (cJSON_IsBool(se)) {
    this->scan_enabled_.store(cJSON_IsTrue(se));
    changed = true;
  }

  cJSON_Delete(root);
  if (changed) {
    this->settings_dirty_.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Settings updated: battery=%us min_unlock=%us scan=%s",
             static_cast<unsigned>(this->battery_scan_interval_ms_.load() / 1000),
             static_cast<unsigned>(this->min_unlock_interval_ms_.load() / 1000),
             this->scan_enabled_.load() ? "on" : "off");
  }
  return changed;
}

void SwitchbotKeypadBridge::load_settings_() {
  this->settings_pref_ =
      global_preferences->make_preference<SettingsBlob>(0x53574753UL /* 'SWGS' */);
  SettingsBlob blob{};
  // A stored value overrides the YAML defaults (the user edited it live).
  if (!this->settings_pref_.load(&blob) || blob.version != 2) return;
  if (blob.battery_scan_interval_ms >= 30000) {
    this->battery_scan_interval_ms_.store(blob.battery_scan_interval_ms);
  }
  if (blob.min_unlock_interval_ms <= 60000) {
    this->min_unlock_interval_ms_.store(blob.min_unlock_interval_ms);
  }
  this->scan_enabled_.store(blob.scan_enabled != 0);
  ESP_LOGI(TAG, "Loaded settings from NVS: battery=%us min_unlock=%us scan=%s",
           static_cast<unsigned>(this->battery_scan_interval_ms_.load() / 1000),
           static_cast<unsigned>(this->min_unlock_interval_ms_.load() / 1000),
           this->scan_enabled_.load() ? "on" : "off");
}

void SwitchbotKeypadBridge::save_settings_() {
  SettingsBlob blob{};
  blob.version = 2;
  blob.battery_scan_interval_ms = this->battery_scan_interval_ms_.load();
  blob.min_unlock_interval_ms = this->min_unlock_interval_ms_.load();
  blob.scan_enabled = this->scan_enabled_.load() ? 1 : 0;
  this->settings_pref_.save(&blob);
  global_preferences->sync();
}

void SwitchbotKeypadBridge::publish_unlock_(UnlockMethod method, int index) {
  // Same-credential debounce: drop a repeat of the last unlock that lands
  // inside the configured window. The keypad's BLE ACK is still sent by the
  // caller; only the Home Assistant-facing event is suppressed.
  if (this->min_unlock_interval_ms_ > 0) {
    const uint32_t now = millis();
    if (this->last_unlock_index_ == index && this->last_unlock_method_ == method &&
        (now - this->last_unlock_ms_) < this->min_unlock_interval_ms_) {
      ESP_LOGD(TAG, "Unlock debounced (same credential within %u ms)",
               static_cast<unsigned>(this->min_unlock_interval_ms_));
      this->last_unlock_ms_ = now;
      return;
    }
    this->last_unlock_ms_ = now;
  }
  this->last_unlock_method_ = method;
  this->last_unlock_index_ = index;

  const char *method_str = unlock_method_name(method);
  const std::string name = this->lookup_user_(method, index);

  if (this->last_method_sensor_ != nullptr) {
    this->last_method_sensor_->publish_state(method_str);
  }
  if (this->last_user_sensor_ != nullptr) {
    // Fall back to a readable "<method> #<index>" for an unmapped slot.
    std::string label = name;
    if (label.empty()) {
      char buf[32];
      snprintf(buf, sizeof(buf), "%s #%d", method_str, index);
      label = buf;
    }
    this->last_user_sensor_->publish_state(label);
  }

  const size_t slot = method_slot_(method);
  this->unlock_counts_[slot]++;
  if (this->unlock_count_sensors_[slot] != nullptr) {
    this->unlock_count_sensors_[slot]->publish_state(
        static_cast<float>(this->unlock_counts_[slot]));
  }

  this->log_event_(EventType::UNLOCK, method, index, name);

  if (this->keypad_event_ != nullptr) {
    this->keypad_event_->trigger("Unlock");
  }
  this->on_unlock_callbacks_.call(std::string(method_str), index, name);
}

void SwitchbotKeypadBridge::publish_doorbell_() {
  if (this->keypad_event_ != nullptr) {
    this->keypad_event_->trigger("Doorbell");
  }
  this->log_event_(EventType::DOORBELL, UnlockMethod::UNKNOWN, -1, "");
  this->on_doorbell_callbacks_.call();
}

// ---------------------------------------------------------------------------
// On-device event log
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::log_event_(EventType type, UnlockMethod method, int index,
                                       const std::string &name) {
  LogEvent ev{};
  ev.ts = static_cast<uint32_t>(::time(nullptr));  // 0-ish until SNTP syncs
  ev.up_ms = millis();
  ev.type = type;
  ev.method = method;
  ev.index = static_cast<int16_t>(index);
  ev.name = name;

  std::lock_guard<std::mutex> lk(this->event_log_mutex_);
  this->event_log_.push_back(std::move(ev));
  while (this->event_log_.size() > EVENT_LOG_MAX) {
    this->event_log_.pop_front();
  }
}

std::string SwitchbotKeypadBridge::events_json_() {
  const uint32_t now_ms = millis();
  cJSON *arr = cJSON_CreateArray();
  {
    std::lock_guard<std::mutex> lk(this->event_log_mutex_);
    // Newest first — friendlier for a log view.
    for (auto it = this->event_log_.rbegin(); it != this->event_log_.rend(); ++it) {
      const LogEvent &e = *it;
      cJSON *o = cJSON_CreateObject();
      const char *type = e.type == EventType::LOCK       ? "lock"
                         : e.type == EventType::UNLOCK   ? "unlock"
                                                         : "doorbell";
      cJSON_AddStringToObject(o, "type", type);
      cJSON_AddNumberToObject(o, "ts", static_cast<double>(e.ts));
      // Milliseconds since the event, computed now — lets the UI show a
      // relative "N ago" even when the clock was never SNTP-synced.
      cJSON_AddNumberToObject(o, "ago", static_cast<double>(now_ms - e.up_ms));
      if (e.type == EventType::UNLOCK) {
        cJSON_AddStringToObject(o, "method", unlock_method_name(e.method));
        cJSON_AddNumberToObject(o, "index", e.index);
        cJSON_AddStringToObject(o, "name", e.name.c_str());
      }
      cJSON_AddItemToArray(arr, o);
    }
  }
  std::string out = "[]";
  char *s = cJSON_PrintUnformatted(arr);
  if (s != nullptr) {
    out = s;
    cJSON_free(s);
  }
  cJSON_Delete(arr);
  return out;
}

// ---------------------------------------------------------------------------
// Keypad battery (advertisement scan)
// ---------------------------------------------------------------------------

void SwitchbotKeypadBridge::maybe_start_battery_scan_() {
  const uint32_t now = millis();

  if (this->battery_scan_active_) {
    // NimBLE drops isScanning() when the window elapses or stop() ran.
    if (!NimBLEDevice::getScan()->isScanning()) {
      this->battery_scan_active_ = false;
      NimBLEDevice::getScan()->clearResults();
      // Alarm sensors use their own (shorter) cadence so tamper/duress surface
      // promptly; battery-only setups keep the slow battery interval. Neither
      // scans continuously, so the radio isn't pinned on.
      const uint32_t interval = this->has_alarm_scan_
                                    ? this->alarm_scan_interval_ms_
                                    : this->battery_scan_interval_ms_.load();
      this->next_battery_scan_at_ = now + interval;
    }
    return;
  }

  // Scanning can be turned off entirely from the web Settings tab to save power.
  if (!this->scan_enabled_.load(std::memory_order_relaxed)) {
    return;
  }

  if (static_cast<int32_t>(now - this->next_battery_scan_at_) < 0) {
    return;
  }

  // The scan singleton is shared with the pairing flows (which run blocking
  // scans), and the keypad does not advertise while it holds a connection to
  // us — defer rather than fight either. The server now runs permanently, so
  // gate on an *active* pairing scan rather than merely "server up".
  if (!this->keypad_paired_ || this->pairing_ui_.is_ble_scan_busy() ||
      (this->server_ != nullptr && this->server_->getConnectedCount() > 0) ||
      NimBLEDevice::getScan()->isScanning()) {
    this->next_battery_scan_at_ = now + BATTERY_SCAN_RETRY_MS;
    return;
  }

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(this->battery_scan_callbacks_, false);
  configure_switchbot_scan(scan);
  if (scan->start(BATTERY_SCAN_DURATION_MS, false, true)) {
    this->battery_scan_active_ = true;
    ESP_LOGD(TAG, "Battery scan window opened (%u ms)",
             static_cast<unsigned>(BATTERY_SCAN_DURATION_MS));
  } else {
    ESP_LOGW(TAG, "Battery scan failed to start");
    this->next_battery_scan_at_ = now + BATTERY_SCAN_RETRY_MS;
  }
}

void SwitchbotKeypadBridge::handle_battery_advert_(const NimBLEAdvertisedDevice *adv) {
  // The callbacks stay registered on the shared scan singleton, so pairing
  // scans fire them too — only act inside our own scan window.
  if (!this->battery_scan_active_.load(std::memory_order_relaxed)) {
    return;
  }

  const std::array<uint8_t, 6> mac = addr_bytes(adv->getAddress());
  const std::vector<uint8_t> sd = switchbot_service_data(adv);

  KeypadFamily family;
  if (this->keypad_info_.valid != 0) {
    if (std::memcmp(mac.data(), this->keypad_info_.mac, mac.size()) != 0) return;
    family = static_cast<KeypadFamily>(this->keypad_info_.family);
  } else {
    // Keypad paired before the MAC was persisted: adopt the first advert
    // matching a known keypad signature (loop() persists it).
    const KeypadIdent ident = identify_keypad(sd.data(), sd.size());
    if (!ident.is_keypad) return;
    family = ident.family;
  }

  std::string mfr;
  if (adv->haveManufacturerData()) {
    mfr = adv->getManufacturerData();
  }

  // Diagnostic: dump the raw advert of *our* keypad so an unknown field (e.g.
  // which byte carries PIR/motion on a given unit) can be found by diffing the
  // idle vs. active capture. DEBUG-only.
  {
    static const char hx[] = "0123456789abcdef";
    std::string sh, mh;
    for (uint8_t b : sd) { sh.push_back(hx[b >> 4]); sh.push_back(hx[b & 0xF]); }
    for (unsigned char b : mfr) { mh.push_back(hx[b >> 4]); mh.push_back(hx[b & 0xF]); }
    ESP_LOGD(TAG, "advert raw: svc=%s mfr=%s", sh.c_str(), mh.c_str());
  }

  const KeypadStatus status = parse_keypad_status(
      family, sd.data(), sd.size(),
      reinterpret_cast<const uint8_t *>(mfr.data()), mfr.size());
  if (!status.valid) {
    return;
  }

  std::lock_guard<std::mutex> lk(this->rx_mutex_);
  this->battery_advert_status_ = status;
  this->battery_advert_rssi_ = adv->getRSSI();
  std::memcpy(this->battery_advert_mac_, mac.data(), mac.size());
  this->battery_advert_family_ = static_cast<uint8_t>(family);
  this->battery_advert_pending_ = true;
}

void SwitchbotKeypadBridge::publish_last_seen_() {
  if (this->last_seen_sensor_ == nullptr) return;
  const time_t now = ::time(nullptr);
  if (now < 1600000000) return;  // clock not SNTP-synced yet — skip
  struct tm tm_utc;
  gmtime_r(&now, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S+00:00", &tm_utc);
  this->last_seen_sensor_->publish_state(buf);
}

void SwitchbotKeypadBridge::apply_pending_battery_() {
  KeypadStatus status;
  int rssi;
  uint8_t mac[6];
  uint8_t family;
  {
    std::lock_guard<std::mutex> lk(this->rx_mutex_);
    if (!this->battery_advert_pending_) return;
    this->battery_advert_pending_ = false;
    status = this->battery_advert_status_;
    rssi = this->battery_advert_rssi_;
    std::memcpy(mac, this->battery_advert_mac_, sizeof(mac));
    family = this->battery_advert_family_;
  }

  // Hearing the advert is proof of life: refresh RSSI + last-seen.
  if (this->rssi_sensor_ != nullptr) {
    this->rssi_sensor_->publish_state(static_cast<float>(rssi));
  }
  this->publish_last_seen_();

  if (this->keypad_info_.valid == 0) {
    std::memcpy(this->keypad_info_.mac, mac, sizeof(this->keypad_info_.mac));
    this->keypad_info_.family = family;
    this->keypad_info_.valid = 1;
    this->keypad_info_pref_.save(&this->keypad_info_);
    global_preferences->sync();
    ESP_LOGI(TAG, "Learned keypad MAC %02X:%02X:%02X:%02X:%02X:%02X from advertisement",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  // Decoded status (watch pir here to calibrate the motion threshold).
  ESP_LOGD(TAG,
           "Keypad status: batt=%d%% chg=%d tamper=%d duress=%d lockout=%d pir=%u",
           status.battery, status.charging, status.tamper, status.duress,
           status.lockout, static_cast<unsigned>(status.pir_level));

  // Battery.
  if (this->battery_level_sensor_ != nullptr && status.battery >= 0 &&
      status.battery != this->last_battery_) {
    this->last_battery_ = status.battery;
    this->battery_level_sensor_->publish_state(static_cast<float>(status.battery));
  }

  // Status / alarm flags (Vision advert; all false for the Original family).
  if (this->charging_sensor_ != nullptr) this->charging_sensor_->publish_state(status.charging);
  if (this->lockout_sensor_ != nullptr) this->lockout_sensor_->publish_state(status.lockout);
  if (this->motion_sensor_ != nullptr) this->motion_sensor_->publish_state(status.motion);
  if (this->tamper_sensor_ != nullptr) this->tamper_sensor_->publish_state(status.tamper);
  if (this->duress_sensor_ != nullptr) this->duress_sensor_->publish_state(status.duress);

  // Fire the alarm triggers on the rising edge only (not every advert while
  // the flag stays set).
  if (status.tamper && !this->last_tamper_) {
    ESP_LOGW(TAG, "Tamper alarm — keypad moved/removed");
    this->on_tamper_callbacks_.call();
  }
  if (status.duress && !this->last_duress_) {
    ESP_LOGW(TAG, "Duress alarm — duress code entered");
    this->on_duress_callbacks_.call();
  }
  this->last_tamper_ = status.tamper;
  this->last_duress_ = status.duress;

  // Stop early once we've read an advert — saves the rest of the 5 s window;
  // the next scan reopens after the interval. Keeps the radio off in between.
  if (this->battery_scan_active_ && NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->stop();
  }
}

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
