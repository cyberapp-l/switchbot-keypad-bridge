"""ESPHome integration: SwitchBot Keypad Bridge.

This external component emulates a SwitchBot Lock BLE peripheral so a paired
SwitchBot Keypad can deliver encrypted unlock/lock commands. The bridge
decrypts the frames in-place using mbed-TLS / PSA Crypto and exposes the
decoded commands through two surfaces:

- A standard ESPHome ``event`` entity (``Lock``/``Unlock``/``Doorbell`` types).
- ESPHome automation triggers (``on_lock`` / ``on_unlock`` / ``on_doorbell``).
  ``on_unlock`` receives the unlock ``method`` and credential ``index`` as
  arguments; the others take none.

The AES session key is generated on the device on first boot and kept in
NVS — it is never part of the YAML configuration.
"""

from __future__ import annotations

import gzip
import logging
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
import esphome.final_validate as fv
from esphome import automation
from esphome.components import binary_sensor, button, event, sensor, text_sensor
from esphome.components.esp32 import (
    add_idf_component,
    add_idf_sdkconfig_option,
    include_builtin_idf_component,
)
from esphome.const import (
    CONF_BATTERY_LEVEL,
    CONF_ID,
    CONF_NAME,
    CONF_TRIGGER_ID,
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_CONFIG,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_PERCENT,
)
from esphome.core import CORE, HexInt

LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@pierluigizagaria"]
DEPENDENCIES = ["esp32"]
AUTO_LOAD = ["event", "text_sensor", "button", "sensor", "binary_sensor"]
MULTI_CONF = False

CONF_KEYPAD_ACTION = "keypad_action"
CONF_KEYPAD = "keypad"
CONF_BATTERY_SCAN_INTERVAL = "battery_scan_interval"
CONF_UNPAIR_BUTTON = "unpair_button"
CONF_ON_LOCK = "on_lock"
CONF_ON_UNLOCK = "on_unlock"
CONF_ON_DOORBELL = "on_doorbell"
CONF_PAIRING_UI = "pairing_ui"

# Credential labelling / unlock statistics.
CONF_LAST_USER = "last_user"
CONF_LAST_METHOD = "last_method"
CONF_UNLOCK_COUNTS = "unlock_counts"
CONF_USERS = "users"
CONF_METHOD = "method"
CONF_INDEX = "index"
CONF_MIN_UNLOCK_INTERVAL = "min_unlock_interval"

# HTTP Basic Auth for the always-on web console.
CONF_WEB_USERNAME = "web_username"
CONF_WEB_PASSWORD = "web_password"

# Keypad liveness / signal diagnostics.
CONF_RSSI = "rssi"
CONF_KEYPAD_CONNECTED = "keypad_connected"
CONF_LAST_SEEN = "last_seen"

# UnlockMethod bytes as the keypad reports them (see lock_protocol.h). 0xFF is
# the "any method" wildcard used by the users mapping.
_UNLOCK_METHOD_BYTES = {
    "pin": 0x04,
    "nfc": 0x08,
    "fingerprint": 0x0C,
    "face": 0x18,
    "unknown": 0x00,
}
_USER_METHOD_BYTES = {"any": 0xFF, **_UNLOCK_METHOD_BYTES}
# Methods that get their own optional running-count sensor.
_COUNT_METHODS = ["pin", "nfc", "fingerprint", "face"]

# Auto-generated id for the progmem array that carries the embedded UI.
CONF_PAIRING_UI_HTML_ID = "pairing_ui_html_id"

# The pairing wizard is a single, self-contained HTML file living next to
# this module. It doubles as a browser-openable design preview.
PAIRING_UI_HTML = Path(__file__).parent / "pairing_ui.html"


switchbot_keypad_bridge_ns = cg.esphome_ns.namespace("switchbot_keypad_bridge")
SwitchbotKeypadBridge = switchbot_keypad_bridge_ns.class_(
    "SwitchbotKeypadBridge", cg.Component
)
UnpairButton = switchbot_keypad_bridge_ns.class_("UnpairButton", button.Button)
LockTrigger = switchbot_keypad_bridge_ns.class_(
    "LockTrigger", automation.Trigger.template()
)
UnlockTrigger = switchbot_keypad_bridge_ns.class_(
    "UnlockTrigger", automation.Trigger.template(cg.std_string, cg.int_, cg.std_string)
)
DoorbellTrigger = switchbot_keypad_bridge_ns.class_(
    "DoorbellTrigger", automation.Trigger.template()
)


