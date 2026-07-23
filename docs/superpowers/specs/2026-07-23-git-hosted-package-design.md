<!--
AI-Provenance:
  model: claude-opus-4-8[1m]
  harness: Claude Code
  skills:
    - brainstorming
-->

# Design: git-hosted ESPHome package for Terma Blue Line

**Date:** 2026-07-23
**Status:** Approved (design), pending spec review
**Scope:** Distribution refactor only — no behavior changes.

## Problem

A device today needs the full ~615-line `terma-moa-blue.example.yaml` copied into
its ESPHome config. In ESPHome Builder (the dashboard in Home Assistant) that means
pasting a huge file per device, and the C++ component under `components/` must also
be copied locally because the config references it via `external_components:
{ type: local }`. We want a device's config to be a small file that imports
everything else over git.

## Goal

Turn the shared configuration into a git-hosted **ESPHome package** so a per-device
file shrinks to ~20 lines and pulls the entities and the C++ component straight from
this repository. No entity logic, lambda, protocol, or C++ behavior changes — this
is a cut-and-host refactor plus documentation updates.

Non-goals (explicitly deferred):

- Moving lambda/script/globals logic into the C++ component.
- Building a native `climate` platform.

## Approach

A single **Terma-specific package** holds the "Terma brain"; each device file owns
its identity, network, and secrets and references the package over git.

### Repo layout after the change

| File | Role | Change |
|---|---|---|
| `terma.package.yaml` | **NEW** — the package. `esp32_ble`, `esp32_ble_tracker`, `ble_client`, `terma_moa:`, all globals + sensor/binary_sensor/select/number/switch/button/text_sensor + scripts. | Content moved verbatim from today's example. |
| `terma-moa-blue.example.yaml` | Thin **consumer** device file — what a user pastes into ESPHome Builder. | Shrinks from ~615 lines to ~20. |
| `components/terma_moa/` | The C++ component (encryption-on-connect helper). | Unchanged; pulled over git by consumers. |
| `README.md` | Docs. | Installation section rewritten for the package flow (keep local-dev instructions). |
| `secrets.yaml.example` | Docs. | Unchanged (still lists the same `!secret` values). |

### The boundary

**Package owns** (parts that never change per-device):

- `esp32_ble: { io_capability: keyboard_only }`
- `esp32_ble_tracker:` (scan parameters as today)
- `ble_client:` — `id: terma_moa_ble_client`, `mac_address: ${radiator_mac}`,
  `auto_connect: true`, plus the `on_connect` / `on_passkey_request` /
  `on_passkey_notification` / `on_disconnect` handlers. Passkey reply uses
  `${passkey}`.
- `terma_moa:` — `ble_client_id: terma_moa_ble_client`
- All `globals`, and every entity (`sensor`, `binary_sensor`, `select`, `number`,
  `switch`, `button`, `text_sensor`) and `script`, verbatim from today's file.

The package references **only substitutions** — never `!secret` directly, never a
board.

**Consumer owns** (per-device identity):

- `substitutions:` — `device_name`, `friendly_name`, `board`,
  `radiator_mac: !secret radiator_mac`
- `esphome:` — `name`, `friendly_name`, `min_version`
- `esp32:` — `board:` **and** `framework: { type: esp-idf }` (required by the
  component; documented loudly, kept in one place)
- `wifi` / `api` / `ota` / `logger` (`captive_portal` optional)
- `external_components:` (git for users, local for the maintainer — see below)
- `packages:` — the reference to `terma.package.yaml`

### Why `external_components` lives in the consumer, not the package

A component can only be declared once. Putting `external_components` in the consumer
lets the same `terma.package.yaml` serve two consumers with zero entity duplication:

- **Users** add a 3-line git block:

  ```yaml
  external_components:
    - source: github://kumalg/esphome-terma-blue-line@main
      components: [terma_moa]
  ```

  and reference the package via git:

  ```yaml
  packages:
    terma: github://kumalg/esphome-terma-blue-line/terma.package.yaml@main
  ```

- **The maintainer, developing locally**, uses:

  ```yaml
  external_components:
    - source: { type: local, path: components }
  packages:
    terma: !include terma.package.yaml
  ```

  so `esphome run` picks up C++ and entity edits immediately, no push-to-git first.

If `external_components` lived in the package, it would have to hardcode a git
source, breaking the local iterate-and-flash loop.

### Substitution contract

| Name | Required? | Default | Notes |
|---|---|---|---|
| `radiator_mac` | Yes | — | No default; missing value fails fast ("substitution not defined"). Consumer supplies it as `!secret radiator_mac`. |
| `passkey` | No | `"123456"` | Terma Blue Line default; override in the consumer if different. |

### Example consumer file (target shape, ~20 lines)

```yaml
substitutions:
  device_name: terma-moa-blue
  friendly_name: "Terma MOA Blue"
  board: esp32dev
  radiator_mac: !secret radiator_mac

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}
  min_version: 2026.4.0

esp32:
  board: ${board}
  framework: { type: esp-idf }   # required by the terma_moa component

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
api:
  encryption: { key: !secret api_encryption_key }
ota:
  - platform: esphome
    password: !secret ota_password
logger:

external_components:
  - source: github://kumalg/esphome-terma-blue-line@main
    components: [terma_moa]

packages:
  terma: github://kumalg/esphome-terma-blue-line/terma.package.yaml@main
```

## What does not change

No entity logic, no lambdas, no C++, no protocol handling, no globals semantics.
Every byte of behavior is identical to today's `terma-moa-blue.example.yaml`.

## Verification / open implementation details

- **External-components path resolution.** Confirm ESPHome's git
  `external_components` resolves `components/terma_moa` from the repo root by
  default; add an explicit `path:` if it does not. (Our repo has
  `components/terma_moa`, which matches the default `components/` convention.)
- **Package-file git reference.** Confirm `packages:` accepts a single-file
  `github://user/repo/path/to/file.yaml@ref` reference (documented form).
- **`esp32:` merge.** Chose to keep the whole `esp32:` block in the consumer (board
  + `framework: esp-idf`) rather than have the package contribute `framework` via a
  partial merge — one owner, less surprise, no risk of a consumer's `arduino`
  silently overriding a package-set `esp-idf`.
- **Version pinning.** `@main` for latest; recommend documenting a tag (e.g.
  `@v1.0.0`) once the repo is tagged, for reproducible builds.

## Testing / acceptance

- The rewritten `terma-moa-blue.example.yaml` compiles via ESPHome and produces the
  same set of entities as the pre-refactor config (same names, same platforms).
- `esphome config terma-moa-blue.example.yaml` shows the package and external
  component resolved (git for users; validated locally via the `!include` +
  `type: local` dev variant).
- A local build (`esphome compile`) succeeds with the local-dev consumer variant.
