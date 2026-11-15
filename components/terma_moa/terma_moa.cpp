/*
 * AI-Provenance:
 *   model: claude-opus-4-8[1m]
 *   harness: Claude Code
 *   skills:
 *     - brainstorming
 */
#ifdef USE_ESP32

#include "terma_moa.h"
#include "esphome/core/log.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"

#include <cmath>
#include <cstdio>

namespace esphome {
namespace terma_moa {

static const char *const TAG = "terma_moa";

void TermaMoa::dump_config() {
  ESP_LOGCONFIG(TAG, "Terma MOA Blue: requests link encryption automatically on connect");
  ESP_LOGCONFIG(TAG, "  Protocol: %s", this->is_legacy() ? "legacy" : "modern");
}

uint16_t TermaMoa::decode_u16(const std::vector<uint8_t> &x, size_t offset) const {
  const uint8_t a = x[offset];
  const uint8_t b = x[offset + 1];
  return this->is_legacy() ? (uint16_t) ((a << 8) | b) : (uint16_t) ((b << 8) | a);
}

std::vector<uint8_t> TermaMoa::encode_u16_pair(uint16_t first, uint16_t second) const {
  if (this->is_legacy()) {
    return {(uint8_t) (first >> 8), (uint8_t) (first & 0xFF), (uint8_t) (second >> 8),
            (uint8_t) (second & 0xFF)};
  }
  return {(uint8_t) (first & 0xFF), (uint8_t) (first >> 8), (uint8_t) (second & 0xFF),
          (uint8_t) (second >> 8)};
}

std::vector<uint8_t> TermaMoa::encode_target(bool room, float celsius) const {
  const float lo = room ? 15.0f : 30.0f;
  const float hi = room ? 30.0f : 60.0f;
  if (std::isnan(celsius))
    celsius = lo;
  const float c = celsius < lo ? lo : (celsius > hi ? hi : celsius);
  return this->encode_u16_pair(0, (uint16_t) std::lround(c * 10.0f));
}

int TermaMoa::mode_manual(bool room) const {
  if (this->is_legacy())
    return room ? 0x05 : 0x06;
  return room ? 0x11 : 0x21;
}

std::vector<uint8_t> TermaMoa::encode_mode(int mode) const {
  if (this->is_legacy())
    return {(uint8_t) mode};
  return {(uint8_t) mode, 0, 0, 0};
}

bool TermaMoa::is_heating(int mode) const {
  if (this->is_legacy())
    return mode != 0x00;
  // Modern: off is 0x00 (physical) or 0x20 (via app on newer firmware).
  return mode != 0x00 && mode != 0x20;
}

bool TermaMoa::is_room_mode(int mode) const {
  if (this->is_legacy())
    return mode == 0x05;
  return mode == 0x11 || mode == 0x03;
}

bool TermaMoa::is_element_mode(int mode) const {
  if (this->is_legacy())
    return mode == 0x06;
  return mode == 0x21 || mode == 0x05;
}

bool TermaMoa::is_timer_mode(int mode) const {
  if (this->is_legacy())
    return false;
  return mode == 0x03 || mode == 0x05 || mode == 0x09;
}

const char *TermaMoa::mode_name(int mode) {
  if (this->is_legacy()) {
    switch (mode) {
      case 0x00:
        return "Off";
      case 0x05:
        return "Room";
      case 0x06:
        return "Element";
      default:
        break;
    }
  } else {
    // The modern b3 byte is a bitfield. Observed: 0x00 off, 0x20 off (remote/app),
    // 0x01 on, 0x03 room+timer, 0x05 element+timer, 0x09 physical timer,
    // 0x11 room+manual, 0x21 element+manual.
    switch (mode) {
      case 0x00:
        return "Off";
      case 0x20:
        return "Off (remote)";
      case 0x11:
        return "Room - Manual";
      case 0x21:
        return "Element - Manual";
      case 0x03:
        return "Room - Timer";
      case 0x05:
        return "Element - Timer";
      case 0x09:
        return "Timer (physical)";
      case 0x01:
        return "On";
      default:
        break;
    }
  }
  snprintf(this->mode_name_buf_, sizeof(this->mode_name_buf_), "On (0x%02X)", mode & 0xFF);
  return this->mode_name_buf_;
}

void TermaMoa::warn_protocol_(const char *evidence) {
  if (this->protocol_warned_)
    return;
  this->protocol_warned_ = true;
  const char *other = this->is_legacy() ? "modern" : "legacy";
  ESP_LOGW(TAG,
           "%s - this radiator looks like it speaks the %s protocol. Set the substitution "
           "protocol: %s and reflash (see README \"Protocol versions\").",
           evidence, other, other);
}

void TermaMoa::check_element_payload(const std::vector<uint8_t> &x) {
  if (this->protocol_warned_ || x.size() < 4)
    return;
  // The element target (bytes 2-3) is always 30-60 C. If it's only in range when read
  // in the other byte order, the protocol setting is wrong.
  const float as_configured = this->decode_u16(x, 2) / 10.0f;
  const uint16_t swapped_raw =
      this->is_legacy() ? (uint16_t) ((x[3] << 8) | x[2]) : (uint16_t) ((x[2] << 8) | x[3]);
  const float swapped = swapped_raw / 10.0f;
  if (!(as_configured >= 30.0f && as_configured <= 60.0f) && (swapped >= 30.0f && swapped <= 60.0f)) {
    char evidence[96];
    snprintf(evidence, sizeof(evidence), "Element target decodes to %.1f C, or %.1f C byte-swapped",
             as_configured, swapped);
    this->warn_protocol_(evidence);
  }
}

void TermaMoa::check_mode_payload(const std::vector<uint8_t> &x) {
  if (this->protocol_warned_ || x.empty())
    return;
  // Legacy units expose a 1-byte mode characteristic; modern ones a 4-byte one.
  if (!this->is_legacy() && x.size() == 1) {
    this->warn_protocol_("Mode characteristic is 1 byte (modern units send 4)");
  } else if (this->is_legacy() && x.size() >= 4) {
    this->warn_protocol_("Mode characteristic is 4 bytes (legacy units send 1)");
  }
}

void TermaMoa::set_link_state_(bool encrypted, bool discovered) {
  this->encrypted_ = encrypted;
  this->discovered_ = discovered;
  const bool ready = encrypted && discovered;
  if (ready == this->ready_)
    return;
  this->ready_ = ready;
  if (ready) {
    ESP_LOGI(TAG, "Link encrypted and services discovered - radiator ready");
    // Defer to the next loop: the other ble_client nodes (the sensors) may not have
    // handled SEARCH_CMPL yet, and on_ready usually reads them straight away.
    this->defer([this]() {
      if (this->is_ready())
        this->ready_callback_.call();
    });
  }
}

void TermaMoa::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  // The parent BLEClient only forwards GATTC events for its own connection.
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      // param->open.remote_bda is the radiator's address (already verified by the
      // parent). Request encryption the instant the link opens: with a stored bond this
      // re-encrypts with the saved keys, otherwise it starts passkey pairing.
      if (param->open.status == ESP_GATT_OK) {
        esp_err_t err = esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT);
        if (err == ESP_OK) {
          ESP_LOGI(TAG, "Link encryption requested");
        } else {
          ESP_LOGW(TAG, "esp_ble_set_encryption failed: %s", esp_err_to_name(err));
        }
      }
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      // Mark this node established once service discovery finishes, matching the
      // built-in ble_client nodes, so the parent can release its GATT service cache.
      // The parent resets node_state on disconnect, so no reset is needed here.
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      this->set_link_state_(this->encrypted_, true);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->set_link_state_(false, false);
      break;
    default:
      break;
  }
}

void TermaMoa::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // GAP events are not filtered per connection, so check the address ourselves.
  if (event != ESP_GAP_BLE_AUTH_CMPL_EVT || this->parent() == nullptr ||
      !this->parent()->check_addr(param->ble_security.auth_cmpl.bd_addr))
    return;
  if (param->ble_security.auth_cmpl.success) {
    this->set_link_state_(true, this->discovered_);
  } else {
    ESP_LOGW(TAG, "Encryption/pairing failed (reason 0x%02X) - is the radiator bonded, or in pairing mode?",
             param->ble_security.auth_cmpl.fail_reason);
    this->set_link_state_(false, this->discovered_);
  }
}

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
