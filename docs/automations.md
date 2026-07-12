# 🤖 Home Assistant automation cookbook

Ready-to-paste automations for the SwitchBot Keypad Bridge. Two things to adjust
for your setup:

- **Entity IDs** depend on the `name:` you gave each entity and your device
  node name. The examples assume the node `switchbot-keypad-bridge` and the
  names from the README, e.g. `sensor.switchbot_keypad_bridge_keypad_last_seen`
  and `binary_sensor.switchbot_keypad_bridge_keypad_tamper`. Check yours under
  **Developer Tools → States**.
- **Custom event names** come from your `homeassistant.event:` blocks. The
  examples use `esphome.switchbot_keypad_unlock`, `…_tamper`, `…_duress`,
  `…_doorbell` — match whatever you put in the YAML.

The two data sources you'll trigger on:

| Source | When to use |
|---|---|
| Custom events (`on_unlock` → `homeassistant.event`) | You want the rich payload — `method`, `index`, **`name`**. |
| The **Action** event entity (`event.<device>_action`) | You just need "something happened" (Lock / Unlock / Doorbell) with no YAML lambda. |

---

## 👤 Who unlocked

### Announce who opened, and how — one automation for everyone

Reads `name` and `method` off the event, so you never edit it when you add a
person (map names in the **Users** tab or `users:`).

```yaml
alias: Announce who unlocked
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_unlock
actions:
  - variables:
      who: "{{ trigger.event.data.name or ('slot ' ~ trigger.event.data.index) }}"
      how: >-
        {{ {'pin': 'a PIN', 'fingerprint': 'a fingerprint',
            'face': 'face recognition', 'nfc': 'an NFC tag'}
           .get(trigger.event.data.method, trigger.event.data.method) }}
  - action: notify.mobile_app_phone
    data:
      title: "🔓 Door unlocked"
      message: "{{ who }} unlocked the door using {{ how }}."
```

### Per-person action — welcome a specific user

```yaml
alias: Welcome home — Liran
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_unlock
conditions:
  - "{{ trigger.event.data.name == 'Liran' }}"
actions:
  - action: light.turn_on
    target: { entity_id: light.entrance }
  - action: media_player.play_media
    target: { entity_id: media_player.hallway }
    data: { media_content_id: "welcome_liran.mp3", media_content_type: music }
```

### Disarm the alarm only for trusted fingerprints

```yaml
alias: Disarm on trusted fingerprint
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_unlock
conditions:
  - "{{ trigger.event.data.method == 'fingerprint' }}"
  - "{{ trigger.event.data.name in ['Liran', 'Dana'] }}"
actions:
  - action: alarm_control_panel.alarm_disarm
    target: { entity_id: alarm_control_panel.home }
    data: { code: !secret alarm_code }
```

### Only allow a user during certain hours (else alert)

```yaml
alias: Cleaner unlocked outside allowed hours
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_unlock
conditions:
  - "{{ trigger.event.data.name == 'Cleaner' }}"
  - condition: time
    after: "17:00:00"
    before: "08:00:00"
actions:
  - action: notify.mobile_app_phone
    data:
      message: "⚠️ Cleaner unlocked the door outside allowed hours."
```

---

## 🚨 Security alarms (Keypad Vision)

### Tamper — someone pried the keypad off the wall

```yaml
alias: Tamper → siren + notify
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_tamper
actions:
  - action: switch.turn_on
    target: { entity_id: switch.siren }
  - action: notify.mobile_app_phone
    data:
      title: "🚨 Keypad tamper"
      message: "The keypad was moved or removed from its mount!"
```

You can also trigger off the binary sensor's state (survives a bridge reboot):

```yaml
triggers:
  - trigger: state
    entity_id: binary_sensor.switchbot_keypad_bridge_keypad_tamper
    to: "on"
```

### Duress — a panic code was entered (silent)

Enter a pre-registered "duress" code and the keypad flags it. Handle it quietly:
notify, but don't tip off whoever is at the door.

```yaml
alias: Duress code → silent panic
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_duress
actions:
  - action: notify.mobile_app_phone
    data:
      title: "🆘 Duress code entered"
      message: "Someone entered the duress code at the door."
      data: { push: { sound: none } }
  # e.g. also start a camera recording, or a silent call
  - action: camera.record
    target: { entity_id: camera.front_door }
    data: { filename: "/media/duress_{{ now().timestamp() | int }}.mp4", duration: 30 }
```

### Lockout — too many wrong attempts (brute force)

```yaml
alias: Keypad lockout → notify
triggers:
  - trigger: state
    entity_id: binary_sensor.switchbot_keypad_bridge_keypad_lockout
    to: "on"
actions:
  - action: notify.mobile_app_phone
    data:
      message: "🔒 The keypad locked out after too many wrong attempts."
```

---

## 🏠 Presence, doorbell & motion

### Motion at the keypad → porch light (Vision)

```yaml
alias: Keypad motion → porch light
triggers:
  - trigger: state
    entity_id: binary_sensor.switchbot_keypad_bridge_keypad_motion
    to: "on"
actions:
  - action: light.turn_on
    target: { entity_id: light.porch }
    data: { brightness_pct: 100 }
```

### Doorbell → announce + camera snapshot

```yaml
alias: Doorbell
triggers:
  - trigger: event
    event_type: esphome.switchbot_keypad_doorbell
actions:
  - action: camera.snapshot
    target: { entity_id: camera.front_door }
    data: { filename: "/media/doorbell.jpg" }
  - action: notify.mobile_app_phone
    data:
      message: "🔔 Someone is at the door"
      data: { image: "/media/doorbell.jpg" }
```

### Using the Action event entity instead

No custom event needed — trigger on the entity's state change and filter by the
`event_type` attribute (this re-fires on two identical actions in a row, which a
`to:` attribute trigger would miss):

```yaml
alias: Any unlock (Action entity)
triggers:
  - trigger: state
    entity_id: event.switchbot_keypad_bridge_action
conditions:
  - "{{ trigger.to_state.attributes.event_type == 'Unlock' }}"
actions:
  - action: notify.mobile_app_phone
    data: { message: "Door unlocked at {{ now().strftime('%H:%M') }}" }
```

---

## 🔋 Health & maintenance

### Keypad went offline / was removed

`last_seen` refreshes on every advert and every connection, so a long gap means
the keypad is gone, out of range, or flat. Set the window well above your
`battery_scan_interval`.

```yaml
alias: Keypad not seen for a while
triggers:
  - trigger: state
    entity_id: sensor.switchbot_keypad_bridge_keypad_last_seen
    for: "00:45:00"
actions:
  - action: notify.mobile_app_phone
    data: { message: "⚠️ Keypad not seen for 45 minutes — check it." }
```

### Low battery

```yaml
alias: Keypad low battery
triggers:
  - trigger: numeric_state
    entity_id: sensor.switchbot_keypad_bridge_keypad_battery
    below: 20
actions:
  - action: notify.mobile_app_phone
    data: { message: "🔋 Keypad battery at {{ trigger.to_state.state }}% — recharge soon." }
```

### Weak signal (placement helper)

```yaml
alias: Keypad weak BLE signal
triggers:
  - trigger: numeric_state
    entity_id: sensor.switchbot_keypad_bridge_keypad_signal
    below: -85
    for: "00:10:00"
actions:
  - action: notify.mobile_app_phone
    data: { message: "📶 Weak keypad signal ({{ trigger.to_state.state }} dBm) — move the bridge closer." }
```
