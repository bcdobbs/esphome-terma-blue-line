<!--
AI-Provenance:
  model: claude-opus-4-8[1m]
  harness: Claude Code
  skills:
    - writing-plans
-->

# Git-Hosted ESPHome Package Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split the ~615-line `terma-moa-blue.example.yaml` into a git-hosted `terma.package.yaml` (the shared "Terma brain") and a ~20-line per-device consumer file, so a device in ESPHome Builder imports everything — entities and the C++ component — over git instead of copying files.

**Architecture:** One Terma-specific package holds every entity, script, global, the `ble_client`, the BLE stack config, and the `terma_moa:` component reference, parameterized only through substitutions. The consumer file owns identity, network, secrets, `esp32:` (board + `esp-idf`), and the `external_components:` declaration. Keeping `external_components:` in the consumer (not the package) lets the same package be pulled over git by users and included locally (`!include` + `type: local`) by the maintainer, with zero entity duplication.

**Tech Stack:** ESPHome (ESP-IDF framework), YAML packages + `external_components`, git as the distribution channel. Verification here is dependency-free `grep`/`git` structural + regression checks; the functional `esphome config`/`compile` acceptance runs in the user's ESPHome environment (Builder or a venv), because neither the `esphome` CLI nor a real `secrets.yaml` exists in this working copy.

## Global Constraints

- **Repo slug (verbatim in every git ref):** `kumalg/esphome-terma-blue-line`
- **Framework:** `esp-idf` is required by the `terma_moa` component; the consumer's `esp32:` block must set `framework: { type: esp-idf }`.
- **`min_version`:** `2026.4.0` (carried over unchanged).
- **Passkey default:** `"123456"` (Terma Blue Line default), a package substitution the consumer can override.
- **No secrets in the package:** `terma.package.yaml` must contain zero `!secret` tags and no board — it references substitutions only.
- **Behavior is byte-identical:** no entity, lambda, script, global, or protocol change. The only edits during extraction are (a) the passkey literal → `${passkey}` and (b) adding a package-level `substitutions:` default for `passkey`. Everything else moves verbatim.
- **Behavior-preservation baseline — the 17 entity `name:` values that MUST all be present in the package** (the source-of-truth regression set):
  `Radiator Room Sensor`, `Radiator Element Temperature`, `Operating Mode Raw`, `Timer Remaining`, `Radiator BLE Connected`, `Heating Active`, `Timer Active`, `Heat Mode`, `Element Target Temperature`, `Room Target Temperature`, `Timer Duration`, `Radiator Power`, `Start Timer`, `Stop Timer`, `Clear BLE Bonds`, `Restart`, `Radiator Status`.

---

## File Structure

| File | Responsibility | Action |
|---|---|---|
| `terma.package.yaml` | The package: BLE stack (`esp32_ble`, `esp32_ble_tracker`), `ble_client`, `terma_moa:`, `globals`, and all entities + scripts. Substitution-only. | Create |
| `terma-moa-blue.example.yaml` | Thin consumer device file for ESPHome Builder: identity, `esp32:`, network, `external_components:` (git), `packages:` (git). | Rewrite (~615 → ~20 lines) |
| `.gitignore` | Ignore the maintainer's personal `dev.yaml` local-dev rig. | Modify |
| `README.md` | Installation rewritten for the package flow; new "Local development" subsection. | Modify |
| `components/terma_moa/`, `secrets.yaml.example` | Unchanged. | — |

The current committed `terma-moa-blue.example.yaml` (at `HEAD`) is the extraction source. Read exact ranges from it with `git show HEAD:terma-moa-blue.example.yaml`.

---

## Task 1: Create the package (`terma.package.yaml`)

Extract the Terma brain from the committed example into a new package file, parameterizing the passkey. This is the core deliverable and is independently reviewable: a reviewer can confirm the split is correct and secret-free without seeing the consumer changes.

**Files:**
- Create: `terma.package.yaml`
- Source (read-only): `terma-moa-blue.example.yaml` at `HEAD`

**Interfaces:**
- Produces (substitution contract the consumer in Task 2 relies on):
  - `radiator_mac` — **required**, no default. Referenced as `${radiator_mac}` in `ble_client.mac_address`.
  - `passkey` — optional, package default `"123456"`. Referenced as `${passkey}` in `ble_client.on_passkey_request`.
- Produces the `ble_client` id `terma_moa_ble_client`, consumed by `terma_moa:` and every entity within the same file.

- [ ] **Step 1: Write the failing structural checks**

