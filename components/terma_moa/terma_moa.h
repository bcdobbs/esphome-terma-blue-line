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

#include <cmath>
#include <cstdint>
#include <vector>

namespace esphome {
namespace terma_moa {

// The generation of Bluetooth protocol the radiator speaks, set with `protocol:` in YAML.
// See README "Protocol versions".
//   MODERN: 16-bit values little-endian, 4-byte mode payload, element mode 0x21.
//   LEGACY: 16-bit values big-endian,    1-byte mode payload, element mode 0x06.
enum class TermaProtocol : uint8_t { MODERN = 0, LEGACY = 1 };

// A `ble_client` node for a Terma Blue Line radiator.
//
// The radiator only talks over an encrypted link, and ESPHome never starts encryption
// itself, so this component requests it the moment the connection opens. It also:
//   - says when the link is usable (is_ready),
//   - reads back the radiator's mode and element target (request_read),
//   - builds and interprets values for the configured protocol (encode_*, is_*).
//
// The public methods in the first three groups are the ones the YAML lambdas call.
class TermaMoa : public Component, public ble_client::BLEClientNode {
 public:
  // ---- Connection ----

  // True once the link is encrypted and services are discovered; false again as soon
  // as it drops. Gate every read and write on this.
  bool is_ready();

  // ---- Reading the radiator's state ----

  // Start reading the mode, then the element target. Returns false (and reads
  // nothing) if the link isn't ready.
  bool request_read();
  // True until both answers are in, or a read fails.
  bool is_reading() const { return this->reading_ != Reading::NONE; }
  // Results of the last request_read(): -1 and NAN until the radiator answers.
  int read_mode() const { return this->read_mode_; }
  float read_target() const { return this->read_target_; }

  // ---- Building and interpreting values (these follow the configured protocol) ----

  // The mode byte for continuous element heating.
  int mode_element() const;
  // True only for continuous element heating. A modern unit running its auto-off
  // timer (0x05) doesn't count: it would switch itself off.
  bool is_element_mode(int mode) const { return mode == this->mode_element(); }
  // True for any mode except off.
  bool is_heating(int mode) const;
  // Payload to write to the mode characteristic.
  std::vector<uint8_t> encode_mode(int mode) const;
  // Payload to write to the element characteristic: clamped to 30-60 C, rounded to 0.1 C.
  std::vector<uint8_t> encode_element_target(float celsius) const;

  // ---- Called by ESPHome ----

  void set_protocol(TermaProtocol protocol) { this->protocol_ = protocol; }
  void dump_config() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;

 protected:
  enum class Reading : uint8_t { NONE, MODE, ELEMENT };

  // Connection events, in the order they normally happen.
  void on_open_(esp_ble_gattc_cb_param_t::gattc_open_evt_param &open);
  void on_rssi_(esp_ble_gap_cb_param_t::ble_read_rssi_cmpl_evt_param &rssi);
  void on_auth_complete_(esp_ble_auth_cmpl_t &auth);
  void on_services_discovered_();
  void on_disconnect_();
  void update_ready_();

  // Reading.
  bool send_read_(uint16_t handle);
  void on_read_(esp_ble_gattc_cb_param_t::gattc_read_char_evt_param &read);
  void on_mode_read_(const std::vector<uint8_t> &payload);
  void on_element_read_(const std::vector<uint8_t> &payload);

  // Protocol.
  bool is_legacy_() const { return this->protocol_ == TermaProtocol::LEGACY; }
  float decode_element_target_(const std::vector<uint8_t> &payload) const;
  // Warn (once) if a payload looks like the other protocol generation.
  void check_mode_payload_(const std::vector<uint8_t> &payload);
  void check_element_payload_(const std::vector<uint8_t> &payload);
  void warn_protocol_(const char *evidence);

  TermaProtocol protocol_{TermaProtocol::MODERN};
  bool protocol_warned_{false};

  // Link state. ready_ = encrypted_ && discovered_, kept so "ready" is logged once.
  bool encrypted_{false};
  bool discovered_{false};
  bool ready_{false};

  // Characteristic handles, found during service discovery (0 = not found).
  uint16_t mode_handle_{0};
  uint16_t element_handle_{0};

  // The read in progress, and the results of the last one.
  Reading reading_{Reading::NONE};
  int read_mode_{-1};
  float read_target_{NAN};
};

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
