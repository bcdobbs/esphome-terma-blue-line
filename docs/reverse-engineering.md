# Reverse-engineering & extending the integration

This is how the protocol table in the [README](../README.md#protocol-reference)
was worked out. Use it if you want to add support for another Terma Blue Line
model, or a feature this integration doesn't cover yet (e.g. the weekly schedule /
*Harmonogram*).

Three complementary techniques, roughly in the order you'll reach for them:

1. **Browse the GATT layout** — what services and characteristics exist.
2. **Read/poke characteristics from the ESP** — what values a characteristic holds
   and how it reacts to writes.
3. **Snoop the official app** — the only reliable way to learn multi-step *write
   sequences* (like the timer-start flow).

> ⚠️ **Keep captures out of git.** BLE snoops and ESP logs contain your radiator's
> MAC address and local network details. The `logs/` directory and `secrets.yaml`
> are already gitignored — keep it that way.

---

## 1. Browse the GATT layout

**Quickest: nRF Connect** (iOS/Android). Put the radiator in pairing mode, connect,
and it enumerates every service, characteristic, UUID and handle, and lets you read
and write them by hand. Great for a first map and for confirming a hypothesis
before writing any YAML.

**From ESPHome:** raise the log level and watch the connection logs — ESPHome lists
the services and characteristics it discovers when `ble_client` connects:

```yaml
logger:
  level: VERY_VERBOSE
```

The service this integration uses is `d97352b0-d19e-11e2-9e96-0800200c9a66`;
characteristics are `d97352b1…b6`.

---

## 2. Read and poke characteristics from the ESP

### Read a characteristic (log its raw bytes)

The `ble_client` sensor with a `format_hex_pretty` lambda is the workhorse — it's
the same pattern the main config already uses. Point it at a candidate UUID and
watch the logs:

```yaml
sensor:
  - platform: ble_client
    type: characteristic
    ble_client_id: terma_moa_ble_client
    id: probe_bX
    name: "probe bX"
    internal: true
    service_uuid: "d97352b0-d19e-11e2-9e96-0800200c9a66"
    characteristic_uuid: "d97352bX-d19e-11e2-9e96-0800200c9a66"  # <- the one you're probing
    update_interval: 30s
    notify: false
    lambda: |-
      ESP_LOGD("probe", "bX (%d bytes): %s", (int) x.size(), format_hex_pretty(x).c_str());
      return NAN;
```

> ⚠️ **Probe gently.** Reading many characteristics at a short `update_interval`
> puts heavy load on the BLE stack and caused the ESP to reset during this project.
> Probe a couple at a time with a 30 s interval, not the whole table at 1 s.

### Poke a characteristic (write and watch)

Add a template button that writes a candidate value, then watch the physical
radiator and the other characteristics for a reaction:

```yaml
button:
  - platform: template
    name: "probe write"
    on_press:
      - ble_client.ble_write:
          id: terma_moa_ble_client
          service_uuid: "d97352b0-d19e-11e2-9e96-0800200c9a66"
          characteristic_uuid: "d97352b3-d19e-11e2-9e96-0800200c9a66"
          value: [0x11, 0, 0, 0]   # try a value, observe what changes
```

### Low-level GATT events

For deeper insight than the YAML sensors give, the `terma_moa` component receives
every GATT callback in `gattc_event_handler` (see
[`components/terma_moa/terma_moa.cpp`](../components/terma_moa/terma_moa.cpp)). You
can temporarily log `ESP_GATTC_READ_CHAR_EVT` / `WRITE_CHAR_EVT` /
`NOTIFY_EVT` there to see raw reads, writes and notifications with their handles.
(An earlier version of this component had a full GATT-dump + per-characteristic
probe built in; it was removed once the protocol was understood — the git history
of the original fork has it if you want a reference.)

---

## 3. Snoop the official Terma app

The GATT map tells you *what* the characteristics are; snooping the app tells you
*how it drives them* — the exact order and values of a multi-step operation. The
timer-start sequence in the README (manual mode → duration → target → run mode) was
found this way.

### iOS — PacketLogger (recommended, most reliable)

1. Install **Additional Tools for Xcode** (Apple Developer downloads) — it includes
   **PacketLogger**.
2. Install the **Bluetooth logging profile** on the iPhone
   (developer.apple.com → *Bluetooth* profile), then reboot.
3. Open PacketLogger on the Mac, start capturing from the connected iPhone.
4. Reproduce **one** action in the Terma app (e.g. start a 1-hour timer).
5. Stop and save the capture as `.pklg`.

### Android — HCI snoop log

1. Developer options → **Enable Bluetooth HCI snoop log**.
2. Reproduce the action in the app.
3. Pull `btsnoop_hci.log` (often via a bug report: `adb bugreport`).

> ⚠️ Android snooping is hit-or-miss. Some phones (e.g. certain OnePlus builds)
> strip the btsnoop out of the bugreport, or run a "filtered" HCI mode that redacts
> payloads, and pulling the raw log from `/data/misc/bluetooth/logs/` needs root.
> If you have both, iOS PacketLogger is the easier path.

### Parse the capture with tshark

Extract the ATT **writes** — that's where the command sequences live:

```bash
# Write Request (0x12) and Write Command (0x52)
tshark -r capture.pklg \
  -Y "btatt.opcode.method == 0x12 || btatt.opcode.method == 0x52" \
  -T fields -e frame.time_relative -e btatt.handle -e btatt.value
```

To also see values the app *read* back, add Read Response (`0x0b`) and Handle Value
Notification (`0x1b`) to the filter. Line up the handles with the UUIDs from step 1,
diff two captures of slightly different actions, and the meaning of each byte falls
out.

---

## Decoding tips

- **Temperatures** are little-endian `uint16`, value ÷ 10 = °C. When a
  characteristic holds both a current and a target value, they're usually two
  `uint16`s back to back (bytes 0–1 and 2–3).
- **The mode byte is a bitfield**, not an enum — decode bits rather than
  memorising every combination. See the README's mode table for the observed
  values.
- **Change one thing at a time** in the app, capture, and diff against a baseline.
  A single-variable change makes the responsible byte obvious.
- **Cross-reference prior work** — J1mbo's
  [homebridge-TERMA-MOA-Blue](https://github.com/J1mbo/homebridge-TERMA-MOA-Blue/)
  and the
  [Home Assistant community thread](https://community.home-assistant.io/t/terma-blue-line-bluetooth-radiators-and-heating-elements/81325)
  already document much of the protocol and are the best starting references for a
  new model.

If you work out something new, a PR updating the protocol table in the README (and
this doc) is very welcome.
