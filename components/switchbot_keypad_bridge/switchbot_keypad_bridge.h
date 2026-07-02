#pragma once

#include <psa/crypto.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <mutex>
#include <vector>

#include "esphome/components/button/button.h"
#include "esphome/components/event/event.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"

#include "lock_protocol.h"
#include "lock_session.h"
#include "nimble_compat.h"
#include "pairing_ui.h"

namespace esphome {
namespace switchbot_keypad_bridge {

// Upper bound (including the trailing NUL) on the persisted keypad name.
constexpr size_t KEYPAD_NAME_MAX = 48;

// The protocol layers live next door: lock_protocol.h decodes plaintext
// frames, lock_session.h owns the encrypted-session state machine (IV,
// anti-replay, transport crypto). This component is the NimBLE transport
// and the ESPHome-facing business logic on top of them.

// ── Concurrency model ───────────────────────────────────────────────────────
// Four execution contexts touch this component. The rule of thumb: only the
// main task acts; every other context hands data over and returns.
//
//   1. ESPHome main task — setup()/loop() and everything they call. The only
//      context that publishes entities, writes NVS, or mutates session and
//      battery-scan state.
//   2. NimBLE host task — server/characteristic callbacks and the battery
//      scan callback. They only enqueue: connect/disconnect/RX frames into
//      rx_queue_, battery adverts into the battery_advert_* fields — both
//      under rx_mutex_, both drained by loop().
//   3. HTTP-server task (pairing wizard) — signals a finished pairing through
//      the pending_keypad_* fields plus the pending_pair_apply_ flag. The
//      fields are written first, then the flag is stored with release
//      semantics; loop() exchanges the flag with acquire before reading them.
//      When adding a pending field, write it BEFORE the flag store.
//   4. Pairing FreeRTOS task ("kp-pair") — owned by KeypadPairer, which
//      exposes progress as Status snapshots copied under its own mutex.
class SwitchbotKeypadBridge : public Component {
  SUB_TEXT_SENSOR(keypad)
  SUB_SENSOR(battery_level)

 public:
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void set_keypad_event(event::Event *ev) { this->keypad_event_ = ev; }
  void set_pairing_ui_html(const uint8_t *html, size_t len) {
    this->pairing_ui_.set_html(html, len);
  }
  void set_battery_scan_interval(uint32_t ms) { this->battery_scan_interval_ms_ = ms; }

  // ----- Credential labelling ("who unlocked") -------------------------------

  // Optional diagnostic entities describing the most recent unlock: the
  // resolved user name and the method used.
  void set_last_user_text_sensor(text_sensor::TextSensor *s) { this->last_user_sensor_ = s; }
  void set_last_method_text_sensor(text_sensor::TextSensor *s) { this->last_method_sensor_ = s; }

  // Per-method running unlock counters (since boot). Any subset may be wired.
  // `method` is the UnlockMethod byte (as sent by the keypad).
  void set_unlock_count_sensor(uint8_t method, sensor::Sensor *s);

  // Register a (method, index) -> display name mapping from YAML. `method`
  // is the UnlockMethod byte, or 0xFF to match any method; `index` is the
  // credential slot, or -1 to match any slot of that method.
  void add_user(uint8_t method, int index, const std::string &name) {
    this->users_.push_back({method, static_cast<int16_t>(index), name});
  }

  // Debounce repeated unlocks from the same credential arriving within this
  // window. 0 disables the throttle (every unlock fires an event).
  void set_min_unlock_interval(uint32_t ms) { this->min_unlock_interval_ms_ = ms; }

  // HTTP Basic Auth for the always-on web console. An empty password leaves
  // the server open (original behaviour); set one to require a login.
  void set_web_credentials(const std::string &user, const std::string &pass) {
    this->web_user_ = user;
    this->web_pass_ = pass;
  }

  // "A pairing is mid-flight" — used by the Unpair button to refuse while a
  // job runs. Not "is the server up" (it always is now).
  bool is_pairing_active() const { return this->pairing_ui_.is_pairing_busy(); }

  // Forgets the paired keypad, rotates the shared key in place and
  // re-opens the pairing wizard — no reboot. Invoked by UnpairButton.
  void unpair();

