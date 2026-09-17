# Changelog

All notable changes to this fork are documented here.

This project is an independently maintained fork of
[pierluigizagaria/switchbot-keypad-bridge](https://github.com/pierluigizagaria/switchbot-keypad-bridge).
It follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **`switchbot-keypad-bridge-wt32-eth01-remote.yaml`** — a ready-to-use WT32-ETH01
  config that pulls the component straight from GitHub
  (`external_components: github://…`), so there's no `components/` folder to copy
  onto the ESPHome instance. Paste it into the dashboard; you maintain only this
  file plus `secrets.yaml`.

## [1.0.0] - 2026-09-17

First independent release of the fork. Everything below is on top of the base
project it was forked from.

### Added

- **WT32-ETH01 wired Ethernet (ESP-LAN)** build — `switchbot-keypad-bridge-wt32-eth01.yaml`,
  LAN8720 PHY, sources the component locally so CI builds this checkout.
- **"Who unlocked" diagnostics** — `last_user` / `last_method` sensors, per-method
  unlock counters, and a `users:` map that resolves credentials to display names
  (also editable from the web console). `min_unlock_interval` debounce.
- **Always-on web console** — the pairing wizard stays up as a config console
  (Pair / Activity / Users / Settings), bilingual (English/Hebrew), optionally
  behind HTTP Basic Auth. Home Assistant shows a **Visit Device** link.
- **On-device event log** and **web user manager** — rename credentials with no
  reflash.
- **Keypad alarms & status from the Vision advertisement** — `tamper`, `duress`,
  `lockout`, `motion`, `charging` binary sensors, plus `on_tamper` / `on_duress`
  triggers. Decoded passively from the advert (clean-room port of pySwitchbot's
  `keypad_vision` parser).
- **Liveness & signal** — `rssi`, `keypad_connected`, `last_seen`.
- **Web-editable settings persisted to NVS** — `battery_scan_interval`,
  `min_unlock_interval`, and a scanning on/off toggle for battery saving.
- **Reverse-engineered BLE protocol** — [`docs/protocol.md`](docs/protocol.md)
  (frame format, IV negotiation, command families, the confirmed 4-level volume
  and the full keypad-settings parameter map) and
  [`tools/decrypt_capture.py`](tools/decrypt_capture.py) to decrypt an Android
  `btsnoop_hci.log` given the keypad's K14.
- **`switchbot_keypad_bridge.send_command` action** + the
  `esphome.<device>_send_keypad_command` HA service — send one raw command and
  log the decrypted reply.
- **`switchbot_keypad_bridge.read_settings` action** + `on_settings_read` trigger,
  and **native HA controls** for the keypad's settings: `select` for volume,
  sensitivity, face-recognition trigger and disabling interval; `switch` for
  Fast Unlock and Disable Keypad; a **Refresh Keypad Settings** button that reads
  them all back in one BLE connection.
- **`show_communication_key`** — surface the keypad's communication key (K14) in
  the web console, persisted across reboots (opt-in; it is a device secret).
- **`auto_relock`** and the **`switchbot_keypad_bridge.rearm` action** — return
  the emulated lock to LOCKED after an unlock so a Keypad Vision keeps triggering
  passive face/palm scanning (timer, or closure-driven from a door-sensor
  automation). Neither needs a physical SwitchBot lock.
- **Paired-keypad banner** in the setup UI — a reload no longer looks unpaired.
- **CI** — compile the firmware and validate the Python codegen on every push.
- **Home Assistant automation cookbook** — [`docs/automations.md`](docs/automations.md).

### Fixed

- **Builds against current ESP-IDF/ESPHome** — declare the `json` (cJSON) and
  `esp_http_server` builtins explicitly; newer ESP-IDF no longer exposes them to
  the component transitively.
- **`send_command` reliability** — connect like the official app (pause our own
  advertising for the attempt, retry, log the NimBLE `rc`, close the link
  cleanly) so an idle keypad is reachable.

[Unreleased]: https://github.com/cyberapp-l/switchbot-keypad-bridge/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/cyberapp-l/switchbot-keypad-bridge/releases/tag/v1.0.0
