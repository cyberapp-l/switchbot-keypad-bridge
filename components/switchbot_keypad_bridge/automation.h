#pragma once

#include "esphome/core/automation.h"
#include "switchbot_keypad_bridge.h"

namespace esphome {
namespace switchbot_keypad_bridge {

class LockTrigger : public Trigger<> {
 public:
  explicit LockTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_lock_callback([this]() { this->trigger(); });
  }
};

class UnlockTrigger : public Trigger<std::string, int, std::string> {
 public:
  explicit UnlockTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_unlock_callback(
        [this](const std::string &method, int index, const std::string &name) {
          this->trigger(method, index, name);
        });
  }
};

class DoorbellTrigger : public Trigger<> {
 public:
  explicit DoorbellTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_doorbell_callback([this]() { this->trigger(); });
  }
};

class TamperTrigger : public Trigger<> {
 public:
  explicit TamperTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_tamper_callback([this]() { this->trigger(); });
  }
};

class DuressTrigger : public Trigger<> {
 public:
  explicit DuressTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_duress_callback([this]() { this->trigger(); });
  }
};

// switchbot_keypad_bridge.send_command — send one raw plaintext command (hex)
// to the paired keypad. For experiments / reverse-engineering.
template<typename... Ts>
class SendCommandAction : public Action<Ts...>, public Parented<SwitchbotKeypadBridge> {
 public:
  TEMPLATABLE_VALUE(std::string, command)
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(int, key_id)
  // Params are passed by const-ref to match Action::play in current ESPHome.
  void play(const Ts &...x) override {
    this->parent_->send_raw_command(this->command_.value(x...), this->key_.value(x...),
                                    this->key_id_.value(x...));
  }
};

// switchbot_keypad_bridge.read_settings — read all keypad settings in one BLE
// connection; the values arrive via the on_settings_read trigger.
template<typename... Ts>
class ReadSettingsAction : public Action<Ts...>, public Parented<SwitchbotKeypadBridge> {
 public:
  TEMPLATABLE_VALUE(std::string, key)
  TEMPLATABLE_VALUE(int, key_id)
  void play(const Ts &...x) override {
    this->parent_->read_settings(this->key_.value(x...), this->key_id_.value(x...));
  }
};

// Fires after a read_settings run with one value per parameter, in the order
// documented on SwitchbotKeypadBridge::read_settings (-1 where a GET failed).
class SettingsReadTrigger : public Trigger<std::vector<int>> {
 public:
  explicit SettingsReadTrigger(SwitchbotKeypadBridge *parent) {
    parent->add_on_settings_read_callback(
        [this](std::vector<int> values) { this->trigger(std::move(values)); });
  }
};

// switchbot_keypad_bridge.rearm — return the emulated lock to LOCKED so the
// keypad re-enables passive face/palm scanning (see SwitchbotKeypadBridge::rearm).
template<typename... Ts>
class RearmAction : public Action<Ts...>, public Parented<SwitchbotKeypadBridge> {
 public:
  void play(const Ts &...x) override { this->parent_->rearm(); }
};

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