  void add_on_lock_callback(std::function<void()> &&callback) {
    this->on_lock_callbacks_.add(std::move(callback));
  }
  void add_on_unlock_callback(std::function<void(std::string, int, std::string)> &&callback) {
    this->on_unlock_callbacks_.add(std::move(callback));
  }
  void add_on_doorbell_callback(std::function<void()> &&callback) {
    this->on_doorbell_callbacks_.add(std::move(callback));
  }

 protected:
  class ServerCallbacks;
  class RxCharCallbacks;
  class BatteryScanCallbacks;
  friend class ServerCallbacks;
  friend class RxCharCallbacks;
  friend class BatteryScanCallbacks;

  enum class LockState : uint8_t {
    LOCKED = 0x81,
    UNLOCKED = 0x91,
  };

  // Identity of the paired keypad, persisted to NVS at pairing time so the
  // battery scan can match its advertisement after a reboot. `valid == 0`
  // for keypads paired before this field existed — the scan then learns the
  // MAC from the first recognised keypad advert and persists it.
  struct KeypadInfo {
    uint8_t mac[6]{};   // big-endian, as printed
    uint8_t family{0};  // KeypadFamily
    uint8_t valid{0};
  };

  // ----- Configuration / setup -----------------------------------------------

  // Generates a fresh AES-128 session key into shared_key_ and persists it
  // to NVS — the one place keys are created (first boot and unpair()).
  void create_shared_key_();
  // (Re-)imports shared_key_ into the PSA crypto slot, replacing any handle
  // imported earlier. Lets unpair() re-key the live session without a reboot.
  bool import_aes_key_();

  bool prepare_keys_();
  bool prepare_ble_();

  // ----- BLE write handling --------------------------------------------------

  // Validation, decryption and decoding live in LockSession; the bridge
  // dispatches the resulting Action and owns the business logic.
  void on_rx_frame_(const std::string &frame);

  void handle_command_(const FrameHeader &header, const DecodedCommand &command);
  void handle_state_poll_(const FrameHeader &header);

  // ----- Transport helpers ---------------------------------------------------

  void send_ack_(const FrameHeader &header);
  void send_encrypted_response_(const FrameHeader &header, const uint8_t *plaintext, size_t length);
  void notify_(const uint8_t *data, size_t length);

  // ----- Eventing ------------------------------------------------------------

  void publish_lock_();
  void publish_unlock_(UnlockMethod method, int index);
  void publish_doorbell_();

  // Resolve a credential to its configured display name, or "" if unmapped.
  std::string lookup_user_(UnlockMethod method, int index) const;

  // ----- On-device event log -------------------------------------------------

  enum class EventType : uint8_t { LOCK = 0, UNLOCK = 1, DOORBELL = 2 };
  struct LogEvent {
    uint32_t ts;      // epoch seconds; 0 when the clock is not yet SNTP-synced
    uint32_t up_ms;   // millis() at capture, for a "N ago" fallback
    EventType type;
    UnlockMethod method;
    int16_t index;
    std::string name;
  };
  // Append an event to the ring buffer (main task). Serialised as JSON on the
  // HTTP task via events_json_() for GET /api/events.
  void log_event_(EventType type, UnlockMethod method, int index, const std::string &name);
  std::string events_json_();

  // ----- Keypad battery (advertisement scan) ----------------------------------

  // The keypad broadcasts its battery level in its BLE advertisement (the
  // command frames it sends us never carry it). A short active scan picks
  // the advert up while the bridge keeps advertising as the lock.
  void maybe_start_battery_scan_();
  void apply_pending_battery_();
  // Runs on the NimBLE host task — parses the advert and queues the result.
  void handle_battery_advert_(const NimBLEAdvertisedDevice *adv);

  // ----- BLE handles ---------------------------------------------------------

  NimBLEServer *server_{nullptr};
  NimBLECharacteristic *tx_characteristic_{nullptr};

  // ----- Thread-safe event queueing from NimBLE callbacks --------------------

  struct QueuedEvent {
    enum Type { CONNECT, DISCONNECT, RX } type;
    std::string frame;
  };

  std::mutex rx_mutex_;
  std::vector<QueuedEvent> rx_queue_;

  void push_connect_();
  void push_disconnect_();
  void push_rx_(const std::string &frame);

  // ----- On-device pairing wizard --------------------------------------------

  PairingUi pairing_ui_{};

