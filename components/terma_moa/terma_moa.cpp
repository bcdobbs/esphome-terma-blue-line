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

static const char *const SERVICE_UUID = "d97352b0-d19e-11e2-9e96-0800200c9a66";
static const char *const ELEMENT_UUID = "d97352b2-d19e-11e2-9e96-0800200c9a66";  // b2
static const char *const MODE_UUID = "d97352b3-d19e-11e2-9e96-0800200c9a66";     // b3

// Below this the link is marginal: expect failed connects and slow service discovery.
static const int WEAK_RSSI_DBM = -85;

void TermaMoa::dump_config() {
  ESP_LOGCONFIG(TAG, "Terma MOA Blue: requests link encryption automatically on connect");
  ESP_LOGCONFIG(TAG, "  Protocol: %s", this->is_legacy() ? "legacy" : "modern");
}

uint16_t TermaMoa::decode_u16(const std::vector<uint8_t> &x, size_t offset) const {
  const uint8_t a = x[offset];
  const uint8_t b = x[offset + 1];
  return this->is_legacy() ? (uint16_t) ((a << 8) | b) : (uint16_t) ((b << 8) | a);
}

std::vector<uint8_t> TermaMoa::encode_element_target(float celsius) const {
  if (std::isnan(celsius))
    celsius = 30.0f;
  const float c = celsius < 30.0f ? 30.0f : (celsius > 60.0f ? 60.0f : celsius);
  const uint16_t v = (uint16_t) std::lround(c * 10.0f);
  // Bytes 0-1 zero, target in bytes 2-3, in the protocol's byte order.
  if (this->is_legacy())
    return {0, 0, (uint8_t) (v >> 8), (uint8_t) (v & 0xFF)};
  return {0, 0, (uint8_t) (v & 0xFF), (uint8_t) (v >> 8)};
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

void TermaMoa::find_handles_() {
  // Called on SEARCH_CMPL, while the parent still holds the discovered services
  // (it releases them once every node is established).
  using esp32_ble_tracker::ESPBTUUID;
  const ESPBTUUID service = ESPBTUUID::from_raw(SERVICE_UUID);
  auto *mode = this->parent()->get_characteristic(service, ESPBTUUID::from_raw(MODE_UUID));
  auto *element = this->parent()->get_characteristic(service, ESPBTUUID::from_raw(ELEMENT_UUID));
  this->mode_handle_ = mode != nullptr ? mode->handle : 0;
  this->element_handle_ = element != nullptr ? element->handle : 0;
  if (this->mode_handle_ == 0 || this->element_handle_ == 0)
    ESP_LOGW(TAG, "Radiator's mode/element characteristics not found - is this a Terma Blue Line?");
}

bool TermaMoa::send_read_(uint16_t handle) {
  esp_err_t err = esp_ble_gattc_read_char((esp_gatt_if_t) this->parent()->get_gattc_if(),
                                          this->parent()->get_conn_id(), handle, ESP_GATT_AUTH_REQ_NONE);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Read request failed: %s", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool TermaMoa::request_read() {
  this->read_mode_ = -1;
  this->read_target_ = NAN;
  this->reading_ = Reading::NONE;
  if (!this->is_ready() || this->mode_handle_ == 0 || this->element_handle_ == 0)
    return false;
  // One at a time: mode first, then the element on its answer.
  if (!this->send_read_(this->mode_handle_))
    return false;
  this->reading_ = Reading::MODE;
  return true;
}

void TermaMoa::on_read_(const esp_ble_gattc_cb_param_t::gattc_read_char_evt_param &read) {
  if (this->reading_ == Reading::MODE && read.handle == this->mode_handle_) {
    this->reading_ = Reading::NONE;
    if (read.status != ESP_GATT_OK) {
      ESP_LOGW(TAG, "Reading the mode failed (status %d)", read.status);
      return;
    }
    const std::vector<uint8_t> x(read.value, read.value + read.value_len);
    this->check_mode_payload(x);
    if (x.empty())
      return;
    this->read_mode_ = x[0];
    if (this->send_read_(this->element_handle_))
      this->reading_ = Reading::ELEMENT;
  } else if (this->reading_ == Reading::ELEMENT && read.handle == this->element_handle_) {
    this->reading_ = Reading::NONE;
    if (read.status != ESP_GATT_OK) {
      ESP_LOGW(TAG, "Reading the element target failed (status %d)", read.status);
      return;
    }
    const std::vector<uint8_t> x(read.value, read.value + read.value_len);
    if (x.size() < 4)
      return;
    this->check_element_payload(x);
    this->read_target_ = this->decode_u16(x, 2) / 10.0f;
  }
}

void TermaMoa::set_link_state_(bool encrypted, bool discovered) {
  this->encrypted_ = encrypted;
  this->discovered_ = discovered;
  const bool ready = encrypted && discovered;
  if (ready == this->ready_)
    return;
  this->ready_ = ready;
  if (ready)
    ESP_LOGI(TAG, "Link encrypted and services discovered - radiator ready");
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
        // Log the signal strength: the first thing to check when connects fail.
        esp_ble_gap_read_rssi(param->open.remote_bda);
      }
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      // Mark this node established once service discovery finishes, matching the
      // built-in ble_client nodes, so the parent can release its GATT service cache.
      // The parent resets node_state on disconnect, so no reset is needed here.
      this->find_handles_();
      this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;
      this->set_link_state_(this->encrypted_, true);
      break;
    case ESP_GATTC_READ_CHAR_EVT:
      this->on_read_(param->read);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->set_link_state_(false, false);
      this->reading_ = Reading::NONE;
      this->mode_handle_ = 0;
      this->element_handle_ = 0;
      break;
    default:
      break;
  }
}

void TermaMoa::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // GAP events are not filtered per connection, so check the address ourselves.
  if (this->parent() == nullptr)
    return;
  if (event == ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT) {
    if (!this->parent()->check_addr(param->read_rssi_cmpl.remote_addr) ||
        param->read_rssi_cmpl.status != ESP_BT_STATUS_SUCCESS)
      return;
    const int rssi = param->read_rssi_cmpl.rssi;
    if (rssi < WEAK_RSSI_DBM) {
      ESP_LOGW(TAG, "Radiator signal: %d dBm - weak, expect failed or slow connections", rssi);
    } else {
      ESP_LOGI(TAG, "Radiator signal: %d dBm", rssi);
    }
    return;
  }
  if (event != ESP_GAP_BLE_AUTH_CMPL_EVT || !this->parent()->check_addr(param->ble_security.auth_cmpl.bd_addr))
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
