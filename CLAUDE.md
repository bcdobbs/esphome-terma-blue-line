# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

An ESPHome integration that switches a Terma Blue Line Bluetooth radiator element on/off from an
ESP32, for a Home Assistant thermostat to drive. Users don't clone this repo: their device YAML
pulls the component and the package straight from GitHub (`ref: main`, `refresh: 1d`; see
`examples/m5stack-atoms3-lite.yaml`). **Anything merged to `main` reaches users' next build**, so
keep `main` working.

## Commands

There is no test suite or linter; changes are checked by validating and compiling a config.

Create a gitignored `dev.yaml` that uses the working copy. Take the example config and swap in:

```yaml
external_components:
  - source: { type: local, path: components }
    components: [terma_moa]
packages:
  terma: !include terma-on-demand.package.yaml
```

Then copy `secrets.yaml.example` to `secrets.yaml` (gitignored). Dummy values are fine for compiling.

```sh
esphome config dev.yaml    # schema validation + package/substitution resolution (fast)
esphome compile dev.yaml   # full ESP-IDF firmware build (first run downloads the toolchain)
esphome run dev.yaml       # build, flash and stream logs (needs the hardware)
```

The generated C++ is at `.esphome/build/<device_name>/src/main.cpp`. Read it to see what the
YAML and `to_code()` turned into.

## Architecture

Two halves, joined by a small C++ API:

**`components/terma_moa/`: the external component (C++ plus Python codegen).** It does what stock
ESPHome can't: the radiator only serves its characteristics over an encrypted, bonded link, and
`ble_client` never starts encryption. So `TermaMoa` registers as a `BLEClientNode` on the
`ble_client`, and calls `esp_ble_set_encryption` on `ESP_GATTC_OPEN_EVT`. It also:
- reports `is_ready()` only once both the auth result (`ESP_GAP_BLE_AUTH_CMPL_EVT`) and service
  discovery have arrived, and the parent is still connected,
- performs read-backs itself (`request_read()`: mode b3, then element b2, one at a time),
- owns everything that differs between protocol generations (`encode_*`, `is_heating`,
  `is_element_mode`, `mode_element`) and logs a one-off warning if payloads look like the other
  generation.

`__init__.py` only defines the YAML schema (`protocol: modern|legacy`, `ble_client_id`) and emits
the setup C++. The component is ESP32 + ESP-IDF only (`cv.only_on_esp32`; Bluedroid APIs).

**`terma-on-demand.package.yaml`: all behaviour and entities.** Each Radiator Power command
runs the `apply_power` script, which works through these steps:
1. connect (`auto_connect: false`),
2. wait for `is_ready()`,
3. read back the current state, and stop there if it's already as asked,
4. write off → element target → element mode,
5. read back again to confirm,
6. disconnect.

A failed command gets up to 3 attempts straight away, then the `interval` retries once a minute.
The package states its substitution contract (`radiator_mac`, `protocol`, `element_temp`) in its
header comment.

**The contract between them.** The package's lambdas call `id(terma_hub).<method>()`, and use the
ids `terma_hub` and `terma_moa_ble_client`. Renaming or changing a public `TermaMoa` method, or
one of those ids, means updating the package in the same change. The public section of
`terma_moa.h` is exactly the set of methods the YAML uses; keep internal helpers `protected`
(ESPHome style: trailing underscore).

## Non-obvious constraints (don't "simplify" these away)

- **Off → target → mode, always.** Legacy units can double the element target if the mode is
  changed directly. If a target above 60 °C is read back, the package switches off and reports a
  fault. See README "Protocol versions".
- **BLE writes live in their own `queued` scripts, run with `script.wait`.** If the link drops
  mid-write, ESPHome abandons the rest of that action chain without telling the enclosing
  `if`/`while`. A write placed inline in `apply_power` would leave it stuck "running".
- **Characteristic handles are looked up on `ESP_GATTC_SEARCH_CMPL_EVT`.** The parent frees its
  service cache once every node is marked `ESTABLISHED`, which this component does at that same
  point.
- **`is_ready()` also checks `parent()->connected()`.** The parent leaves the connected state
  before nodes receive the disconnect event.
- **Some protocol facts live in both halves.** The service/characteristic UUIDs and the 30–60 °C
  element range appear in both the C++ and the package YAML. Change them together, and update
  README "Protocol reference" when protocol handling changes.
- **Entity names/IDs are user-facing.** Radiator Power, Radiator Status, Clear BLE Bonds and
  Restart are promised to keep their entity IDs in Home Assistant.

## Conventions

- Source files carry an `AI-Provenance` header comment; keep it when editing.
- The README is the user documentation (installation, protocol versions, reading logs, protocol
  reference). Behaviour changes usually need a matching README edit.
