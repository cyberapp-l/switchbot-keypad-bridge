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

### Settings parameters

Config lives in a `0f 52 01 <param> <value>` (SET) / `0f 53 01 <param>` (GET)
family. Each parameter was mapped by capturing the official app while changing
one setting, then confirming the **GET read-back matched the app's shown value**
(e.g. GET `0x0b` → `1e` = 30 while the app showed a 30 s "disabling interval").

| param | setting (SwitchBot app screen) | values | SET example |
|-------|--------------------------------|--------|-------------|
| `0x02`| **Disable Keypad** ⚠️           | `01` = off (keypad active), `02` = on (all buttons dead) | `0f52010202` |
| `0x07`| **Fast Unlock**                | `00` = off, `01` = on (faster, more battery) | `0f52010701` |
| `0x08`| **Recognition Sensitivity**    | `01` = Low, `02` = Medium, `03` = High | `0f52010803` |
| `0x0a`| **Trigger Face Recognition**   | `01` = Auto, `02` = Manual, `03` = Custom | `0f52010a01` |
| `0x0b`| **Disabling Interval**         | raw **seconds**, hex: `00`/`05`/`0f`/`1e`/`3c` = 0/5/15/30/60 s | `0f52010b1e` |
| `0x0c`| **Beep Volume**                | `01` mute / `02` low / `03` medium / `04` high (trailing `02` tag) | `0f52010c0402` |
| `0x0d`| paired with the volume group (GET only) | — | — |

GET-confirmed against the app: `0x08` → `03` (High), `0x0a` → `01` (Auto),
`0x0b` → `1e` (30 s), `0x07` → `00` (Fast Unlock off), `0x0c` → `02 04`.

`0x02` (Disable Keypad) and `0x07` (Fast Unlock) were assigned from the order the
settings screens were visited in the capture; both ended in the state the app
showed. **Verify `0x02` before relying on it — enabling it makes the keypad
inoperable.** To read any value back, GET it, e.g. `0f 53 01 0b`.

Volume (`0x0c`) is the one value that takes two bytes (`0X 02`); every other
setting's value is a single byte. All SETs reply with a bare ack (no payload).

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

For the mapped settings above, the example config also ships **native HA
controls** — `select` entities for volume, sensitivity, face-recognition trigger
and disabling interval, and `switch` entities for Fast Unlock and Disable Keypad
— each wired to the right `0f52` SET through a small `keypad_setting` script that
injects your K14. Set `keypad_k14` in `secrets.yaml` once and they just work.

## Reproducing a capture

1. Android → Developer options → **Enable Bluetooth HCI snoop log**.
2. Operate the keypad from the official SwitchBot app (unlock, change volume, …).
3. Pull `btsnoop_hci.log` (via `adb bugreport` or the vendor path).
4. `python tools/decrypt_capture.py btsnoop_hci.log <your-K14-hex>`

It prints every decrypted command/response and a de-duplicated list of the
distinct commands it saw — a fast way to spot new parameters.
