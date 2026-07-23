# esphome-terma-blue-line

An [ESPHome](https://esphome.io/) integration that controls a **Terma Blue Line**
Bluetooth heating element (tested on the **Terma MOA Blue**) from an ESP32, and
exposes it to Home Assistant over the native API.

It talks to the radiator directly over BLE — no cloud, no phone app running in the
background — and surfaces the radiator's *own* regulation: element and room
heating, the built-in auto-off timer, and the exact operating mode. The ESP does
**not** run its own thermostat loop; it drives the radiator the same way the
Terma app does and reflects whatever the radiator reports back.

## Why this exists

The Terma MOA Blue keeps its characteristics behind an **encrypted, bonded** BLE
link and stops advertising once connected. ESPHome's stock `ble_client` connects
but never initiates encryption, so reads fail with *insufficient authentication*
and a naive setup can only talk to the radiator while it's held in pairing mode.

This project ships a tiny external component (`terma_moa`) that requests link
encryption the instant the GATT connection opens, so a persistent, unattended
connection just works after the first pairing. It's built for the **ESP-IDF**
framework.

## Features

- **Element or room heating** — hold the heating element at a target (30–60 °C)
  or let the radiator hold the room air at a target (15–30 °C), using its own
  built-in sensor and regulation.
- **Auto-off timer** — start the radiator's native bounded timer for a chosen
  duration, watch the countdown, and stop it without switching the radiator off.
- **Two-way state** — Heat Mode, targets, power and status all reflect changes
  made on the physical thermostat or the Terma app, decoded straight from the
  radiator's mode byte.
- **Home-Assistant-friendly primitives** — `Heating Active` and `Timer Active`
  binary sensors let you build policies in HA automations (e.g. *notify me if the
  radiator is heating with no auto-off timer*).
- **Persistent encrypted BLE** — pair once, then reconnect automatically on every
  boot.

## Entities exposed

| Entity | Type | Notes |
|---|---|---|
| Radiator Element Temperature | sensor (°C) | Current element temperature |
| Radiator Room Sensor | sensor (°C) | Radiator's built-in room sensor (not lab-accurate) |
| Timer Remaining | sensor (min) | Countdown while a timer runs |
| Heating Active | binary sensor | On whenever the radiator is heating |
| Timer Active | binary sensor | On while an auto-off timer is counting down |
| Radiator BLE Connected | binary sensor (diagnostic) | Link state |
| Radiator Status | text sensor | Decoded mode: `Element - Manual`, `Room - Timer`, `Off`, … |
| Heat Mode | select | `Element` / `Room` |
| Element Target Temperature | number | 30–60 °C, 7.5 °C steps (the radiator's 5 physical levels) |
| Room Target Temperature | number | 15–30 °C, 0.5 °C steps |
| Timer Duration | number | 15–180 min — how long **Start Timer** runs |
| Radiator Power | switch | On = manual (continuous) heating in the selected mode |
| Start Timer / Stop Timer | button | Start or cancel the auto-off timer |
| Clear BLE Bonds / Restart | button (diagnostic) | Re-pairing / reboot |

## Hardware

- Any **ESP32** with Bluetooth LE. Developed on a **Seeed XIAO ESP32-C6**
  (`board: seeed_xiao_esp32c6`); a classic `esp32dev` board works too — just set
  `board:` to match yours.
- A **Terma Blue Line** BLE radiator / heating element (MOA Blue confirmed).

No extra wiring or sensors are needed — the radiator provides its own
temperatures.

## Installation

1. **Clone this repo** next to your ESPHome config, or copy the `components/`
   directory and `terma-moa-blue.example.yaml` into your ESPHome directory. The
   config references the component via `external_components: { type: local }`.

2. **Create your secrets.** Copy `secrets.yaml.example` to `secrets.yaml` and fill
   in your Wi-Fi, an API encryption key, an OTA password, an AP fallback password,
   and your radiator's MAC address:

   ```yaml
   radiator_mac: "XX:XX:XX:XX:XX:XX"
   ```

   Find the MAC in the Terma app, with a scanner like **nRF Connect**, or from
   this device's `esp32_ble_tracker` logs on first boot.

3. **Adjust the config.** In `terma-moa-blue.example.yaml`, set `board:` to your
   ESP32 and rename it if you like. (Copy it to your own `.yaml` — the `.example`
   name is just to keep it out of the way.)

4. **Flash** with ESPHome (`esphome run terma-moa-blue.example.yaml`).

### First pairing

The radiator only bonds when it's in **pairing mode**:

1. Put the radiator into Bluetooth pairing mode (see the Terma manual — usually a
   long-press until the BLE indicator flashes).
2. Boot the ESP. It connects and negotiates encryption using PIN **`123456`**
   (the Terma Blue Line default; change the `passkey:` in the config if yours
   differs).
3. Once bonded, subsequent reconnects are automatic — no pairing mode needed.

To re-pair (e.g. after replacing the ESP), press **Clear BLE Bonds**, restart the
ESP, and put the radiator back in pairing mode.

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

**Temperature encoding** — little-endian `uint16`, value ÷ 10 = °C. To write a
target, put it in bytes 2–3 with bytes 0–1 zero:

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
is the whole reason the component exists.

## Extending it (new models / features)

Want to add support for another Terma Blue Line model, or a feature this doesn't
cover yet (e.g. the weekly schedule)? See
**[docs/reverse-engineering.md](docs/reverse-engineering.md)** — it walks through
how the protocol above was worked out: browsing the GATT layout (nRF Connect /
verbose ESPHome logs), reading and poking characteristics from the ESP, and
snooping the official app (PacketLogger / HCI snoop + tshark) to capture write
sequences. PRs updating the protocol table are welcome.

## Acknowledgments

This project began as a fork of Andrew's
[terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome) and
grew into a standalone ESP-IDF rewrite with room/element control, a full timer
implementation, and exact mode decoding. Huge thanks to:

- [Andrew-a-g/terma-moa-blue-esphome](https://github.com/Andrew-a-g/terma-moa-blue-esphome) — the original ESPHome project this builds on
- [Home Assistant Community thread](https://community.home-assistant.io/t/terma-blue-line-bluetooth-radiators-and-heating-elements/81325) — the protocol reverse-engineering groundwork
- [J1mbo/homebridge-TERMA-MOA-Blue](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue/) — a great HomeKit solution and protocol reference
- [ESPHome](https://esphome.io/) — the firmware framework
- [Anthropic Claude](https://claude.ai/) — AI assistance with the ESP-IDF component and reverse-engineering

## License

[MIT](LICENSE). Includes the original copyright from the upstream project.

## Disclaimer

Unofficial integration, not affiliated with or endorsed by Terma. Use at your own
risk, and follow the usual safety guidelines around electrical heating equipment.
