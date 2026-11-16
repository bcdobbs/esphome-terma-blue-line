# esphome-terma-blue-line

An [ESPHome](https://esphome.io/) integration that controls a **Terma Blue Line**
Bluetooth heating element (tested on the **Terma MOA Blue**) from an ESP32, and
exposes it to Home Assistant over the native API.

It talks to the radiator directly over BLE — no cloud, no phone app running in the
background — and surfaces the radiator's *own* regulation: element and room
heating, the built-in auto-off timer, and the exact operating mode. The ESP does
**not** run its own thermostat loop; it drives the radiator the same way the
Terma app does and reflects whatever the radiator reports back.

There are two packages to choose from:

- **Full** (`terma.package.yaml`) — everything above, over a persistent connection.
- **On-demand** (`terma-on-demand.package.yaml`) — just on/off at a fixed element
  temperature, connecting only to send each command. For when Home Assistant (e.g. a
  Generic Thermostat with its own room sensor) decides when to heat. See
  [On-demand mode](#on-demand-mode-minimal-package).

Unless a section says otherwise, the setup steps below apply to both.

## Why this exists

The Terma MOA Blue keeps its characteristics behind an **encrypted, bonded** BLE
link and stops advertising once connected. ESPHome's stock `ble_client` connects
but never initiates encryption, so reads fail with *insufficient authentication*
and a naive setup can only talk to the radiator while it's held in pairing mode.

This project ships a tiny external component (`terma_moa`) that requests link
encryption the instant the GATT connection opens, so a persistent, unattended
connection (or an on-demand one) just works after the first pairing. It's built for
the **ESP-IDF** framework.

## Features

- **Element or room heating** — hold the heating element at a target (30–60 °C)
  or let the radiator hold the room air at a target (15–30 °C), using its own
  built-in sensor and regulation.
- **Auto-off timer** — start the radiator's native bounded timer for a chosen
  duration, watch the countdown, and stop it without switching the radiator off
  (`modern`-protocol units only; see [Protocol versions](#protocol-versions)).
- **Two-way state** — Heat Mode, targets, power and status all reflect changes
  made on the physical thermostat or the Terma app, decoded straight from the
  radiator's mode byte.
- **Home-Assistant-friendly primitives** — `Heating Active` and `Timer Active`
  binary sensors let you build policies in HA automations (e.g. *notify me if the
  radiator is heating with no auto-off timer*).
- **Persistent encrypted BLE** — pair once, then reconnect automatically on every
  boot. Nothing is read or written until the link is actually encrypted.
- **Two protocol generations** — `modern` (default) and `legacy` units are both
  supported via one setting.
- **On-demand mode** — a minimal alternative package: one on/off switch at full
  power, connecting over Bluetooth only to send each command (see
  [On-demand mode](#on-demand-mode-minimal-package)).

## Entities exposed

These are the full package's entities. The on-demand package exposes only
**Radiator Power**, **Radiator Status**, **Clear BLE Bonds** and **Restart** (see
[On-demand mode](#on-demand-mode-minimal-package)).

| Entity | Type | Notes |
|---|---|---|
| Radiator Element Temperature | sensor (°C) | Current element temperature |
| Radiator Room Sensor | sensor (°C) | Radiator's built-in room sensor (not lab-accurate) |
| Timer Remaining | sensor (min) | Countdown while a timer runs; 0 when off (`modern` only) |
| Heating Active | binary sensor | On whenever the radiator is heating |
| Timer Active | binary sensor | On while an auto-off timer is counting down (`modern` only) |
| Radiator BLE Connected | binary sensor (diagnostic) | On once the link is encrypted and usable |
| Radiator Status | text sensor | Decoded mode: `Element - Manual`, `Room - Timer`, `Off`, … |
| Heat Mode | select | `Element` / `Room` |
| Element Target Temperature | number | 30–60 °C, 7.5 °C steps (the radiator's 5 physical levels) |
| Room Target Temperature | number | 15–30 °C, 0.5 °C steps |
| Timer Duration | number | 15–180 min — how long **Start Timer** runs |
| Radiator Power | switch | On = manual (continuous) heating in the selected mode |
| Start Timer / Stop Timer | button | Start or cancel the auto-off timer (`modern` only) |
| Clear BLE Bonds / Restart | button (diagnostic) | Re-pairing / reboot |

## Optional: one adaptive control in Home Assistant

*Full package only.*

The integration deliberately exposes **Heat Mode** plus **separate Element/Room
target** numbers, because an ESPHome entity's `min`/`max`/`step` are fixed when it
registers — a single number can't retune its range per mode. If you'd rather drive
the radiator from **one** control whose range follows the mode, do it on the Home
Assistant side; the raw entities keep working, so this is purely optional and needs
**no firmware change**.

In the snippets below, replace the `terma_moa_blue` entity prefix with your own
device's (check Developer Tools → States).

### A thermostat card (`climate_template`, HACS custom component)

Gives a proper thermostat entity — round dial, Element/Room preset, on/off — whose
range flips per mode. Install [jcwillox/hass-template-climate](https://github.com/jcwillox/hass-template-climate)
as a HACS custom repository (category *Integration*) and **restart HA** (this custom
platform loads at startup — it doesn't reload live). Then:

```yaml
climate:
  - platform: climate_template
    name: "Radiator"
    unique_id: terma_radiator_climate
    icon: mdi:radiator
    modes: ["off", "heat"]          # NB: key is `modes`, not `hvac_modes`
    temp_step: 0.5
    # range flips per mode (jcwillox supports templated min/max):
    min_temp_template: "{{ 30 if is_state('select.terma_moa_blue_heat_mode', 'Element') else 15 }}"
    max_temp_template: "{{ 60 if is_state('select.terma_moa_blue_heat_mode', 'Element') else 30 }}"
    current_temperature_template: >-
      {{ states('sensor.terma_moa_blue_radiator_element_temperature')
         if is_state('select.terma_moa_blue_heat_mode', 'Element')
         else states('sensor.terma_moa_blue_radiator_room_sensor') }}
    target_temperature_template: >-
      {{ states('number.terma_moa_blue_element_target_temperature')
         if is_state('select.terma_moa_blue_heat_mode', 'Element')
         else states('number.terma_moa_blue_room_target_temperature') }}
    hvac_mode_template: "{{ 'heat' if is_state('switch.terma_moa_blue_radiator_power', 'on') else 'off' }}"
    preset_modes: ["Element", "Room"]
    preset_mode_template: "{{ states('select.terma_moa_blue_heat_mode') }}"
    set_hvac_mode:
      - choose:
          - conditions: "{{ hvac_mode == 'heat' }}"
            sequence:
              - service: switch.turn_on
                target: { entity_id: switch.terma_moa_blue_radiator_power }
        default:
          - service: switch.turn_off
            target: { entity_id: switch.terma_moa_blue_radiator_power }
    set_preset_mode:
      - service: select.select_option
        target: { entity_id: select.terma_moa_blue_heat_mode }
        data: { option: "{{ preset_mode }}" }
    set_temperature:
      - choose:
          - conditions: "{{ is_state('select.terma_moa_blue_heat_mode', 'Element') }}"
            sequence:
              - service: number.set_value
                target: { entity_id: number.terma_moa_blue_element_target_temperature }
                # snap to the nearest 7.5° element level, clamped 30–60:
                data: { value: "{{ [[ ((temperature / 7.5) | round(0)) * 7.5, 30] | max, 60] | min }}" }
        default:
          - service: number.set_value
            target: { entity_id: number.terma_moa_blue_room_target_temperature }
            data: { value: "{{ temperature }}" }
```

Notes: the dial uses a single 0.5° step, so Element targets **snap to the nearest
valid level** (30 / 37.5 / 45 / 52.5 / 60) on write. The auto-off timer has no
climate equivalent — keep using the **Start/Stop Timer** buttons. `climate_template`
is community-maintained; after HA upgrades, glance at the logs for deprecations.

### Lighter alternative: a template number (core HA, no custom component)

If you don't want a custom component, a core [`template`](https://www.home-assistant.io/integrations/template/)
number gives you one control whose `min`/`max`/**and step** all follow the mode
(the step adaptivity a `climate` entity can't do), and it reloads live:

```yaml
template:
  - number:
      - name: "Radiator Target"
        unit_of_measurement: "°C"
        min:  "{{ 30  if is_state('select.terma_moa_blue_heat_mode', 'Element') else 15 }}"
        max:  "{{ 60  if is_state('select.terma_moa_blue_heat_mode', 'Element') else 30 }}"
        step: "{{ 7.5 if is_state('select.terma_moa_blue_heat_mode', 'Element') else 0.5 }}"
        state: >-
          {{ states('number.terma_moa_blue_element_target_temperature')
             if is_state('select.terma_moa_blue_heat_mode', 'Element')
             else states('number.terma_moa_blue_room_target_temperature') }}
        set_value:
          - if: "{{ is_state('select.terma_moa_blue_heat_mode', 'Element') }}"
            then:
              - action: number.set_value
                target: { entity_id: number.terma_moa_blue_element_target_temperature }
                data: { value: "{{ value }}" }
            else:
              - action: number.set_value
                target: { entity_id: number.terma_moa_blue_room_target_temperature }
                data: { value: "{{ value }}" }
```

## Room temperature: calibration and a Home Assistant thermostat

*Full package.* If you have a separate room sensor and want Home Assistant to do all
the control, the [on-demand package](#on-demand-mode-minimal-package) is simpler.

The radiator's built-in room sensor sits in its controller, so it reads high, and
more so once the element is hot (e.g. 24.9 °C cold vs 28.3 °C after heating, in the
same room). Two things help.

**Calibrate the reading** with an ESPHome filter in your device YAML. Each line is
*what the radiator reports → what a trusted thermometer says*; take one pair with the
rail off and cold and one after ~30 min of heating:

```yaml
sensor:
  - id: !extend room_temp_raw
    filters:
      - calibrate_linear:
          - 24.9 -> 21.5   # rail off and cold
          - 28.3 -> 22.0   # rail on and hot
```

(A single `- offset: -3.0` works too, but won't track the heating error.) This only
changes the value Home Assistant sees: in **Room** mode the radiator still regulates
on its own uncorrected sensor.

**Let Home Assistant do the room control** instead of the radiator's Room mode. Leave
**Heat Mode** on **Element** and wrap the switch and the calibrated sensor in HA's
built-in Generic Thermostat (Settings → Helpers → Generic thermostat, or YAML):

```yaml
climate:
  - platform: generic_thermostat
    name: Bathroom
    heater: switch.terma_moa_blue_radiator_power
    target_sensor: sensor.terma_moa_blue_radiator_room_sensor
    target_temp: 22
    min_temp: 15
    max_temp: 26
    cold_tolerance: 0.5
    hot_tolerance: 0.5
    min_cycle_duration:
      minutes: 10   # avoid short-cycling; legacy units re-run off -> target -> mode on every start
```

You get a real thermostat entity (and a thermostat tile in Apple Home via the HomeKit
Bridge). Because the sensor sits in the rail, it jumps when heating starts, so keep
the cycle duration and tolerances. For better control, swap `target_sensor` for any
separate room sensor away from the rail.

## On-demand mode (minimal package)

If Home Assistant makes all the decisions (a Generic Thermostat with its own room
sensor, say), you don't need any of the radiator's modes, targets or timers - just on
and off. `terma-on-demand.package.yaml` does only that, and **doesn't hold a Bluetooth
connection**: the ESP connects to send a command, checks it took, and disconnects.
That leaves the radiator free between commands (the phone app works again) and stops
the high-pitched whine some units make while something is connected.

Use it in place of the full package:

```yaml
packages:
  terma:
    url: https://github.com/bcdobbs/esphome-terma-blue-line
    ref: main
    files: [terma-on-demand.package.yaml]
```

`examples/m5stack-atoms3-lite-on-demand.yaml` is a complete device config.

### Setting the element temperature

The element runs at 60 °C (full power) while on. To run it cooler, set the
`element_temp` substitution in your device YAML:

```yaml
substitutions:
  element_temp: "52.5"
```

| `element_temp` | Radiator button level |
|---|---|
| `"30"` | 1 (lowest) |
| `"37.5"` | 2 |
| `"45"` | 3 |
| `"52.5"` | 4 |
| `"60"` | 5 - full power (default) |

Any value from 30 to 60 works (it's sent to 0.1 °C); values outside that range are
clamped to 30 or 60. The five button levels are the values the radiator itself uses,
so they're the safest choice.

| Entity | Notes |
|---|---|
| Radiator Power (switch) | On = element mode at `element_temp` (default 60 °C, full power); off = off |
| Radiator Status (text) | `On` / `Off` once confirmed, `Switching on`, `Retrying on`, or a fault |
| Clear BLE Bonds / Restart | diagnostic buttons |

How it behaves:

- **Every command** connects and reads the radiator's state first. If it's already
  as asked, nothing is written. Otherwise it switches off, (for on) writes the element
  target and element mode, reads back to confirm, then disconnects - a few seconds.
- **Failures are retried** 3 times, then once a minute until confirmed. The switch
  shows what was asked for; Radiator Status shows what was confirmed.
- **Safety:** if the radiator reports an element target above 60 °C after switching on
  (the legacy doubled-target fault), it is switched straight off and the status says so.
- **On boot** the radiator is switched off. A Generic Thermostat turns it back on at its
  next sensor update if it still needs heat.
- **The radiator's own buttons** aren't noticed until the next command - see
  [Recommended thermostat setup](#recommended-thermostat-setup) below.
- Switching from the full package: delete any `!extend room_temp_raw` calibration
  block from your device YAML (that sensor no longer exists). The bond is kept, so no
  re-pairing. **Radiator Power**, **Radiator Status**, **Clear BLE Bonds** and
  **Restart** keep their entity IDs. Everything else the full package exposed goes
  unavailable in Home Assistant: first move anything that uses those entities (a
  thermostat's sensor, dashboards, automations, HomeKit Bridge) to something else,
  then delete them under Settings → Entities (a Home Assistant restart may be needed
  before Delete is enabled).

### Recommended thermostat setup

Pair the package with Home Assistant's **Generic Thermostat** (Settings → Helpers →
Create helper → Generic thermostat) and a room sensor **away from the rail**:

| Setting | Value | Why |
|---|---|---|
| Heater | **Radiator Power** | |
| Temperature sensor | your room sensor | the radiator's built-in one reads high and isn't exposed here |
| Cold / hot tolerance | 0.5 | stops it switching on every 0.1 °C wobble |
| Minimum cycle duration | 10 min | limits how often the ESP connects |
| **Keep alive** | **30 min** | corrects presses on the radiator's own buttons (below) |

Or in YAML:

```yaml
climate:
  - platform: generic_thermostat
    name: Bathroom
    heater: switch.terma_moa_blue_radiator_power
    target_sensor: sensor.bathroom_temperature   # your room sensor
    target_temp: 22
    cold_tolerance: 0.5
    hot_tolerance: 0.5
    min_cycle_duration: { minutes: 10 }
    keep_alive: { minutes: 30 }
```

**Why keep alive matters.** The ESP doesn't hold a connection, so it can't see the
radiator's own buttons. If someone switches the radiator on by hand, Home Assistant
still thinks it's off - and when the room passes the target, the thermostat won't send
"off", because as far as it knows the heater is already off. The radiator would keep
heating until the thermostat next turns it on and off itself. Keep alive makes the
thermostat re-send its current on/off state on a schedule, so a manual change is
corrected within that interval.

Each re-send is one short connection (a few seconds). Because the package checks
first, a re-send to a radiator that's already in the right state writes nothing - it
only acts if something was changed at the radiator. If your radiator whines while
connected and that's noticeable, lengthen the interval - 60 min is a reasonable
compromise.

**Not covered:** if Home Assistant itself goes down while the radiator is on, it stays
on at `element_temp` until Home Assistant returns - the same as leaving it on from its
own button. The radiator still regulates its element, so it never runs hotter than
that.

## Hardware

- Any **ESP32** with Bluetooth LE. Developed on a **Seeed XIAO ESP32-C6**
  (`board: seeed_xiao_esp32c6`); a classic `esp32dev` board works too — just set
  `board:` to match yours.
- A **Terma Blue Line** BLE radiator / heating element (MOA Blue confirmed).

No extra wiring is needed. The full package uses the radiator's own temperature
sensors; the on-demand package needs a room temperature sensor in Home Assistant for
the thermostat (any kind - Zigbee, Matter, Thread, …), ideally away from the rail.

## Installation

You don't copy the big config — your device imports it from this repo over git.
In **ESPHome Builder** (or a local ESPHome), create a device with a config like
[`terma-moa-blue.example.yaml`](terma-moa-blue.example.yaml):

```yaml
substitutions:
  device_name: terma-moa-blue
  friendly_name: "Terma MOA Blue"
  board: esp32dev
  radiator_mac: !secret radiator_mac

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}

esp32:
  board: ${board}
  framework:
    type: esp-idf          # required by the terma_moa component

logger:
api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
    password: !secret ota_password
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source: github://bcdobbs/esphome-terma-blue-line@main
    components: [terma_moa]

packages:
  terma: github://bcdobbs/esphome-terma-blue-line/terma.package.yaml@main
```

1. **Create your secrets.** Copy `secrets.yaml.example` to `secrets.yaml` and fill
   in the section for your config: Wi-Fi, your radiator's MAC, an API encryption key,
   and (for this config) an OTA password and a fallback hotspot password. Don't know the MAC? See
   [Finding your radiator's MAC](#finding-your-radiators-mac).
2. **Set `board:`** to your ESP32. Leave `framework: esp-idf` — the component
   requires it.
3. **Flash** with ESPHome (`esphome run terma-moa-blue.example.yaml`), or just
   Install from ESPHome Builder.

**For the on-demand package**, use `terma-on-demand.package.yaml` in `packages:`
instead (and optionally set `element_temp`). A complete config is in
[`examples/m5stack-atoms3-lite-on-demand.yaml`](examples/m5stack-atoms3-lite-on-demand.yaml).

**Pin the version.** Both git references compile code from this repo straight into
your firmware, and `@main` follows whatever the latest commit is. For reproducible
builds that only change when you decide, replace `main` in both with a release tag
(e.g. `@v1.0.0`) or a full commit SHA.

[`secrets.yaml.example`](secrets.yaml.example) lists the secrets each example uses.
The AtomS3 examples use device-prefixed names (`terma_moa_blue__radiator_mac`, …),
because ESPHome Builder shares one secrets file between all devices, and secure OTA
with the API key instead of a separate password. Any names work as long as the
`!secret` names match your secrets file.

### Protocol versions

Terma units speak one of two generations of the BLE protocol. Set the one yours uses
under `substitutions` (the default is `modern`):

```yaml
  protocol: legacy
```

| | `modern` (default) | `legacy` |
|---|---|---|
| 16-bit values (temps × 10, timer) | little-endian | big-endian |
| Mode characteristic (b3) | 4 bytes | 1 byte |
| Room / element mode byte | `0x11` / `0x21` | `5` / `6` |
| Auto-off timer | yes (`0x03` / `0x05` / `0x09`) | no |
| Prior work | this project | J1mbo's [homebridge plugin](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue) |

Signs that your radiator is `legacy`:

- The logs show a `terma_moa` warning that the radiator looks like the other protocol.
  This check runs automatically and is the quickest sign.
- The temperatures are nonsense: for example **Radiator Element Temperature** shows
  `2995.3 °C` where the raw log line is `b2 element raw (4 bytes): 01.75.02.58`.
  Read big-endian, that's 37.3 °C current and 60.0 °C target. (Raw payloads are logged
  at `VERBOSE`: set `logger: level: VERBOSE` to see them.)
- The raw log shows `b3 mode raw (1 bytes)`. Modern units send 4 bytes.
- **Radiator Power** won't stay on. A modern-protocol write (`0x21 00 00 00`, with a
  byte-swapped target) means nothing to a legacy unit, so it stays off and the next
  poll flips the switch back.
- Pressing the radiator's own button gives `Operating mode: 0x06` (element) or `0x05`
  (room).
- On the **on-demand** package (no temperature entities): every command ends in
  `Radiator reports mode 0x.. - not what was asked` and retries, and Radiator Status
  stays on `Retrying on` / `Retrying off`.

On `legacy`, the integration follows J1mbo's safeguard: these units can end up with a
**doubled target** if switched straight between room and element mode, so every
change goes **off → target → mode** and is then read back. If a read-back target is
above the radiator's maximum (30 °C room, 60 °C element), it switches the radiator
off and logs an error. (The on-demand package does the same, on every unit.) **Start Timer** / **Stop Timer** only log a warning on `legacy`
units, **Timer Active** stays off, and **Timer Remaining** is never read.

On `modern` units, changing **Heat Mode** while a timer is running switches the
radiator to continuous heating in the new mode, which cancels the timer (a warning is
logged). Press **Start Timer** again if you still want one.

A complete, tested config for an **M5Stack AtomS3 Lite** (ESP32-S3) is in
[`examples/m5stack-atoms3-lite.yaml`](examples/m5stack-atoms3-lite.yaml). It uses
`variant:` rather than `board:`, OTA secured by the API key, `protocol: legacy`, and
the long-form git sources with explicit `ref` and `refresh`.

### Finding your radiator's MAC

The MAC may be on a label on the element. If not, have the ESP find it. iPhones
can't help here, because iOS hides Bluetooth MAC addresses.

1. Put a placeholder in the `radiator_mac` secret (e.g. `"00:00:00:00:00:00"`) so the
   config compiles.
2. Add this to your device YAML (it's in the AtomS3 example, commented out) and
   install:

   ```yaml
   esp32_ble_tracker:
     on_ble_advertise:
       - then:
           - lambda: |-
               if (x.get_name().find("TERMA") != std::string::npos) {
                 ESP_LOGI("find_terma", "Found '%s' MAC: %s RSSI: %d",
                          x.get_name().c_str(), x.address_str().c_str(), x.get_rssi());
               }
   ```

   On the **on-demand** package the ESP only scans for the first 5 minutes after
   boot, so do the next step within 5 minutes of installing (or restart it to try
   again).

3. Close the Terma app or turn off your phone's Bluetooth. The radiator stops
   advertising while anything is connected. Press its button to wake it, then watch
   the logs for `[find_terma] Found 'MOA Blue TERMA' MAC: XX:XX:XX:XX:XX:XX`.
4. Put that MAC in the secret, remove the block, and install again with the radiator
   in pairing mode (see below).

### First pairing

The radiator only bonds when it's in **pairing mode**:

1. Put the radiator into Bluetooth pairing mode (see the Terma manual — usually a
   long-press until the BLE indicator flashes).
2. Boot the ESP while it's flashing. It connects and negotiates encryption using PIN
   **`123456`**. The PIN is fixed by the radiator itself (the Terma Blue Line
   default), so there's nothing to configure. (The on-demand package connects at boot
   too, to switch the radiator off; if pairing mode ends first, put it back in
   pairing mode and toggle **Radiator Power**.)
3. Once bonded, later connections need no pairing mode.

To re-pair (e.g. after replacing the ESP), press **Clear BLE Bonds**, put the radiator
back in pairing mode, and restart the ESP.

### Local development (for contributors)

To hack on the C++ component or the entities and flash without pushing to git
first, create a personal `dev.yaml` (gitignored) that includes the package
locally and points `external_components` at your working copy:

```yaml
substitutions:
  device_name: terma-dev
  friendly_name: "Terma Dev"
  board: esp32dev
  radiator_mac: !secret radiator_mac

esphome:
  name: ${device_name}
esp32:
  board: ${board}
  framework:
    type: esp-idf
logger:
api:
  encryption:
    key: !secret api_encryption_key
ota:
  - platform: esphome
    password: !secret ota_password
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source:
      type: local
      path: components

packages:
  terma: !include terma.package.yaml
```

`esphome run dev.yaml` then picks up local edits to both `components/` and
`terma.package.yaml` immediately. To work on the on-demand package, include
`terma-on-demand.package.yaml` instead.

## Protocol reference

Everything below was reverse-engineered from the community efforts credited at the
bottom, plus BLE snoops of the Terma app confirming the room path and the timer
sequence. Values verified against a MOA Blue.

**Service:** `d97352b0-d19e-11e2-9e96-0800200c9a66`

| Char | UUID suffix | Meaning |
|---|---|---|
| b1 | `d97352b1-…` | Room: current temp (bytes 0–1), target (bytes 2–3) |
| b2 | `d97352b2-…` | Element: current temp (bytes 0–1), target (bytes 2–3) |
| b3 | `d97352b3-…` | Operating mode (byte 0, a bitfield) |
| b6 | `d97352b6-…` | Timer: remaining seconds (bytes 0–1), total seconds (bytes 2–3) |

**Temperature encoding** — `uint16`, value ÷ 10 = °C; little-endian on `modern`
units, big-endian on `legacy` ones (see [Protocol versions](#protocol-versions)). The
examples below are `modern`. To write a target, put it in bytes 2–3 with bytes 0–1 zero:

```
21.5 °C → 215 → 0xD7 0x00 → write {0x00, 0x00, 0xD7, 0x00}
```

**Operating-mode byte (b3, byte 0)** — a bitfield, read directly:

| Value | Meaning |
|---|---|
| `0x00` | Off |
| `0x20` | Off (via app / remote) |
| `0x01` | On |
| `0x11` | Room — manual (continuous) |
| `0x21` | Element — manual (continuous) |
| `0x03` | Room — timer |
| `0x05` | Element — timer |
| `0x09` | Timer (started on the physical thermostat) |

**Starting the auto-off timer** reproduces the write sequence the Terma app sends;
the final run-mode write is what actually starts the countdown:

1. Manual mode → b3 = `{0x11,…}` (room) or `{0x21,…}` (element)
2. Duration → b6 = `{secs, secs}` (remaining and total both = duration)
3. Target temp → b1 (room) or b2 (element)
4. Run mode → b3 = `{0x03,…}` (room) or `{0x05,…}` (element)

**Encryption** — the radiator refuses to serve its characteristics over an
unencrypted link. ESPHome does not initiate encryption on its own, so the
`terma_moa` component calls `esp_ble_set_encryption` on `ESP_GATTC_OPEN_EVT`. This
is the whole reason the component exists. It then waits for both the pairing result
(`ESP_GAP_BLE_AUTH_CMPL_EVT`) and service discovery before reporting the link ready:
`id(terma_hub).is_ready()` gates every write and every polled read (the on-demand
package waits on it after each connect), and its `on_ready:` trigger refreshes the
status. (ESPHome itself reads each characteristic
once right after service discovery.)

## Extending it (new models / features)

Want to add support for another Terma Blue Line model, or a feature this doesn't
cover yet (e.g. the weekly schedule)? See
**[docs/reverse-engineering.md](docs/reverse-engineering.md)** — it walks through
how the protocol above was worked out: browsing the GATT layout (nRF Connect /
verbose ESPHome logs), reading and poking characteristics from the ESP, and
snooping the official app (PacketLogger / HCI snoop + tshark) to capture write
sequences. PRs updating the protocol table are welcome.

## Acknowledgments

This repository is a fork of Kamil Golec's
[kumalg/esphome-terma-blue-line](https://github.com/kumalg/esphome-terma-blue-line),
adding support for `legacy`-protocol radiators. That project began as a fork of
Andrew's [terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome)
and grew into a standalone ESP-IDF rewrite with room/element control, a full timer
implementation, and exact mode decoding. Huge thanks to:

- [kumalg/esphome-terma-blue-line](https://github.com/kumalg/esphome-terma-blue-line) — the ESP-IDF rewrite this fork builds on
- [Andrew-a-g/terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome) — the original ESPHome project this builds on
- [Home Assistant Community thread](https://community.home-assistant.io/t/terma-blue-line-bluetooth-radiators-and-heating-elements/81325) — the protocol reverse-engineering groundwork
- [J1mbo/homebridge-TERMA-MOA-Blue](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue/) — a great HomeKit solution and protocol reference
- [ESPHome](https://esphome.io/) — the firmware framework
- [Anthropic Claude](https://claude.ai/) — AI assistance with the ESP-IDF component and reverse-engineering

## License

[MIT](LICENSE). Includes the copyright notices from the upstream projects.

## Disclaimer

Unofficial integration, not affiliated with or endorsed by Terma. Use at your own
risk, and follow the usual safety guidelines around electrical heating equipment.