Create a scratch check script and run it — it must fail because the file does not exist yet. Run:

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
test -f terma.package.yaml && echo "EXISTS" || echo "MISSING (expected at this step)"
```

Expected: `MISSING (expected at this step)`

- [ ] **Step 2: Create `terma.package.yaml`**

Assemble the file in this exact order. The entity/script/global/BLE body is moved **verbatim** from the committed example; only the two marked edits differ.

1. **Package substitution defaults** (new — top of file):

```yaml
# terma.package.yaml — the shared "Terma Blue Line" configuration.
# Consumed via `packages:` (git for users, `!include` for local dev). Holds every
# entity, script, global, the ble_client, the BLE stack, and the terma_moa
# component. References substitutions only — no !secret, no board here.
#
# Substitution contract (provided by the consuming device file):
#   radiator_mac  (required)  the radiator's BLE MAC, e.g. !secret radiator_mac
#   passkey       (optional)  BLE pairing PIN; defaults to the Terma default below
substitutions:
  passkey: "123456"
```

2. **`esp32_ble` + `esp32_ble_tracker`** — copy verbatim from the example (`esp32_ble:` through the end of the `esp32_ble_tracker:` block, lines 49–58 of `HEAD`).

3. **`ble_client`** — copy the `ble_client:` block (lines 60–92 of `HEAD`) verbatim **except** the passkey literal. In `on_passkey_request`, change the two lines:

```yaml
        - logger.log: "Passkey requested by radiator - sending 123456"
        - ble_client.passkey_reply:
            id: terma_moa_ble_client
            passkey: 123456
```

to:

```yaml
        - logger.log: "Passkey requested by radiator - sending ${passkey}"
        - ble_client.passkey_reply:
            id: terma_moa_ble_client
            passkey: ${passkey}
```

`mac_address: ${radiator_mac}` stays exactly as-is (it is already a substitution).

4. **`terma_moa:`** — copy the `terma_moa:` block (lines 94–97 of `HEAD`) verbatim.

5. **`globals` through `text_sensor`** — copy the entire remainder of the example from `globals:` (line 99) to end of file (line 615) **verbatim**. This is every global, `sensor`, `binary_sensor`, `select`, `number`, `script`, `switch`, `button`, and `text_sensor`.

Do **not** include: `substitutions` (device identity), `esphome`, `esp32`, `external_components`, `logger`, `api`, `ota`, `wifi`, `captive_portal` — those belong to the consumer (Task 2).

- [ ] **Step 3: Run the structural + regression checks**

Run each; all must match expected output.

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line

# (a) No secrets and no board leaked into the package
grep -nE '!secret|^\s*board:' terma.package.yaml && echo "FAIL: secret/board present" || echo "OK: no secret/board"

# (b) Substitution contract present
grep -q '${radiator_mac}' terma.package.yaml && grep -qE '^\s*passkey:\s*"123456"' terma.package.yaml && echo "OK: substitutions" || echo "FAIL: substitutions"
grep -q 'passkey: ${passkey}' terma.package.yaml && echo "OK: passkey parameterized" || echo "FAIL: passkey literal"

# (c) All 17 baseline entity names present
grep -cE '^\s*name:' terma.package.yaml   # expect: 17

# (d) Consumer-only keys absent from the package
grep -nE '^(esphome|external_components|api|ota|wifi|captive_portal|logger):' terma.package.yaml && echo "FAIL: consumer key in package" || echo "OK: no consumer keys"

# (e) Behavior preservation: entity/script/global body identical to HEAD except the 3 passkey/substitution lines.
# Compare the moved region (globals..EOF) against HEAD; diff must be empty.
diff <(git show HEAD:terma-moa-blue.example.yaml | sed -n '/^globals:/,$p') \
     <(sed -n '/^globals:/,$p' terma.package.yaml) && echo "OK: globals..EOF verbatim" || echo "FAIL: body drift"
```

Expected: `OK: no secret/board`, `OK: substitutions`, `OK: passkey parameterized`, `17`, `OK: no consumer keys`, `OK: globals..EOF verbatim`.

> If check (e) reports drift, the `globals:`-to-end region was altered during the move — revert it to match `HEAD` exactly (only the earlier `ble_client` passkey lines and the new top `substitutions` may differ from `HEAD`).

- [ ] **Step 4: Commit**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
git add terma.package.yaml
git commit -m "$(cat <<'EOF'
Add terma.package.yaml — shared Terma Blue Line package

Move the BLE stack, ble_client, terma_moa, globals, and all entities out of
the example into a git-consumable package. Parameterize the pairing PIN as
${passkey} (default 123456). No behavior change.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Rewrite the consumer (`terma-moa-blue.example.yaml`)

Replace the fat example with a thin device file that pulls the package and the C++ component over git. Independently reviewable: a reviewer checks the git refs, the substitution wiring, and that the `esp-idf` requirement is stated — without re-reading the package.

**Files:**
- Modify (full rewrite): `terma-moa-blue.example.yaml`

