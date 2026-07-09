#pragma once

// Lightweight HTTP server that hosts the on-device pairing wizard.
//
// Endpoints (all behind HTTP Basic Auth when a web password is set):
//   GET  /                       → embedded HTML (single self-contained page)
//   POST /api/login              → {email,password} → {region} | 401
//   GET  /api/keypads            → [ {mac, name, model, online, rssi} ]
//   POST /api/pair               → {mac} → {job_id, labels: [step names]}
//   GET  /api/pair/status        → {step, total, message, done, error}
//   GET  /api/events             → [ {type, ts, ago, method, index, name} ]
//   GET  /api/users              → [ {method, index, name} ]
//   POST /api/users              → {ok} | 400   (body: [ {method, index, name} ])
//
// The server uses ESP-IDF's `esp_http_server` (already pulled in by NimBLE
// and the ESP-IDF framework — no extra managed components needed).
// It binds to port 80 by default; if ESPHome's `web_server:` is also
// enabled the user must move one of the two onto a different port.

#include <esp_http_server.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "cloud_client.h"
#include "keypad_advert.h"
#include "keypad_pairer.h"

namespace esphome {
namespace switchbot_keypad_bridge {

class PairingUi {
 public:
  // start() boots the HTTP server and registers all URI handlers. Returns
  // false on init failure (port conflict, OOM, ...). Idempotent: calling
  // it again on an already-running server is a no-op.
  bool start(uint16_t port = 80);

  // stop() releases the server handle. Safe to call when not started.
  void stop();

  // The bridge's 16-byte AES session key — injected into the keypad's lock
  // slot during pairing.
  void set_shared_key(const std::array<uint8_t, 16> &key) { this->shared_key_ = key; }

  // The embedded UI page, gzip-compressed and baked into flash by codegen
  // (see __init__.py); served verbatim with Content-Encoding: gzip.
  // Not NUL-terminated, so the length is carried alongside the pointer.
  void set_html(const uint8_t *html, size_t len) {
    this->html_ = html;
    this->html_len_ = len;
  }

  // Called once after a successful pairing with the keypad's display name,
  // pretty MAC and protocol family (the latter two feed the battery scan).
  using OnPairedCallback = std::function<void(
      const std::string &name, const std::string &mac, KeypadFamily family)>;
  void set_on_paired_callback(OnPairedCallback cb) {
    this->on_paired_cb_ = std::move(cb);
  }

  // Supplies the JSON array served by GET /api/events (the component's on-device
  // event log). Invoked on the HTTP-server task, so the component serialises
  // its ring buffer under its own lock.
  void set_events_provider(std::function<std::string()> cb) {
    this->events_provider_ = std::move(cb);
  }

  // GET /api/users returns the provider's JSON; POST /api/users hands the raw
  // body to the handler, which parses + persists and returns true on success.
  void set_users_get_provider(std::function<std::string()> cb) {
    this->users_get_provider_ = std::move(cb);
  }
  void set_users_set_handler(std::function<bool(const std::string &)> cb) {
    this->users_set_handler_ = std::move(cb);
  }

  bool is_running() const { return this->server_ != nullptr; }

  // Optional HTTP Basic Auth for the whole server. When a password is set,
  // every endpoint (including the UI page) requires it. Empty password (the
  // default) leaves the server open, preserving the original behaviour.
  void set_credentials(const std::string &user, const std::string &pass) {
    this->auth_user_ = user;
    this->auth_pass_ = pass;
  }

  // True while a blocking BLE scan started by the pairing flow is in progress.
  // The component's battery scan checks this to avoid fighting over the shared
  // NimBLE scan singleton now that the server stays up permanently.
  bool is_ble_scan_busy() const { return this->ble_scan_busy_.load(std::memory_order_relaxed); }

  // True while a pairing operation is actually under way — either listing
  // keypads (BLE scan) or running the pairing job. Distinct from is_running():
  // the server itself is now always up, so this is what the Unpair guard uses.
  bool is_pairing_busy() const {
    return this->ble_scan_busy_.load(std::memory_order_relaxed) ||
           this->pairer_.status().state == KeypadPairer::State::RUNNING;
  }

 private:
  // Returns true if the request may proceed. When auth is configured and the
  // request lacks valid Basic credentials, it sends a 401 challenge and
  // returns false — the caller must then return ESP_OK immediately.
  static bool require_auth_(httpd_req_t *req);
  // URI handler trampolines — esp_http_server takes a C function, so the
  // handlers are static and forward to the instance stored in
  // req->user_ctx (set to `this` at registration time).
  static esp_err_t handle_root_(httpd_req_t *req);
  static esp_err_t handle_login_(httpd_req_t *req);
  static esp_err_t handle_keypads_(httpd_req_t *req);
  static esp_err_t handle_pair_(httpd_req_t *req);
  static esp_err_t handle_pair_status_(httpd_req_t *req);
  static esp_err_t handle_events_(httpd_req_t *req);
  static esp_err_t handle_users_get_(httpd_req_t *req);
  static esp_err_t handle_users_set_(httpd_req_t *req);

  static esp_err_t reply_json_(httpd_req_t *req, const char *json,
                               const char *status = "200 OK");
  static esp_err_t reply_error_(httpd_req_t *req, const char *status,
                                const std::string &message);
  static std::string read_body_(httpd_req_t *req);

  httpd_handle_t server_{nullptr};
  CloudClient    cloud_{};
  KeypadPairer   pairer_{};
  std::string    auth_user_{"admin"};
  std::string    auth_pass_{};  // empty = no auth
  std::atomic<bool> ble_scan_busy_{false};
  std::array<uint8_t, 16> shared_key_{};
  const uint8_t *html_{nullptr};
  size_t         html_len_{0};
  OnPairedCallback on_paired_cb_;
  std::function<std::string()> events_provider_;
  std::function<std::string()> users_get_provider_;
  std::function<bool(const std::string &)> users_set_handler_;
  // Identify the pairing this UI started. The success handler matches
  // Status::job_id against pairing_job_id_ before firing on_paired_cb_,
  // so a previous job's lingering SUCCESS can never apply the wrong
  // name. All three fields are touched only by the HTTP-server task.
  std::string    pairing_keypad_name_;
  std::string    pairing_job_id_;
  bool           success_notified_{false};

  // Keypads identified from their BLE advertisement, keyed by pretty MAC. A
  // keypad's model signature rides in the 0xFD3D service data, which for the
  // Keypad Vision only arrives in the (intermittently received) scan response.
  // Caching every positive identification keeps such a keypad listed on later
  // scans where its service data was missed, as long as it's still in range.
  std::map<std::string, KeypadIdent> identified_keypads_;
};

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
