#include "keypad_advert.h"

#include <cstring>

namespace esphome {
namespace switchbot_keypad_bridge {

const char *keypad_family_str(KeypadFamily family) {
  return family == KeypadFamily::VISION ? "vision" : "original";
}

KeypadFamily keypad_family_from_str(const char *s) {
  return (s != nullptr && std::strcmp(s, "vision") == 0) ? KeypadFamily::VISION
                                                         : KeypadFamily::ORIGINAL;
}

namespace {

// pySwitchbot SUPPORTED_TYPES keypad signatures (SwitchBot service-data,
// advertised under UUID 0xFD3D — with 0x0D00 as a legacy fallback):
//
//   first byte 'y'  (0x79, low 7 bits) ........... Keypad / Keypad Touch → ORIGINAL
//   4-byte suffix  ?? 11 03 78  (?? = enc bit) .... Keypad Vision         → VISION
//   4-byte suffix  ?? 11 51 98 ................... Keypad Vision Pro      → VISION
//
// The first byte is matched after masking the high (encryption) bit; the
// Vision variants are matched as a trailing 4-byte window. This mirrors
// pySwitchbot's `_find_model_from_service_data` (first byte) and
// `_find_model_from_service_data_suffix` (which tries both the [-4:] and the
// [-5:-1] window).
constexpr uint8_t KEYPAD_TYPE_BYTE = 0x79;  // 'y'

// True when a 4-byte window equals `?? b1 b2 b3`, accepting 0x00 or 0x01 as the
// leading encryption bit (both forms appear in pySwitchbot's table).
bool suffix_is(const uint8_t *w, uint8_t b1, uint8_t b2, uint8_t b3) {
  return (w[0] == 0x00 || w[0] == 0x01) && w[1] == b1 && w[2] == b2 &&
         w[3] == b3;
}

}  // namespace

KeypadIdent identify_keypad(const uint8_t *svc_data, size_t len) {
  KeypadIdent id;
  if (svc_data == nullptr || len == 0) return id;

  // 1. First-byte match (low 7 bits) — Keypad / Keypad Touch (Original family).
  if ((svc_data[0] & 0x7F) == KEYPAD_TYPE_BYTE) {
    id.is_keypad = true;
    id.family = KeypadFamily::ORIGINAL;
    return id;
  }

  // 2. Trailing 4-byte window — Keypad Vision (?? 11 03 78) and Keypad Vision
  //    Pro (?? 11 51 98), both the Vision family. Try the last-4 and the
  //    next-to-last-4 windows, as pySwitchbot does.
  if (len > 5) {
    const uint8_t *windows[] = {svc_data + (len - 4), svc_data + (len - 5)};
    for (const uint8_t *w : windows) {
      if (suffix_is(w, 0x11, 0x03, 0x78) || suffix_is(w, 0x11, 0x51, 0x98)) {
        id.is_keypad = true;
        id.family = KeypadFamily::VISION;
        return id;
      }
    }
  }

  return id;
}

int parse_keypad_battery(KeypadFamily family,
                         const uint8_t *svc_data, size_t svc_len,
                         const uint8_t *mfr_data, size_t mfr_len) {
  // A battery level is a percentage; clamp so a firmware quirk or misread
  // can't publish e.g. 127% to a device_class: battery sensor.
  if (family == KeypadFamily::ORIGINAL) {
    if (svc_data == nullptr || svc_len < 3) return -1;
    const int pct = svc_data[2] & 0x7F;
    return pct > 100 ? 100 : pct;
  }

  // VISION: pySwitchbot reads mfr_data[7], where mfr_data is the payload
  // after the company id — the raw AD blob therefore carries it at byte 9,
  // behind the little-endian SwitchBot company id (0x69 0x09).
  if (mfr_data == nullptr || mfr_len < 10) return -1;
  if (mfr_data[0] != 0x69 || mfr_data[1] != 0x09) return -1;
  const int pct = mfr_data[9] & 0x7F;
  return pct > 100 ? 100 : pct;
}

KeypadStatus parse_keypad_status(KeypadFamily family,
                                 const uint8_t *svc_data, size_t svc_len,
                                 const uint8_t *mfr_data, size_t mfr_len) {
  KeypadStatus st;

  if (family == KeypadFamily::ORIGINAL) {
    // Original / Touch: only a battery percentage, in the service data.
    if (svc_data != nullptr && svc_len >= 3) {
      const int pct = svc_data[2] & 0x7F;
      st.battery = pct > 100 ? 100 : pct;
      st.valid = true;
    }
    return st;
  }

  // VISION / VISION Pro. Our mfr_data keeps the 2-byte company id, so every
  // pySwitchbot index i is our (i + 2). process_common_mfr_data reads up to
  // its index 12 (our 14) → require our length >= 15.
  if (mfr_data == nullptr || mfr_len < 15) return st;
  if (mfr_data[0] != 0x69 || mfr_data[1] != 0x09) return st;

  const int pct = mfr_data[9] & 0x7F;  // their [7]
  st.battery = pct > 100 ? 100 : pct;
  st.charging = (mfr_data[9] & 0x80) != 0;

  const uint8_t alarms = mfr_data[10];  // their [8]
  st.lockout = (alarms & 0x01) != 0;
  st.tamper = (alarms & 0x02) != 0;
  st.duress = (alarms & 0x04) != 0;
  st.high_temperature = (alarms & 0x40) != 0;
  st.low_temperature = (alarms & 0x80) != 0;

  // PIR level (Vision) / radar level (Vision Pro): their [13] → our [15].
  // This is a 0..3 level, not a clean flag, and is undocumented — expose the
  // raw level for tuning and treat only the upper levels as "motion" so the
  // sensor isn't stuck on at an idle baseline.
  if (mfr_len >= 16) {
    st.pir_level = mfr_data[15] & 0x03;
    st.motion = st.pir_level >= 2;
  }

  st.valid = true;
  return st;
}

}  // namespace switchbot_keypad_bridge
}  // namespace esphome
