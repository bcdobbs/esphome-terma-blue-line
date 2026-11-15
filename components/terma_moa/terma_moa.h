/*
 * AI-Provenance:
 *   model: claude-opus-4-8[1m]
 *   harness: Claude Code
 *   skills:
 *     - brainstorming
 */
#pragma once

#ifdef USE_ESP32

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace terma_moa {

// Protocol generation spoken by the radiator. Set with `protocol:` on the component.
//   MODERN: little-endian uint16 values, 4-byte mode payload, room 0x11 / element 0x21,
//           timers 0x03 / 0x05 / 0x09.
//   LEGACY: big-endian uint16 values (element raw 01 75 02 58 = 37.3 / 60.0 C), 1-byte
//           mode payload, room 5 / element 6, no timer.
enum class TermaProtocol : uint8_t { MODERN = 0, LEGACY = 1 };

// Registers as a node on a `ble_client` and, the moment the GATT connection
// opens, proactively requests link encryption. The Terma MOA Blue refuses to
// serve its characteristics over an unencrypted link (reads fail with
// insufficient-authentication), and ESPHome never initiates encryption on its
// own, so we do it here.
//
// It also owns the radiator's protocol generation, so every value and mode byte the
// YAML reads or writes goes through the helpers below instead of hand-rolled bytes.
class TermaMoa : public Component, public ble_client::BLEClientNode {
 public:
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_protocol(TermaProtocol protocol) { this->protocol_ = protocol; }
  bool is_legacy() const { return this->protocol_ == TermaProtocol::LEGACY; }

  // ---- 16-bit values (temperatures x10, timer seconds) ----
  // Decode the uint16 at x[offset], x[offset + 1]. Caller ensures x.size() >= offset + 2.
  uint16_t decode_u16(const std::vector<uint8_t> &x, size_t offset) const;
  // Encode {first, second} as a 4-byte payload. Targets: (0, temp_x10); timer: (s, s).
  std::vector<uint8_t> encode_u16_pair(uint16_t first, uint16_t second) const;

  // ---- operating mode (b3) ----
  // Mode byte for continuous heating in room or element regulation.
  int mode_manual(bool room) const;
  // Payload for writing a mode byte: 1 byte (legacy) or 4 bytes (modern).
  std::vector<uint8_t> encode_mode(int mode) const;
  bool is_heating(int mode) const;
  bool is_room_mode(int mode) const;
  bool is_element_mode(int mode) const;
  bool is_timer_mode(int mode) const;
  bool supports_timer() const { return !this->is_legacy(); }
  // Human-readable status for the mode byte.
  std::string mode_name(int mode) const;

  // ---- diagnostics: warn once if the radiator looks like the other protocol ----
  void check_element_payload(const std::vector<uint8_t> &x);
  void check_mode_payload(const std::vector<uint8_t> &x);

 protected:
  void warn_protocol_(const char *evidence);
  TermaProtocol protocol_{TermaProtocol::MODERN};
  bool protocol_warned_{false};
};

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