**Interfaces:**
- Consumes from Task 1: substitution `radiator_mac` (supplied as `!secret radiator_mac`); optional `passkey` (left at package default); the package's `terma_moa_ble_client` and entities.
- Produces: the C++ component `terma_moa` via `external_components` (git source), and the substitution values `device_name`, `friendly_name`, `board`, `radiator_mac`.

- [ ] **Step 1: Verify the current file is the fat version (pre-condition)**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
grep -cE '^\s*name:' terma-moa-blue.example.yaml   # expect: 18 (fat version, still has all entities)
```

Expected: `18`

- [ ] **Step 2: Overwrite `terma-moa-blue.example.yaml` with the consumer**

Write exactly this content:

```yaml
# Per-device config for a Terma Blue Line radiator, for ESPHome Builder / `esphome run`.
# All entities and the C++ component are pulled from this repo over git via the
# package + external_components references below — no need to copy any large file.
#
# Setup:
#   1. Copy secrets.yaml.example -> secrets.yaml and fill in your values
#      (Wi-Fi, api key, ota password, ap password, and radiator_mac).
#   2. Set `board:` to your ESP32.
#   3. Flash: `esphome run terma-moa-blue.example.yaml`
# Pin @main to a released tag (e.g. @v1.0.0) for reproducible builds once tagged.

substitutions:
  device_name: terma-moa-blue
  friendly_name: "Terma MOA Blue"
  # Any ESP32 with BLE. Developed on seeed_xiao_esp32c6; esp32dev also works.
  board: esp32dev
  # Radiator BLE MAC — keep it in secrets.yaml (see secrets.yaml.example).
  radiator_mac: !secret radiator_mac

esphome:
  name: ${device_name}
  friendly_name: ${friendly_name}
  min_version: 2026.4.0

esp32:
  board: ${board}
  framework:
    type: esp-idf   # required by the terma_moa component

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
  ap:
    ssid: "${friendly_name} Fallback"
    password: !secret ap_password

captive_portal:

# The terma_moa C++ component, pulled straight from this repo.
external_components:
  - source: github://kumalg/esphome-terma-blue-line@main
    components: [terma_moa]

# All entities, the ble_client, the BLE stack, and the terma_moa: hub.
packages:
  terma: github://kumalg/esphome-terma-blue-line/terma.package.yaml@main
```

- [ ] **Step 3: Run the consumer structural checks**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line

# (a) It no longer defines entities (only the esphome name remains)
grep -cE '^\s*name:' terma-moa-blue.example.yaml   # expect: 1

# (b) Exact git refs present (slug + package path + external component)
grep -q 'packages:' terma-moa-blue.example.yaml && \
grep -q 'terma: github://kumalg/esphome-terma-blue-line/terma.package.yaml@main' terma-moa-blue.example.yaml && \
grep -q 'source: github://kumalg/esphome-terma-blue-line@main' terma-moa-blue.example.yaml && \
grep -q 'components: \[terma_moa\]' terma-moa-blue.example.yaml && echo "OK: git refs" || echo "FAIL: git refs"

# (c) esp-idf requirement stated; radiator_mac wired to secret
grep -q 'type: esp-idf' terma-moa-blue.example.yaml && \
grep -q 'radiator_mac: !secret radiator_mac' terma-moa-blue.example.yaml && echo "OK: framework+mac" || echo "FAIL: framework+mac"

# (d) File is thin
wc -l terma-moa-blue.example.yaml   # expect: well under 70 lines
```

Expected: `1`, `OK: git refs`, `OK: framework+mac`, and a line count under ~70.

- [ ] **Step 4: Commit**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
git add terma-moa-blue.example.yaml
git commit -m "$(cat <<'EOF'
Slim example config to a git-package consumer

The per-device file now pulls all entities from terma.package.yaml and the
C++ component from components/ over git. Shrinks ~615 lines to ~20.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Docs — `.gitignore` + `README.md`

Document the new install flow and the maintainer's local-dev rig. Independently reviewable as a docs-only change.

**Files:**
- Modify: `.gitignore`
- Modify: `README.md`

- [ ] **Step 1: Ignore the personal local-dev file**

Append to `.gitignore` (after the `secrets.yaml` line, within the secrets section or its own):

```gitignore

# Maintainer's personal local-dev entry file (uses local components + !include)
dev.yaml
```

Verify:

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
grep -q '^dev.yaml' .gitignore && echo "OK" || echo "FAIL"
```

Expected: `OK`

- [ ] **Step 2: Rewrite the README "Installation" section**

Replace the current numbered "Installation" list (the four steps under `## Installation`) with the package-based flow. Use exactly this content for the section body:

```markdown
## Installation

You don't copy the big config anymore — your device imports it from this repo
over git. In **ESPHome Builder** (or a local ESPHome), create a device with a
config like [`terma-moa-blue.example.yaml`](terma-moa-blue.example.yaml):

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
  - source: github://kumalg/esphome-terma-blue-line@main
    components: [terma_moa]

packages:
  terma: github://kumalg/esphome-terma-blue-line/terma.package.yaml@main
```

