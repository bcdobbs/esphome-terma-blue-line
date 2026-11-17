# esphome-terma-blue-line

An [ESPHome](https://esphome.io/) integration that switches a **Terma Blue Line**
Bluetooth heating element (tested on the **Terma MOA Blue**) on and off from an
ESP32, for a Home Assistant thermostat to drive.

This is a cut-down fork of Kamil Golec's
[kumalg/esphome-terma-blue-line](https://github.com/kumalg/esphome-terma-blue-line),
which exposes the radiator's own room/element modes, targets and auto-off timer over
a persistent connection. If you want those, use the upstream project. This fork keeps
only on/off control, connects only when it needs to, and adds support for
`legacy`-protocol radiators.

Home Assistant makes the decisions (typically a Generic Thermostat with its own room
sensor); the ESP exposes one **Radiator Power** switch. On runs the element at a fixed
temperature (60 °C, full power, by default); off is off. The ESP **doesn't hold a
Bluetooth connection**: it connects to send a command, checks it took, and
disconnects. That leaves the radiator free between commands (the Terma app still
works) and avoids the high-pitched whine some units make while something is
connected. No cloud, no phone app running in the background.

## Why this exists

The Terma MOA Blue keeps its characteristics behind an **encrypted, bonded** BLE
link and stops advertising once connected. ESPHome's stock `ble_client` connects
but never initiates encryption, so reads fail with *insufficient authentication*
and a naive setup can only talk to the radiator while it's held in pairing mode.

This project ships a tiny external component (`terma_moa`) that requests link
encryption the instant the GATT connection opens, so connections just work after the
first pairing. It's built for the **ESP-IDF** framework.

## Entities

| Entity | Type | Notes |
|---|---|---|
| Radiator Power | switch | On = element heating at `element_temp`; off = off |
| Radiator Status | text sensor | `On` / `Off` once confirmed, `Switching on`, `Retrying on`, or a fault |
| Clear BLE Bonds | button (diagnostic) | For re-pairing |
| Restart | button (diagnostic) | Reboots the ESP |

## How it behaves

- **Every command** connects and reads the radiator's state first. If it's already
  as asked, nothing is written. Otherwise it switches off, (for on) writes the element
  target and element mode, reads back mode and target to confirm, then disconnects -
  a few seconds.
- **Failures are retried:** up to 3 attempts straight away, then once a minute until
  confirmed. The switch shows what was asked for; Radiator Status shows what was
  confirmed. A failed connection is given up on as soon as it fails, so a retry
  follows within seconds.
- **Safety:** if the radiator reports an element target above 60 °C after switching on
  (the legacy doubled-target fault, see [Protocol versions](#protocol-versions)), it
  is switched straight off and retried until a read-back confirms it's off. Radiator
  Status shows the fault until the next "on" command.
- **On boot** the radiator is switched off. A Generic Thermostat turns it back on at its
  next sensor update if it still needs heat.
- **The radiator's own buttons** aren't noticed until the next command - see
  [Recommended thermostat setup](#recommended-thermostat-setup).

## Setting the element temperature

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

## Recommended thermostat setup

Pair it with Home Assistant's **Generic Thermostat** (Settings → Helpers → Create
helper → Generic thermostat) and a room sensor **away from the rail**:

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

Each re-send is one short connection (a few seconds). Because the ESP checks first, a
re-send to a radiator that's already in the right state writes nothing - it only acts
if something was changed at the radiator. If your radiator whines while connected and
that's noticeable, lengthen the interval - 60 min is a reasonable compromise.

**Not covered:** if Home Assistant itself goes down while the radiator is on, it stays
on at `element_temp` until Home Assistant returns - the same as leaving it on from its
own button. The radiator still regulates its element, so it never runs hotter than
that.

## Hardware

- Any **ESP32** with Bluetooth LE, within Bluetooth range of the radiator. Tested on an
  **M5Stack AtomS3 Lite** (ESP32-S3).
- A **Terma Blue Line** BLE radiator / heating element (MOA Blue confirmed).
- A **room temperature sensor** in Home Assistant for the thermostat (any kind -
  Zigbee, Matter, Thread, …), ideally away from the rail.

No wiring needed.

## Installation

Your device imports the package from this repo over git, so its own config stays
short. [`examples/m5stack-atoms3-lite.yaml`](examples/m5stack-atoms3-lite.yaml) is a
complete config:

```yaml
substitutions:
  device_name: terma-moa-blue
  radiator_mac: !secret terma_moa_blue__radiator_mac
  protocol: legacy        # delete if your radiator uses the modern protocol
  element_temp: "60"

esphome:
  name: ${device_name}

esp32:
  variant: esp32s3
  flash_size: 8MB
  framework:
    type: esp-idf          # required by the terma_moa component
    advanced:
      flash_chip: generic
    sdkconfig_options:     # full Bluetooth transmit power - see "Reading the logs"
      CONFIG_BT_CTRL_DFT_TX_POWER_LEVEL_P20: y

logger:

api:
  encryption:
    key: !secret terma_moa_blue__api_encryption_key

ota:
  - platform: esphome
    encryption:            # OTA secured by the API key

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

external_components:
  - source:
      type: git
      url: https://github.com/bcdobbs/esphome-terma-blue-line
      ref: main
    components: [terma_moa]
    refresh: 1d

packages:
  terma:
    url: https://github.com/bcdobbs/esphome-terma-blue-line
    ref: main
    files: [terma-on-demand.package.yaml]
    refresh: 1d
```

1. **Create your secrets.** Copy [`secrets.yaml.example`](secrets.yaml.example) to
   `secrets.yaml` (or paste the lines into ESPHome Builder's Secrets) and fill in
   Wi-Fi, your radiator's MAC and an API encryption key. Don't know the MAC? See
   [Finding your radiator's MAC](#finding-your-radiators-mac).
2. **Match your board.** For another ESP32, change the `esp32:` block (e.g.
   `board: esp32dev`). Keep `framework: esp-idf` - the component requires it.
3. **Set `protocol`** - see [Protocol versions](#protocol-versions).
4. **Install** from ESPHome Builder (the first install over USB), with the radiator in
   pairing mode - see [First pairing](#first-pairing).
5. **Add it to Home Assistant** (Settings → Devices & services → ESPHome) and set up
   the [thermostat](#recommended-thermostat-setup).

**Pin the version.** Both git references compile code from this repo straight into
your firmware, and `ref: main` follows whatever the latest commit is. For builds that
only change when you decide, set `ref` to a release tag or a full commit SHA.
`refresh: 1d` re-checks git once a day; set it to `0s` while testing new commits.

## Protocol versions

Terma units speak one of two generations of the BLE protocol. Set the one yours uses
under `substitutions` (the default is `modern`):

```yaml
  protocol: legacy
```

| | `modern` (default) | `legacy` |
|---|---|---|
| 16-bit values (temps × 10) | little-endian | big-endian |
| Mode characteristic (b3) | 4 bytes | 1 byte |
| Element mode byte | `0x21` | `6` |
| Prior work | [kumalg](https://github.com/kumalg/esphome-terma-blue-line) | J1mbo's [homebridge plugin](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue) |

Signs the setting is wrong:

- The logs show a `terma_moa` warning that the radiator looks like the other protocol.
  This check runs on every read-back and is the quickest sign.
- Every command ends in `Radiator reports mode 0x.. - not what was asked` and retries,
  and Radiator Status stays on `Retrying on` / `Retrying off`.

Every change goes **off → target → mode** and is then read back. That follows J1mbo's
safeguard for `legacy` units, which can end up with a **doubled target** when the mode
is changed directly; if the read-back target is above 60 °C, the radiator is switched
off (and checked) and Radiator Status reports a fault.

## Reading the logs

A healthy command looks like this (info level):

```
[terma] Turning radiator ON (attempt 1)
[esp32_ble_client] ... Connecting
[esp32_ble_client] ... Connection open
[terma_moa] Link encryption requested
[terma_moa] Radiator signal: -71 dBm
[esp32_ble_client] ... auth complete
[esp32_ble_client] ... Service discovery complete
[terma_moa] Link encrypted and services discovered - radiator ready
[terma] Radiator is in mode 0x00 (target 60.0 C) - changing it
[terma] Confirmed ON (mode 0x06, target 60.0 C)
[esp32_ble_client] ... Disconnecting (conn_id: 0).
```

**`Connection open error, status=133`** followed by `Could not connect to the
radiator` means the Bluetooth link couldn't be set up: the ESP heard the radiator
and asked to connect, but the radiator didn't answer. It's retried automatically,
and an occasional one is harmless. If you see them often:

- **Turn up the ESP's Bluetooth transmit power.** This made the biggest difference on
  the test unit. The radiator hears the ESP less well than the ESP hears it -
  especially while its element is heating, so "off" commands suffered most: connects
  failed for several minutes after switching on, and service discovery took 5-15 s.
  At full power every command connected first time, with discovery in 1-2 s. On an
  **ESP32-S3** or **ESP32-C3** (default +9 dBm, maximum +20 dBm), add this to the
  `esp32:` block of your device YAML, as the
  [example](examples/m5stack-atoms3-lite.yaml) does:

  ```yaml
  esp32:
    framework:
      sdkconfig_options:
        CONFIG_BT_CTRL_DFT_TX_POWER_LEVEL_P20: y
  ```

  The original ESP32 has no such option.
- **Check the `Radiator signal` line.** It measures the radiator's signal at the ESP
  (so only one direction of the link). Around -80 dBm is fine with full transmit
  power; below -85 dBm it's logged as a warning. If so, move the ESP closer or into
  line of sight. On an ESP32, Wi-Fi and Bluetooth share one radio, so a weak Wi-Fi
  signal (shown in the ESP's startup log) makes Bluetooth worse too.
- **Close the Terma app.** The radiator accepts one connection at a time, so a phone
  connected to it locks the ESP out.

## Finding your radiator's MAC

The MAC may be on a label on the element. If not, have the ESP find it. iPhones
can't help here, because iOS hides Bluetooth MAC addresses.

1. Put a placeholder in the `terma_moa_blue__radiator_mac` secret (e.g.
   `"00:00:00:00:00:00"`) so the config compiles.
2. Add this to your device YAML (it's in the example, commented out) and install:

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

   The ESP only scans for the first 5 minutes after boot, so do the next step within
   that time (or restart it to try again).
3. Close the Terma app or turn off your phone's Bluetooth. The radiator stops
   advertising while anything is connected. Press its button to wake it, then watch
   the logs for `[find_terma] Found 'MOA Blue TERMA' MAC: XX:XX:XX:XX:XX:XX`.
4. Put that MAC in the secret, remove the block, and install again with the radiator
   in pairing mode (see below).

## First pairing

The radiator only bonds when it's in **pairing mode**:

1. Put the radiator into Bluetooth pairing mode (see the Terma manual - usually a
   long-press until the BLE indicator flashes).
2. Boot the ESP while it's flashing. It connects at boot (to switch the radiator off)
   and negotiates encryption using PIN **`123456`**. The PIN is fixed by the radiator
   itself (the Terma Blue Line default), so there's nothing to configure. If pairing
   mode ends first, put it back in pairing mode and toggle **Radiator Power**.
3. Once bonded, later connections need no pairing mode.

To re-pair (e.g. after replacing the ESP), press **Clear BLE Bonds**, put the radiator
back in pairing mode, and restart the ESP.

## Moving from the full package

Earlier versions of this repo (and [kumalg's upstream](https://github.com/kumalg/esphome-terma-blue-line))
had a full package with room/element modes, targets and timers over a persistent
connection. The last of them is tag
[`v1.0.0`](https://github.com/bcdobbs/esphome-terma-blue-line/tree/v1.0.0) (also the
`full-package` branch); point both `ref:` lines at it to keep using it.

To switch: point `packages:` at `terma-on-demand.package.yaml` and delete any
`!extend room_temp_raw` calibration block from your device YAML. The bond is kept,
so no re-pairing.

**Radiator Power**, **Radiator Status**, **Clear BLE Bonds** and **Restart** keep their
entity IDs. Everything else goes unavailable in Home Assistant: first move anything
that uses those entities (a thermostat's sensor, dashboards, automations, HomeKit
Bridge) to something else, then delete them under Settings → Entities (a Home
Assistant restart may be needed before Delete is enabled).

## Local development

To work on the component or the package without pushing to git first, create a
personal `dev.yaml` (gitignored) that points at your working copy - the example
config with these two blocks swapped in:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [terma_moa]

packages:
  terma: !include terma-on-demand.package.yaml
```

`esphome run dev.yaml` then picks up local edits immediately.

## Protocol reference

Reverse-engineered by the community efforts credited below, and verified against a
MOA Blue. Only the parts this integration uses are listed.

**Service:** `d97352b0-d19e-11e2-9e96-0800200c9a66`

| Char | UUID | Meaning |
|---|---|---|
| b2 | `d97352b2-d19e-11e2-9e96-0800200c9a66` | Element: current temp (bytes 0–1), target (bytes 2–3) |
| b3 | `d97352b3-d19e-11e2-9e96-0800200c9a66` | Operating mode (byte 0) |

**Temperatures** are `uint16`, value ÷ 10 = °C, in the protocol's byte order. To write
a target, put it in bytes 2–3 with bytes 0–1 zero. 60.0 °C → 600 → `0x0258`:

```
modern: {0x00, 0x00, 0x58, 0x02}
legacy: {0x00, 0x00, 0x02, 0x58}
```

**Mode byte (b3, byte 0):**

| Value | `modern` | `legacy` |
|---|---|---|
| `0x00` | Off | Off |
| `0x20` | Off (via app / remote) | - |
| `0x21` / `0x06` | Element - continuous | Element |

Modern units also report room and timer modes (e.g. `0x05` = element with the auto-off
timer running). This integration treats anything other than continuous element mode
as "not on" and rewrites it, so a timer started from the app can't switch the
radiator off behind the thermostat's back.

**Encryption** - the radiator refuses to serve its characteristics over an
unencrypted link. ESPHome does not initiate encryption on its own, so the `terma_moa`
component calls `esp_ble_set_encryption` on `ESP_GATTC_OPEN_EVT`. It then waits for
both the pairing result (`ESP_GAP_BLE_AUTH_CMPL_EVT`) and service discovery before
reporting the link ready: `id(terma_hub).is_ready()`, which the package waits on after
each connect. The component also does the read-backs itself
(`id(terma_hub).request_read()`, mode then element), so no `ble_client` sensors are
involved.

## Acknowledgments

This repository is a fork of Kamil Golec's
[kumalg/esphome-terma-blue-line](https://github.com/kumalg/esphome-terma-blue-line),
cut down to on/off control and adding support for `legacy`-protocol radiators. That
project began as a fork of Andrew's
[terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome) and grew
into a standalone ESP-IDF rewrite. Huge thanks to:

- [kumalg/esphome-terma-blue-line](https://github.com/kumalg/esphome-terma-blue-line) — the ESP-IDF rewrite this fork builds on
- [Andrew-a-g/terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome) — the original ESPHome project
- [Home Assistant Community thread](https://community.home-assistant.io/t/terma-blue-line-bluetooth-radiators-and-heating-elements/81325) — the protocol reverse-engineering groundwork
- [J1mbo/homebridge-TERMA-MOA-Blue](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue/) — a great HomeKit solution and protocol reference
- [ESPHome](https://esphome.io/) — the firmware framework
- [Anthropic Claude](https://claude.ai/) — AI assistance with the ESP-IDF component and reverse-engineering

## License

[MIT](LICENSE). Includes the copyright notices from the upstream projects.

## Disclaimer

Unofficial integration, not affiliated with or endorsed by Terma. Use at your own
risk, and follow the usual safety guidelines around electrical heating equipment.
