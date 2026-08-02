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
  void play(Ts... x) override {
    this->parent_->send_raw_command(this->command_.value(x...), this->key_.value(x...),
                                    this->key_id_.value(x...));
  }
};

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
