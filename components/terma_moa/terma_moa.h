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
#include "esphome/core/helpers.h"
#include "esphome/components/ble_client/ble_client.h"

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace esphome {
namespace terma_moa {

// Protocol generation spoken by the radiator. Set with `protocol:` on the component.
//   MODERN: little-endian uint16 values, 4-byte mode payload, element mode 0x21.
//   LEGACY: big-endian uint16 values (element raw 01 75 02 58 = 37.3 / 60.0 C), 1-byte
//           mode payload, element mode 6.
enum class TermaProtocol : uint8_t { MODERN = 0, LEGACY = 1 };

// Registers as a node on a `ble_client` and, the moment the GATT connection
// opens, proactively requests link encryption. The Terma MOA Blue refuses to
// serve its characteristics over an unencrypted link (reads fail with
// insufficient-authentication), and ESPHome never initiates encryption on its
// own, so we do it here.
//
// It also tracks when the link is actually usable (encrypted AND services
// discovered), and owns the radiator's protocol generation, so every value and
// mode byte the YAML reads or writes goes through the helpers below.
class TermaMoa : public Component, public ble_client::BLEClientNode {
 public:
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

  // True once the link is encrypted and service discovery has finished, false again
  // as soon as it drops. Gate every read/write on this.
  // (Also checks the parent: it drops out of ESTABLISHED before nodes see the disconnect.)
  bool is_ready() { return this->ready_ && this->parent() != nullptr && this->parent()->connected(); }

  void set_protocol(TermaProtocol protocol) { this->protocol_ = protocol; }
  bool is_legacy() const { return this->protocol_ == TermaProtocol::LEGACY; }

  // ---- 16-bit values (temperatures x10) ----
  // Decode the uint16 at x[offset], x[offset + 1]. Caller ensures x.size() >= offset + 2.
  uint16_t decode_u16(const std::vector<uint8_t> &x, size_t offset) const;
  // Payload for an element target write (b2): clamps to 30-60 C and rounds to 0.1 C.
  std::vector<uint8_t> encode_element_target(float celsius) const;

  // ---- operating mode (b3) ----
  // Mode byte for continuous element heating.
  int mode_element() const { return this->is_legacy() ? 0x06 : 0x21; }
  // Payload for writing a mode byte: 1 byte (legacy) or 4 bytes (modern).
  std::vector<uint8_t> encode_mode(int mode) const;
  bool is_heating(int mode) const;
  // Exactly continuous element heating. A modern unit running its auto-off timer
  // (0x05) doesn't count: it would switch itself off.
  bool is_element_mode(int mode) const { return mode == this->mode_element(); }

  // ---- diagnostics: warn once if the radiator looks like the other protocol ----
  void check_element_payload(const std::vector<uint8_t> &x);
  void check_mode_payload(const std::vector<uint8_t> &x);

 protected:
  void warn_protocol_(const char *evidence);
  void set_link_state_(bool encrypted, bool discovered);

  TermaProtocol protocol_{TermaProtocol::MODERN};
  bool protocol_warned_{false};
  bool encrypted_{false};
  bool discovered_{false};
  bool ready_{false};
};

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
