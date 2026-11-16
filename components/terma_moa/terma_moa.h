/*
 * AI-Provenance:
 *   model: claude-opus-4-8[1m]
 *   harness: Claude Code
 *   skills:
 *     - brainstorming
 */
#pragma once

#ifdef USE_ESP32

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/components/ble_client/ble_client.h"

#include <esp_gap_ble_api.h>
#include <esp_gattc_api.h>

#include <cstddef>
#include <cstdint>
#include <functional>
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
// It also tracks when the link is actually usable (encrypted AND services discovered)
// and fires `on_ready`, so the YAML never has to guess with a fixed delay.
//
// And it owns the radiator's protocol generation, so every value and mode byte the
// YAML reads or writes goes through the helpers below instead of hand-rolled bytes.
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
  void add_on_ready_callback(std::function<void()> &&callback) {
    this->ready_callback_.add(std::move(callback));
  }

  void set_protocol(TermaProtocol protocol) { this->protocol_ = protocol; }
  bool is_legacy() const { return this->protocol_ == TermaProtocol::LEGACY; }

  // ---- 16-bit values (temperatures x10, timer seconds) ----
  // Decode the uint16 at x[offset], x[offset + 1]. Caller ensures x.size() >= offset + 2.
  uint16_t decode_u16(const std::vector<uint8_t> &x, size_t offset) const;
  // Encode {first, second} as a 4-byte payload. Targets: (0, temp_x10); timer: (s, s).
  std::vector<uint8_t> encode_u16_pair(uint16_t first, uint16_t second) const;
  // Payload for a target temperature write (b1 room / b2 element): clamps to the
  // mode's range (room 15-30 C, element 30-60 C) and rounds to the nearest 0.1 C.
  std::vector<uint8_t> encode_target(bool room, float celsius) const;

  // ---- operating mode (b3) ----
  // Mode byte for continuous heating in room or element regulation.
  int mode_manual(bool room) const;
  // Mode byte that starts the auto-off timer in room or element regulation (modern only).
  int mode_timer(bool room) const { return room ? 0x03 : 0x05; }
  // Payload for writing a mode byte: 1 byte (legacy) or 4 bytes (modern).
  std::vector<uint8_t> encode_mode(int mode) const;
  bool is_heating(int mode) const;
  bool is_room_mode(int mode) const;
  bool is_element_mode(int mode) const;
  bool is_timer_mode(int mode) const;
  bool supports_timer() const { return !this->is_legacy(); }
  // Human-readable status for the mode byte. The pointer stays valid until the next call.
  const char *mode_name(int mode);

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
  CallbackManager<void()> ready_callback_;
  char mode_name_buf_[16];
};

// `on_ready:` automation - fires each time the link becomes encrypted and usable.
class TermaReadyTrigger : public Trigger<> {
 public:
  explicit TermaReadyTrigger(TermaMoa *parent) {
    parent->add_on_ready_callback([this]() { this->trigger(); });
  }
};

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