1. **Create your secrets.** Copy `secrets.yaml.example` to `secrets.yaml` and fill
   in Wi-Fi, an API encryption key, an OTA password, an AP fallback password, and
   your radiator's MAC (`radiator_mac`).
2. **Set `board:`** to your ESP32. Leave `framework: esp-idf` — the component
   requires it.
3. **Flash** with ESPHome (`esphome run terma-moa-blue.example.yaml`), or just
   Install from ESPHome Builder.

Pin `@main` to a released tag (e.g. `@v1.0.0`) once the repo is tagged, for
reproducible builds. To override the pairing PIN, add `passkey: "your-pin"` under
`substitutions`.
```

- [ ] **Step 3: Add a "Local development" subsection**

Immediately after the Installation section, add:

```markdown
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
`terma.package.yaml` immediately.
```

- [ ] **Step 4: Verify the README changes**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
grep -q 'packages:' README.md && \
grep -q 'terma.package.yaml@main' README.md && \
grep -q 'Local development' README.md && \
grep -q 'type: local' README.md && echo "OK: README updated" || echo "FAIL"
```

Expected: `OK: README updated`

- [ ] **Step 5: Commit**

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
git add .gitignore README.md
git commit -m "$(cat <<'EOF'
Docs: package-based install flow + local-dev rig

Rewrite Installation for the git package/external_components flow and add a
Local development subsection (dev.yaml with !include + local components).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Functional acceptance in ESPHome (run in your environment)

The git refs only resolve once the package is on `origin/main`, and validation needs the `esphome` CLI + a real `secrets.yaml` — neither exists in this working copy. This task is a checklist you run in your ESPHome environment. Do the local-dev validation **before** publishing so a broken package is never pushed.

**Files:** none (verification only).

- [ ] **Step 1: Validate locally via `dev.yaml` (no push needed)**

Create `dev.yaml` from the README "Local development" snippet, ensure `secrets.yaml` exists, then:

```bash
esphome config dev.yaml
```

Expected: config resolves with no errors; the printed config lists all 17 entities (Radiator Room Sensor … Radiator Status) and the `terma_moa` component. Optionally compile:

```bash
esphome compile dev.yaml
```

Expected: build succeeds.

- [ ] **Step 2: Publish**

Only after Step 1 passes, push so the git refs resolve:

```bash
cd /Users/kamilgolec/Projects/_kumalg/esphome-terma-blue-line
git push origin main
```

- [ ] **Step 3: Validate the published consumer**

In ESPHome Builder (or locally with the CLI), validate `terma-moa-blue.example.yaml`:

```bash
esphome config terma-moa-blue.example.yaml
```

Expected: ESPHome fetches `terma.package.yaml` and the `terma_moa` component from `github://kumalg/esphome-terma-blue-line@main`, resolves the config, and lists the same 17 entities. In Builder: **Install** succeeds and the device comes up with the identical entity set to the pre-refactor firmware.

- [ ] **Step 4: Confirm behavior parity on the device**

On the running device, confirm the entity set and behavior match the pre-refactor firmware: Heat Mode select, Element/Room targets, Radiator Power, Start/Stop Timer, and the sensors all function. No new or missing entities.

---

## Self-Review

**Spec coverage** — every spec section maps to a task:
- Repo layout (new package, slim consumer, unchanged component/secrets) → Tasks 1, 2, 3.
- The boundary (package-owned vs consumer-owned keys) → Task 1 (package) + Task 2 (consumer), enforced by checks 1(d)/1(e) and 2(a).
- `external_components` in the consumer + local-dev rationale → Task 2 (git block) + Task 3 (local-dev README + `dev.yaml`).
- Substitution contract (`radiator_mac` required, `passkey` default) → Task 1 Step 2/Step 3(b).
- "No behavior change" → Task 1 check (e) verbatim diff + baseline entity-count checks + Task 4 device parity.
- Verification items (path resolution, package git-file ref, `esp32` merge choice, version pinning) → Task 4 functional acceptance; `esp32:` kept whole in the consumer (Task 2); pinning documented (Task 2 comment + Task 3 README).

**Placeholder scan:** No TBD/TODO. The verbatim-move steps cite exact `HEAD` line ranges and the exact two edits rather than re-typing 500 lines (DRY; avoids transcription error) — the source is a concrete committed file, so this is not a "similar to" placeholder.

**Type/name consistency:** `terma_moa_ble_client`, `${radiator_mac}`, `${passkey}`, `components: [terma_moa]`, `terma.package.yaml`, and the slug `kumalg/esphome-terma-blue-line` are used identically across Tasks 1–4 and match the Global Constraints.
