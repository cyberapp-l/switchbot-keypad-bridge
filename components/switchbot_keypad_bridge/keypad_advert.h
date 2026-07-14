#pragma once

// SwitchBot keypad identification from a BLE advertisement.
//
// This is the project's equivalent of pySwitchbot's `adv_parser` keypad
// detection: the model (and therefore the pairing protocol family) is read
// straight from the SwitchBot service-data advertisement rather than from a
// cloud SKU string. That makes detection independent of the ever-growing list
// of cloud `device_type` codes — a new keypad works as long as it advertises a
// recognised SwitchBot signature.
//
// The classifier is deliberately free of any NimBLE/cloud dependency: it takes
// the raw 0xFD3D service-data bytes so the byte-matching logic stays in one
// place and is trivial to reason about.

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace switchbot_keypad_bridge {

// Pairing-protocol dialect. The SwitchBot Keypad firmware ships in two
// families with slightly different BLE handshakes ("original" and "vision").
// Which family a keypad speaks is identified right here, from its BLE
// advertisement — never from any cloud field.
enum class KeypadFamily : uint8_t { ORIGINAL, VISION };

// Stable wire names for the family, used to carry it between the pairing UI
// and the firmware. `keypad_family_from_str` falls back to ORIGINAL for any
// unrecognised input.
const char *keypad_family_str(KeypadFamily family);
KeypadFamily keypad_family_from_str(const char *s);

struct KeypadIdent {
  bool is_keypad{false};
  KeypadFamily family{KeypadFamily::ORIGINAL};
};

// Identify a SwitchBot keypad from the bytes of its 0xFD3D service-data
// advertisement, mirroring pySwitchbot's SUPPORTED_TYPES signatures. Returns
// `is_keypad == false` when the advert is empty or not a recognised keypad.
KeypadIdent identify_keypad(const uint8_t *svc_data, size_t len);

// Extract the keypad's battery percentage from its BLE advertisement,
// mirroring pySwitchbot's adv_parsers:
//
//   ORIGINAL (process_wokeypad) ......... service-data byte 2, low 7 bits
//   VISION (process_keypad_vision[_pro])  manufacturer-data byte 7 after the
//                                         2-byte company id, low 7 bits
//
// `mfr_data` is the raw manufacturer-specific AD payload, company id
// included (little-endian 0x0969 for SwitchBot). Returns -1 when the
// advertisement does not carry the field.
int parse_keypad_battery(KeypadFamily family,
                         const uint8_t *svc_data, size_t svc_len,
                         const uint8_t *mfr_data, size_t mfr_len);

// Full keypad status decoded from the advertisement, mirroring pySwitchbot's
// keypad_vision adv parser. The alarm/motion/charging flags are only carried by
// the VISION family's manufacturer data; for ORIGINAL only `battery` is set.
struct KeypadStatus {
  bool valid{false};
  int battery{-1};          // 0..100, or -1 when absent
  bool charging{false};
  bool tamper{false};       // device pried off its mount
  bool duress{false};       // a duress/panic code was entered
  bool lockout{false};      // locked out after too many failed attempts
  bool low_temperature{false};
  bool high_temperature{false};
  bool motion{false};       // derived from pir_level (best-effort, see note)
  uint8_t pir_level{0};     // raw PIR/radar level 0..3 (Vision); logged for tuning
};

// Parse the full status. `mfr_data` is the raw manufacturer payload including
// the 2-byte company id (0x0969), so every pySwitchbot index i is our i+2.
KeypadStatus parse_keypad_status(KeypadFamily family,
                                 const uint8_t *svc_data, size_t svc_len,
                                 const uint8_t *mfr_data, size_t mfr_len);

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
