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
#include <vector>

namespace esphome {
namespace terma_moa {

// Byte order of the radiator's 16-bit fields. Upstream-verified units are little-
// endian; others report the same fields big-endian (e.g. element raw 01 75 02 58 =
// 37.3 / 60.0 C). Set with `byte_order:` on the terma_moa component.
enum class TermaByteOrder : uint8_t { LITTLE = 0, BIG = 1 };

// Registers as a node on a `ble_client` and, the moment the GATT connection
// opens, proactively requests link encryption. The Terma MOA Blue refuses to
// serve its characteristics over an unencrypted link (reads fail with
// insufficient-authentication), and ESPHome never initiates encryption on its
// own, so we do it here.
//
// It also owns the radiator's byte order, so every read and write in the YAML goes
// through decode_u16() / encode_u16_pair() instead of hand-rolled byte maths.
class TermaMoa : public Component, public ble_client::BLEClientNode {
 public:
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;

  void set_byte_order(TermaByteOrder order) { this->byte_order_ = order; }
  bool is_big_endian() const { return this->byte_order_ == TermaByteOrder::BIG; }

  // Decode the uint16 at x[offset], x[offset + 1] in the configured byte order.
  // Caller must ensure x.size() >= offset + 2.
  uint16_t decode_u16(const std::vector<uint8_t> &x, size_t offset) const;

  // Encode two uint16s into a 4-byte payload {first, second} in the configured order.
  // Targets are written as encode_u16_pair(0, temp_x10); the timer as (secs, secs).
  std::vector<uint8_t> encode_u16_pair(uint16_t first, uint16_t second) const;

  // Sanity-check an element (b2) payload: if its target (bytes 2-3) is only a valid
  // element temperature (30-60 C) when read in the *other* byte order, log a warning
  // once suggesting the byte_order setting. Never changes behaviour.
  void check_element_byte_order(const std::vector<uint8_t> &x);

 protected:
  TermaByteOrder byte_order_{TermaByteOrder::LITTLE};
  bool byte_order_warned_{false};
};

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
