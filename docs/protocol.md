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
| `0f 52 01 <p> <v…>` | **SET** parameter `p` to value `v` | config writes; keypad replies with a bare ack |
| `0f 53 01 <p>` | **GET** parameter `p`            | config reads; keypad replies with the value |
| `01 03`        | doorbell / call button           | |
| `00 05 …`      | credential op (add/modify code?) | passcode management |

**Response semantics.** A **SET** (`0f52`) is fire-and-forget: the keypad returns
only a bare 4-byte header (`01 <key_id> <iv0> <iv1>`) with *no* encrypted payload —
so an empty `decrypted=` from `send_command` after a SET is success, not failure.
Only a **GET** (`0f53`) returns an encrypted data payload. (The official app
behaves identically — its SET responses are the same bare acks.)

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

| param | seen values           | GET reply | meaning |
|-------|-----------------------|-----------|---------|
| `0x02`| `01`, `02`            | —         | a mode/enable toggle |
| `0x07`| `01`                  | `00`      | a status/feature flag |
| `0x0c`| `01 02` … `04 02`     | `02 04`   | **volume** — confirmed on hardware (see below) |
| `0x0d`| GET only              | —         | paired with `0x0c` (read-back of the same setting group) |

**Volume — CONFIRMED on hardware.** `0f 52 01 0c 0X 02`, where `0X` is a
**4-level** value (the trailing `02` is a fixed tag):

| command          | volume  |
|------------------|---------|
| `0f52010c0102`   | mute    |
| `0f52010c0202`   | low     |
| `0f52010c0302`   | medium  |
| `0f52010c0402`   | high    |

The keypad acks the SET with a bare header (no payload), exactly as it does for
the official app. To read the current level back, GET it: `0f 53 01 0c`
(returned `02 04` in the capture).

## Trying commands from this firmware

The `switchbot_keypad_bridge.send_command` action connects as a BLE central,
negotiates an IV, sends one raw plaintext command, then decrypts and logs the
reply.

### Which key encrypts the command

This is the part that trips people up. The keypad has **two** command channels,
each with its own key:

| `key` / `key_id` you pass | Key used | Default `key_id` | Channel |
|---|---|---|---|
| both empty | the bridge's **session key** (`shared_key_`) | `0xC6` Vision / `0x88` Original | lock-emulation (keypad ↔ bridge) |
| `key` set  | the key you pass | `0x45` | app / cloud channel |

The **app-style settings** commands (`0f52`/`0f53` — volume, feature toggles)
were captured on the **communication key (K14)** at **`key_id` 0x45**, *not* the
lock-emulation channel. So to replay them you must pass your K14 explicitly:

```yaml
on_...:
  - switchbot_keypad_bridge.send_command:
      command: "0f52010c0202"                     # SET volume = low (0x0c, level 2)
      key: "0286…"                                # your K14 (32 hex chars)
      key_id: 0x45
```

Leaving `key` empty uses the bridge's own session key and slot — good for
lock/unlock-style traffic, but the keypad may not honour settings writes there.
The decrypted response is logged at `WARN` as `send_command: … decrypted=<hex>`.
Get your K14 from `show_communication_key` (web console → **Settings**).

> **"Could not connect to the keypad."** The keypad is only *connectable* in
> short windows (it sleeps between interactions), whereas the official app — a
> pure BLE central — catches those windows easily. The bridge is dual-role
> (it also advertises as a lock), so it retries the connect a few times and
> pauses its own advertising for the attempt. If it still fails, **wake the
> keypad by tapping a key right before** sending, and keep it within ~2 m. The
> failure log includes the NimBLE `rc=` code for diagnosis.

### Calling it from Home Assistant

`send_command` is an ESPHome **action** — a YAML building block — so it is *not*
itself a Home Assistant service and won't show up in HA on its own. Wrap it in
an `api:` action to expose a service you can call from HA → Developer Tools →
Actions:

```yaml
api:
  actions:
    - action: send_keypad_command
      variables:
        command: string
        key: string   # optional 32-hex K14; empty = bridge session key
        key_id: int    # optional; 69 = 0x45 (app channel), 0 = auto
      then:
        - switchbot_keypad_bridge.send_command:
            command: !lambda "return command;"
            key: !lambda "return key;"
            key_id: !lambda "return key_id;"
```

That surfaces `esphome.<device>_send_keypad_command`, taking `command` (hex) plus
optional `key` (your K14) and `key_id` (`69` for the 0x45 app channel). Leave
`key`/`key_id` blank for the session-key channel. (The WT32-ETH01 example config
ships this wrapper.)

## Reproducing a capture

1. Android → Developer options → **Enable Bluetooth HCI snoop log**.
2. Operate the keypad from the official SwitchBot app (unlock, change volume, …).
3. Pull `btsnoop_hci.log` (via `adb bugreport` or the vendor path).
4. `python tools/decrypt_capture.py btsnoop_hci.log <your-K14-hex>`

It prints every decrypted command/response and a de-duplicated list of the
distinct commands it saw — a fast way to spot new parameters.