  // A finished pairing is signalled from the HTTP-server task; loop() (the
  // main task) consumes it and publishes the keypad name + writes NVS, so
  // neither runs on the network task. pending_keypad_name_ is written
  // before the flag is set — the release/acquire pair makes it safe to
  // read once the flag is observed.
  std::atomic<bool> pending_pair_apply_{false};
  std::string pending_keypad_name_;
  std::string pending_keypad_mac_;
  KeypadFamily pending_keypad_family_{KeypadFamily::ORIGINAL};

  // ----- ESPHome wiring ------------------------------------------------------

  event::Event *keypad_event_{nullptr};
  CallbackManager<void()> on_lock_callbacks_{};
  CallbackManager<void(std::string, int, std::string)> on_unlock_callbacks_{};
  CallbackManager<void()> on_doorbell_callbacks_{};

  // ----- Credential labelling / unlock stats ---------------------------------

  // (method, index) -> display name mappings from YAML. `method == 0xFF`
  // matches any method; `index < 0` matches any slot of that method.
  struct UserEntry {
    uint8_t method;
    int16_t index;
    std::string name;
  };
  std::vector<UserEntry> users_{};

  text_sensor::TextSensor *last_user_sensor_{nullptr};
  text_sensor::TextSensor *last_method_sensor_{nullptr};

  // Per-method unlock counters (since boot) and their optional sensors, keyed
  // by a small dense index (see method_slot_()).
  static constexpr size_t METHOD_SLOTS = 5;  // pin, nfc, fingerprint, face, unknown
  sensor::Sensor *unlock_count_sensors_[METHOD_SLOTS]{};
  uint32_t unlock_counts_[METHOD_SLOTS]{};
  static size_t method_slot_(UnlockMethod method);

  // Same-credential unlock debounce.
  uint32_t min_unlock_interval_ms_{0};
  uint32_t last_unlock_ms_{0};
  UnlockMethod last_unlock_method_{UnlockMethod::UNKNOWN};
  int last_unlock_index_{-2};  // -2 = nothing seen yet (distinct from a real -1)

  // HTTP Basic Auth credentials for the web console (empty pass = open).
  std::string web_user_{"admin"};
  std::string web_pass_{};

  // Recent lock/unlock/doorbell events, newest at the back. Written by the
  // main task, read (serialised) by the HTTP task, so guarded by its mutex.
  static constexpr size_t EVENT_LOG_MAX = 32;
  std::deque<LogEvent> event_log_;
  std::mutex event_log_mutex_;

  // ----- User configuration --------------------------------------------------

  // 16-byte AES-128 session key. Generated on first boot, persisted in
  // NVS, rotated by unpair(). Never part of the YAML config.
  std::array<uint8_t, 16> shared_key_{};
  ESPPreferenceObject shared_key_pref_;
  ESPPreferenceObject keypad_name_pref_;

  // ----- Runtime state -------------------------------------------------------

  // PSA AES-CTR key handle. Imported once at setup so the per-frame crypto
  // path does not pay the cost (or risk the failure) of re-importing it.
  psa_key_id_t aes_key_handle_{PSA_KEY_ID_NULL};
  LockState lock_state_{LockState::LOCKED};

  // Per-connection encrypted-session state (token slot, IV, anti-replay,
  // transport crypto). Only ever touched from the main task.
  LockSession session_{};

  // ----- Keypad battery state --------------------------------------------------

  ESPPreferenceObject keypad_info_pref_;
  KeypadInfo keypad_info_{};
  bool keypad_paired_{false};

  uint32_t battery_scan_interval_ms_{15 * 60 * 1000};
  uint32_t next_battery_scan_at_{0};  // millis() deadline for the next scan
  // Written by loop(), read by the NimBLE scan callback as its "is this our
  // scan window?" gate — hence atomic.
  std::atomic<bool> battery_scan_active_{false};
  BatteryScanCallbacks *battery_scan_callbacks_{nullptr};
  int last_battery_{-1};  // last published value; -1 = nothing published yet

  // Latest advert parsed by the NimBLE scan callback, handed to loop()
  // under rx_mutex_ (same pattern as the RX queue).
  bool battery_advert_pending_{false};
  int battery_advert_value_{-1};
  uint8_t battery_advert_mac_[6]{};
  uint8_t battery_advert_family_{0};
};

// Home Assistant button that unpairs the keypad: forgets it, rotates the
// shared key and re-opens the pairing wizard — all without a reboot.
class UnpairButton : public button::Button, public Parented<SwitchbotKeypadBridge> {
 protected:
  void press_action() override;
};

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
