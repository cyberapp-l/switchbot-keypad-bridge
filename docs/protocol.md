# SwitchBot keypad BLE protocol (reverse-engineered notes)

These are working notes on the encrypted BLE protocol the SwitchBot keypad
speaks. They were reconstructed clean-room from an Android `btsnoop_hci.log`
capture of the official app plus this firmware's own traffic. Nothing here is
copied from SwitchBot or from any closed-source firmware. Treat it as
best-effort: it is enough to drive the keypad, not an official spec.

The companion tool [`tools/decrypt_capture.py`](../tools/decrypt_capture.py)
implements everything below — read it alongside this doc.

## GATT layout

The keypad exposes the SwitchBot lock service. This firmware also *impersonates*
a lock over the same UUIDs.

| Role        | UUID       | Handle (this capture) | Direction        |
|-------------|------------|-----------------------|------------------|
| Service     | `cba20d00…`| —                     | —                |
| RX (write)  | `cba20002…`| `0x0010`              | phone → keypad   |
| TX (notify) | `cba20003…`| `0x0012`              | keypad → phone   |

Handles are per-connection; discover them from the UUIDs rather than hard-coding.
`decrypt_capture.py` keys off `0x0010`/`0x0012` because that is what the sniffed
session used.

## Frame format

Every application frame is:

```
57 <key_id> <iv0> <iv1> <ciphertext…>      # command  (phone → keypad)
01 <key_id> <iv0> <iv1> <ciphertext…>      # response (keypad → phone)
```

- Byte 0 is `0x57` for commands, `0x01` for responses/acks.
- `key_id` selects which key the keypad uses. `0x00` during negotiation; the
  per-user communication key uses its own id (we observed `0x45`).
- `iv0 iv1` are a 2-byte nonce echoed from the negotiated session IV.
- `ciphertext` is `AES-128-CTR(key, session_IV, plaintext)`.

The **counter is the full 16-byte session IV**, and it is **reset to that IV at
the start of every frame** (SwitchBot does not advance the counter across
frames — each frame re-seeds CTR from the same session IV). So a single AES-CTR
decryptor initialised with `(key, session_IV)` decrypts the payload of any one
frame; re-initialise per frame.

## IV negotiation

Before any encrypted command, the phone negotiates a session IV:

```
phone → keypad :  57 00 00 00 0f 21 03 <key_id>
keypad → phone :  01 00 00 00 <IV[16]>
```

The 16 bytes after `01 00 00 00` are the session IV used as the CTR seed for the
rest of the connection. `decrypt_capture.py` watches for the `01 00 00 00 …`
20-byte reply and latches the IV.

## The key (K14)

`key` above is the keypad's **communication key**, sometimes called *K14*. It is
a per-device 16-byte AES key, not the user PIN. You need it to decrypt or to
send raw commands.

To read yours from this firmware: set `show_communication_key: true`, flash,
pair the keypad, then open the web console → **Settings**; the `key_id` and the
32-hex-char key are shown there (and logged once at `WARN`). It lives in RAM
only and the option is **off by default** — it is a genuine device secret, so
keep it out of public configs (use `!secret`) and out of git.

## Command families (decrypted plaintext)

Observed plaintext command prefixes. `01 03` is the fixed lock sub-header that
precedes most operations.

| Prefix (hex)   | Meaning                          | Notes |
|----------------|----------------------------------|-------|
| `0f 4e 01 03`  | lock / unlock                    | unlock reports method + credential index (see below) |
| `0f 4f 81`     | state poll                       | keypad returns lock/battery state |
| `0f 52 01 <p> <v…>` | **SET** parameter `p` to value `v` | config writes |
| `0f 53 01 <p>` | **GET** parameter `p`            | config reads |
| `01 03`        | doorbell / call button           | |
| `00 05 …`      | credential op (add/modify code?) | passcode management |

### Unlock method byte

On an unlock event the plaintext carries the credential type and slot index:

| Method byte | Credential |
|-------------|------------|
| `0x04`      | PIN        |
| `0x08`      | NFC card   |
| `0x0C`      | fingerprint|
| `0x18`      | face       |

This is what the firmware surfaces as the `method` + `index` (and mapped `name`)
on the unlock trigger / `last_user` sensor.

### Discovered SET/GET parameters

Seen in the settings capture (`0f 52 01 <param> <value>` / `0f 53 01 <param>`):

| param | seen values           | working hypothesis |
|-------|-----------------------|--------------------|
| `0x02`| `01`, `02`            | a mode/enable toggle |
| `0x07`| `01` (GET `0f 53 07 03`) | a status/feature flag |
| `0x0c`| `01 02`, `02 02`, `03 02` | **volume** — first byte 1/2/3 = low/med/high, second byte `02` a units/type tag |
| `0x0d`| GET only              | paired with `0x0c` (read-back of the same setting group) |

**Volume hypothesis** (untested on hardware): `0f 52 01 0c 0X 02` with
`0X ∈ {01,02,03}`. Try it with `send_command` and watch the decrypted response.

## Trying commands from this firmware

The `switchbot_keypad_bridge.send_command` action connects as a BLE central,
negotiates an IV, sends one raw plaintext command, then decrypts and logs the
reply. Example — set volume to medium:

```yaml
on_...:
  - switchbot_keypad_bridge.send_command:
      command: "0f52010c0202"   # SET param 0x0c = 02 (medium?), tag 02
      # key/key_id default to the paired communication key
```

Leave `key`/`key_id` unset to reuse the paired key. The decrypted response is
logged at `WARN` as `send_command: … decrypted=<hex>`.

## Reproducing a capture

1. Android → Developer options → **Enable Bluetooth HCI snoop log**.
2. Operate the keypad from the official SwitchBot app (unlock, change volume, …).
3. Pull `btsnoop_hci.log` (via `adb bugreport` or the vendor path).
4. `python tools/decrypt_capture.py btsnoop_hci.log <your-K14-hex>`

It prints every decrypted command/response and a de-duplicated list of the
distinct commands it saw — a fast way to spot new parameters.
