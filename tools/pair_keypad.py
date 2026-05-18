#!/usr/bin/env python3
"""Pair a SwitchBot Keypad with the switchbot-keypad-bridge ESP32 device.

Usage:
    python pair_keypad.py KEYPAD_MAC ESP_MAC SHARED_KEY --user EMAIL [--password PASSWORD]

Where to find the addresses:
    KEYPAD_MAC  SwitchBot app -> open the keypad device -> ... -> Device Info -> BLE Address
    ESP_MAC     ESPHome boot log ("BLE address: ...") or Home Assistant device page ("BLE MAC" sensor)

The keypad model (Original/Touch vs Vision/Vision Pro) is auto-detected from
the user-credential key_id returned by the SwitchBot cloud — see
KEYPAD_PRESETS below. The bridge firmware must be configured with the same
"token slot" id (default 0x88 = Original; the Vision uses 0xC6).
"""

import argparse
import asyncio
import getpass
import json
import sys
import urllib.request

from bleak import BleakClient
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

_SWITCHBOT_CLIENT_ID = "5nnwmhmsa9xxskm14hd85lm9bm"
_API_ACCOUNT_BASE = "https://account.api.switchbot.net"
_UUID_RX = "cba20002-224d-11e6-9fb8-0002a5d5c51b"
_UUID_TX = "cba20003-224d-11e6-9fb8-0002a5d5c51b"
_ACK_TIMEOUT = 3.0


# ---------------------------------------------------------------------------
# SwitchBot cloud
# ---------------------------------------------------------------------------

def _api_post(url: str, data: dict | None = None, headers: dict | None = None) -> dict:
    body = json.dumps(data or {}).encode()
    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Content-Type", "application/json")
    if headers:
        for k, v in headers.items():
            req.add_header(k, v)
    with urllib.request.urlopen(req, timeout=10) as resp:
        result = json.loads(resp.read())
    if result.get("statusCode") != 100:
        raise RuntimeError(
            f"SwitchBot API error {result.get('statusCode')}: {result.get('message')}"
        )
    return result["body"]


def fetch_keypad_credentials(keypad_mac: str, username: str, password: str) -> tuple[int, bytes]:
    print("Fetching keypad credentials...")

    auth = _api_post(
        f"{_API_ACCOUNT_BASE}/account/api/v1/user/login",
        {
            "clientId": _SWITCHBOT_CLIENT_ID,
            "username": username,
            "password": password,
            "grantType": "password",
            "verifyCode": "",
        },
    )
    auth_headers = {"authorization": auth["access_token"]}

    try:
        userinfo = _api_post(
            f"{_API_ACCOUNT_BASE}/account/api/v1/user/userinfo",
            headers=auth_headers,
        )
        region = userinfo.get("botRegion") or "us"
    except Exception:
        region = "us"

    mac = keypad_mac.replace(":", "").replace("-", "").upper()
    comm = _api_post(
        f"https://wonderlabs.{region}.api.switchbot.net/wonder/keys/v1/communicate",
        {"device_mac": mac, "keyType": "user"},
        auth_headers,
    )
    key_info = comm["communicationKey"]
    return int(key_info["keyId"], 16), bytes.fromhex(key_info["key"])


# ---------------------------------------------------------------------------
# Per-model pairing presets
# ---------------------------------------------------------------------------
#
# Different keypad families speak slightly different dialects of the same
# pairing ceremony. The cloud API reports the per-device user credential
# `key_id` (the one used to encrypt the pairing session), which doubles as a
# reliable family fingerprint:
#
#   0x7E -> Keypad / Keypad Touch  (token slot 0x88, family 0x52)
#   0x72 -> Keypad Vision / Vision Pro (token slot 0xC6, family 0x53)
#
# The bridge firmware needs to know which "token slot" the keypad will use to
# look up its peer MAC at runtime, hence the same value is configured on both
# sides.

KEYPAD_PRESETS = {
    0x7E: {
        "name": "Keypad / Keypad Touch",
        "shared_slot": 0x88,
        "slot_init_nonce": 0x69,
        "enter_pairing": bytes.fromhex("0f52010700"),
        "capabilities_probe": None,
        "finalize_tail": bytes.fromhex("000809040507"),
    },
    0x72: {
        "name": "Keypad Vision / Vision Pro",
        "shared_slot": 0xC6,
        "slot_init_nonce": 0x80,
        "enter_pairing": bytes.fromhex("0f530107"),
        "capabilities_probe": bytes.fromhex("0f530703"),
        "finalize_tail": bytes.fromhex("040401050809"),
    },
}


# ---------------------------------------------------------------------------
# BLE pairing
# ---------------------------------------------------------------------------