def _deprecated_pairing_ui(value):
    """The on-device wizard is now always compiled in. Accept ``true`` for
    backwards compatibility (with a deprecation warning) and reject ``false``
    explicitly so users who tried to disable it learn what changed."""
    v = cv.boolean(value)
    if v is False:
        raise cv.Invalid(
            "pairing_ui: false is no longer supported — the on-device pairing "
            "wizard is always compiled in. Remove this option."
        )
    LOGGER.warning(
        "switchbot_keypad_bridge: the 'pairing_ui' option is deprecated and "
        "can be removed — the wizard is now always available."
    )
    return v


def _battery_scan_interval(value):
    """A too-short interval means back-to-back active BLE scans that starve the
    keypad link (0 would scan forever), so enforce a sane floor."""
    value = cv.positive_time_period_milliseconds(value)
    if value.total_milliseconds < 30000:
        raise cv.Invalid("battery_scan_interval must be at least 30s.")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SwitchbotKeypadBridge),
        cv.Optional(CONF_KEYPAD_ACTION): event.event_schema(icon="mdi:gesture-tap"),
        cv.Optional(CONF_KEYPAD): text_sensor.text_sensor_schema(
            icon="mdi:dialpad",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # The keypad broadcasts its battery in its BLE advertisement; the
        # bridge picks it up with a short background scan once per interval.
        cv.Optional(CONF_BATTERY_LEVEL): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            device_class=DEVICE_CLASS_BATTERY,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ),
        cv.Optional(CONF_BATTERY_SCAN_INTERVAL, default="15min"): _battery_scan_interval,
        # Keypad BLE signal strength, refreshed by the battery-advert scan.
        cv.Optional(CONF_RSSI): sensor.sensor_schema(
            unit_of_measurement="dBm",
            device_class="signal_strength",
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            accuracy_decimals=0,
        ),
        # On during the keypad's short per-action BLE connection.
        cv.Optional(CONF_KEYPAD_CONNECTED): binary_sensor.binary_sensor_schema(
            device_class="connectivity",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:bluetooth-connect",
        ),
        # ISO-8601 UTC timestamp of the last time the keypad was heard (connect
        # or advert). Add `device_class: timestamp` in YAML for relative display.
        cv.Optional(CONF_LAST_SEEN): text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:clock-outline",
        ),
        # Display name of the credential behind the most recent unlock, resolved
        # through the `users` mapping (falls back to "<method> #<index>").
        cv.Optional(CONF_LAST_USER): text_sensor.text_sensor_schema(
            icon="mdi:account-check",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Method of the most recent unlock (pin / fingerprint / nfc / face).
        cv.Optional(CONF_LAST_METHOD): text_sensor.text_sensor_schema(
            icon="mdi:gesture-tap-button",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        # Optional per-method running unlock counters (reset on reboot).
        cv.Optional(CONF_UNLOCK_COUNTS): cv.Schema(
            {
                cv.Optional(m): sensor.sensor_schema(
                    state_class=STATE_CLASS_TOTAL_INCREASING,
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                    accuracy_decimals=0,
                    icon="mdi:counter",
                )
                for m in _COUNT_METHODS
            }
        ),
        # Map credential slots to human names. `method` defaults to "any" and
        # `index` to -1 (any slot), so a bare name can catch a whole method.
        cv.Optional(CONF_USERS): cv.ensure_list(
            cv.Schema(
                {
                    cv.Required(CONF_NAME): cv.string_strict,
                    cv.Optional(CONF_METHOD, default="any"): cv.one_of(
                        *_USER_METHOD_BYTES, lower=True
                    ),
                    cv.Optional(CONF_INDEX, default=-1): cv.int_,
                }
            )
        ),
        # Debounce repeated unlocks from the same credential (0 = off).
        cv.Optional(
            CONF_MIN_UNLOCK_INTERVAL, default="0s"
        ): cv.positive_time_period_milliseconds,
        # HTTP Basic Auth for the always-on web console. Omit web_password to
        # leave it open (original behaviour).
        cv.Optional(CONF_WEB_USERNAME, default="admin"): cv.string_strict,
        cv.Optional(CONF_WEB_PASSWORD): cv.string_strict,
        cv.Optional(CONF_PAIRING_UI): _deprecated_pairing_ui,
        cv.Optional(CONF_UNPAIR_BUTTON): button.button_schema(
            UnpairButton,
            entity_category=ENTITY_CATEGORY_CONFIG,
            icon="mdi:link-off",
        ),
        cv.GenerateID(CONF_PAIRING_UI_HTML_ID): cv.declare_id(cg.uint8),
        cv.Optional(CONF_ON_LOCK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(LockTrigger)}
        ),
        cv.Optional(CONF_ON_UNLOCK): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(UnlockTrigger)}
        ),
        cv.Optional(CONF_ON_DOORBELL): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(DoorbellTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


_INCOMPATIBLE_COMPONENTS = [
    "esp32_ble",
    "esp32_improv",
    "esp32_ble_beacon",
    "esp32_ble_client",
    "esp32_ble_tracker",
    "esp32_ble_server",
]


def _final_validate(config):
    full_config = fv.full_config.get()

    if CORE.is_esp32:
        conflicting = [c for c in _INCOMPATIBLE_COMPONENTS if c in full_config]
        if conflicting:
            raise cv.Invalid(
                "switchbot_keypad_bridge uses NimBLE directly and is incompatible with "
                "the ESPHome BLE stack. Remove these components from your configuration: "
                + ", ".join(conflicting)
            )

        if "psram" not in full_config:
            LOGGER.warning(
                "switchbot_keypad_bridge: consider enabling PSRAM if available — "
                "NimBLE benefits from the extra heap."
            )

    # The pairing wizard binds to port 80 and reuses ESPHome's USE_WEBSERVER
    # flag for HA discovery. Both clash with the official `web_server:`
    # component, which also defines USE_WEBSERVER and (by default) listens
    # on 80.
    if "web_server" in full_config:
        raise cv.Invalid(
            "switchbot_keypad_bridge cannot coexist with the `web_server:` "
            "component — both bind to port 80 and both define USE_WEBSERVER. "
            "Remove `web_server:`."
        )

    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if keypad_conf := config.get(CONF_KEYPAD_ACTION):
        keypad = await event.new_event(
            keypad_conf, event_types=["Lock", "Unlock", "Doorbell"]
        )
        cg.add(var.set_keypad_event(keypad))

    if keypad_sensor_conf := config.get(CONF_KEYPAD):
        sens = await text_sensor.new_text_sensor(keypad_sensor_conf)
        cg.add(var.set_keypad_text_sensor(sens))

    if battery_conf := config.get(CONF_BATTERY_LEVEL):
        batt = await sensor.new_sensor(battery_conf)
        cg.add(var.set_battery_level_sensor(batt))
        cg.add(
            var.set_battery_scan_interval(
                config[CONF_BATTERY_SCAN_INTERVAL].total_milliseconds
            )
        )

    if button_conf := config.get(CONF_UNPAIR_BUTTON):
        btn = await button.new_button(button_conf)
        await cg.register_parented(btn, config[CONF_ID])

    if last_user_conf := config.get(CONF_LAST_USER):
        sens = await text_sensor.new_text_sensor(last_user_conf)
        cg.add(var.set_last_user_text_sensor(sens))

    if last_method_conf := config.get(CONF_LAST_METHOD):
        sens = await text_sensor.new_text_sensor(last_method_conf)
        cg.add(var.set_last_method_text_sensor(sens))

    if rssi_conf := config.get(CONF_RSSI):
        rssi_sensor = await sensor.new_sensor(rssi_conf)
        cg.add(var.set_rssi_sensor(rssi_sensor))

    if connected_conf := config.get(CONF_KEYPAD_CONNECTED):
        conn = await binary_sensor.new_binary_sensor(connected_conf)
        cg.add(var.set_connected_binary_sensor(conn))

    if last_seen_conf := config.get(CONF_LAST_SEEN):
        seen = await text_sensor.new_text_sensor(last_seen_conf)
        cg.add(var.set_last_seen_text_sensor(seen))

    if counts_conf := config.get(CONF_UNLOCK_COUNTS):
        for method_name in _COUNT_METHODS:
            if count_conf := counts_conf.get(method_name):
                count_sensor = await sensor.new_sensor(count_conf)
                cg.add(
                    var.set_unlock_count_sensor(
                        _UNLOCK_METHOD_BYTES[method_name], count_sensor
                    )
                )

    for user in config.get(CONF_USERS, []):
        cg.add(
            var.add_user(
                _USER_METHOD_BYTES[user[CONF_METHOD]], user[CONF_INDEX], user[CONF_NAME]
            )
        )

    cg.add(var.set_min_unlock_interval(config[CONF_MIN_UNLOCK_INTERVAL].total_milliseconds))

    cg.add(
        var.set_web_credentials(
            config[CONF_WEB_USERNAME], config.get(CONF_WEB_PASSWORD, "")
        )
    )

    # Make Home Assistant show the "Visit Device" link on the device page.
    # ESPHome's api component fills `webserver_port` in DeviceInfoResponse
    # iff USE_WEBSERVER is defined; HA uses that field to construct the URL.
    # We piggy-back on the same flag so the pairing wizard gets discovered
    # without requiring the user to also enable `web_server:` in YAML.
    cg.add_define("USE_WEBSERVER")
    cg.add_define("USE_WEBSERVER_PORT", 80)

    # Bake the pairing wizard's HTML into the firmware image as a
    # gzip-compressed PROGMEM array (~4x smaller; served with
    # Content-Encoding: gzip). `pairing_ui.html` is the single source of
    # truth — no generated header to commit, no build step to run by hand.
    # mtime=0 keeps the gzip header (and thus the build) reproducible.
    html_bytes = gzip.compress(PAIRING_UI_HTML.read_bytes(), mtime=0)
    html_arr = cg.progmem_array(
        config[CONF_PAIRING_UI_HTML_ID], [HexInt(b) for b in html_bytes]
    )
    cg.add(var.set_pairing_ui_html(html_arr, len(html_bytes)))

    for trig_conf in config.get(CONF_ON_LOCK, []):
        trig = cg.new_Pvariable(trig_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trig, [], trig_conf)

    for trig_conf in config.get(CONF_ON_UNLOCK, []):
        trig = cg.new_Pvariable(trig_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trig,
            [(cg.std_string, "method"), (cg.int_, "index"), (cg.std_string, "name")],
            trig_conf,
        )

    for trig_conf in config.get(CONF_ON_DOORBELL, []):
        trig = cg.new_Pvariable(trig_conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trig, [], trig_conf)

    # NimBLE C++ wrapper, pulled as ESP-IDF managed component (no Python deps).
    add_idf_component(
        name="esp-nimble-cpp",
        repo="https://github.com/h2zero/esp-nimble-cpp.git",
        ref="2.5.0",
    )

    # Switch the BT stack from Bluedroid to NimBLE.
    add_idf_sdkconfig_option("CONFIG_BT_ENABLED", True)
    add_idf_sdkconfig_option("CONFIG_BT_BLUEDROID_ENABLED", False)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_ENABLED", True)

    # Silence the NimBLE logger to keep heap and serial noise to a minimum.
    add_idf_sdkconfig_option("CONFIG_NIMBLE_CPP_LOG_LEVEL", 0)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_LOG_LEVEL", 0)
    add_idf_sdkconfig_option("CONFIG_BT_NIMBLE_LOG_LEVEL_NONE", True)

    # The cloud client calls https://*.switchbot.net — pull in mbedTLS's
    # built-in certificate bundle so esp_crt_bundle_attach() finds the CAs
    # that sign those domains. Without CONFIG_MBEDTLS_CERTIFICATE_BUNDLE the
    # TLS handshake fails with ESP_ERR_HTTP_CONNECT. Also tell ESP-IDF that
    # this user component depends on esp_http_client and esp-tls so the
    # headers and link symbols are visible to cloud_client.cpp.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL", True)
    include_builtin_idf_component("esp_http_client")
    include_builtin_idf_component("esp-tls")