class Pairer:
    def __init__(self, client: BleakClient, key_id: int, key: bytes):
        self._client = client
        self._key_id = key_id
        self._key = key
        self._iv: bytes | None = None
        self._event = asyncio.Event()

    def _on_notify(self, _sender, data: bytearray):
        if len(data) == 20 and data[0] == 0x01 and data[1] == 0x00:
            self._iv = bytes(data[4:20])
        self._event.set()

    async def start(self):
        await self._client.start_notify(_UUID_TX, self._on_notify)

    async def _wait(self):
        await asyncio.wait_for(self._event.wait(), timeout=_ACK_TIMEOUT)

    async def request_iv(self):
        self._event.clear()
        await self._client.write_gatt_char(
            _UUID_RX,
            bytes([0x57, 0x00, 0x00, 0x00, 0x0F, 0x21, 0x03, self._key_id]),
            response=True,
        )
        await self._wait()
        if self._iv is None:
            raise RuntimeError(
                "Keypad did not open a session. Make sure the keypad is in pairing mode."
            )

    async def send(self, plaintext: bytes):
        assert self._iv is not None
        ct = (
            Cipher(algorithms.AES128(self._key), modes.CTR(self._iv))
            .encryptor()
            .update(plaintext)
        )
        self._event.clear()
        await self._client.write_gatt_char(
            _UUID_RX,
            bytes([0x57, self._key_id]) + self._iv[:2] + ct,
            response=True,
        )
        try:
            await self._wait()
        except asyncio.TimeoutError:
            pass


async def _run_pairing(
    keypad_mac: str, esp_mac: str, shared_token: bytes, key_id: int, key: bytes
):
    preset = KEYPAD_PRESETS.get(key_id)
    if preset is None:
        raise RuntimeError(
            f"Unsupported keypad family (cloud reported key_id=0x{key_id:02X}). "
            f"Known families: " + ", ".join(f"0x{k:02X} ({v['name']})" for k, v in KEYPAD_PRESETS.items())
        )

    slot = preset["shared_slot"]
    nonce = preset["slot_init_nonce"]
    print(f"Detected keypad: {preset['name']}")

    print("Pairing...")
    async with BleakClient(keypad_mac) as client:
        p = Pairer(client, key_id, key)
        await p.start()

        await p.request_iv()

        esp_mac_bytes = bytes.fromhex(esp_mac.replace(":", ""))

        await p.send(preset["enter_pairing"])
        if preset["capabilities_probe"] is not None:
            await p.send(preset["capabilities_probe"])
        await p.send(bytes.fromhex("0603"))
        await p.send(bytes([0x0F, 0x20, 0x03, slot, nonce]))
        await p.send(bytes([0x0F, 0x20, 0x04, slot, 0x00]) + shared_token[:8])
        await p.send(bytes([0x0F, 0x20, 0x04, slot, 0x01]) + shared_token[8:])
        await p.send(bytes([0x06, 0x01, slot]) + esp_mac_bytes)
        await p.send(bytes.fromhex("0f520202") + bytes([0x10, 0xFF, 0x05, 0x06]) + preset["finalize_tail"])
        await p.send(bytes.fromhex("0f530106"))

    print("Done. Press any key on the keypad to verify.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Pair a SwitchBot Keypad with the switchbot-keypad-bridge ESP32 device.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Where to find the addresses:\n"
            "  KEYPAD_MAC  SwitchBot app -> open the keypad device -> ... -> Device Info -> BLE Address\n"
            "  ESP_MAC     ESPHome boot log ('BLE address: ...') or Home Assistant device page ('BLE MAC' sensor)"
        ),
    )
    parser.add_argument("keypad_mac", metavar="KEYPAD_MAC", help="BLE MAC of the SwitchBot Keypad.")
    parser.add_argument("esp_mac", metavar="ESP_MAC", help="BLE MAC of the ESP32 running switchbot-keypad-bridge.")
    parser.add_argument("shared_key", metavar="SHARED_KEY", help="32-hex-char key from generate_key.py, matching switchbot_shared_key in secrets.yaml.")
    parser.add_argument("-u", "--user", required=True, help="SwitchBot account email address.")
    parser.add_argument("-p", "--password", default=None, help="SwitchBot account password (prompted if omitted).")
    args = parser.parse_args()

    if len(args.shared_key) != 32:
        parser.error("SHARED_KEY must be exactly 32 hex characters (16 bytes).")
    try:
        shared_token = bytes.fromhex(args.shared_key)
    except ValueError:
        parser.error("SHARED_KEY must contain only hex characters (0-9, a-f).")

    password = args.password or getpass.getpass("SwitchBot password: ")

    try:
        key_id, key = fetch_keypad_credentials(args.keypad_mac, args.user, password)
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    except Exception as exc:
        print(
            f"Could not reach the SwitchBot cloud. Check your credentials and internet connection. ({exc})",
            file=sys.stderr,
        )
        sys.exit(1)

    try:
        asyncio.run(_run_pairing(args.keypad_mac, args.esp_mac, shared_token, key_id, key))
    except RuntimeError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        sys.exit(1)
    except Exception as exc:
        print(
            f"Could not connect to the keypad. Make sure Bluetooth is enabled and the keypad is nearby. ({exc})",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
