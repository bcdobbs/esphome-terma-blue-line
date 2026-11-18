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

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace esphome {
namespace terma_moa {

static const char *const TAG = "terma_moa";

// The radiator's GATT service and the two characteristics we use.
static const char *const SERVICE_UUID = "d97352b0-d19e-11e2-9e96-0800200c9a66";
static const char *const ELEMENT_UUID = "d97352b2-d19e-11e2-9e96-0800200c9a66";  // "b2": element target
static const char *const MODE_UUID = "d97352b3-d19e-11e2-9e96-0800200c9a66";     // "b3": operating mode

// Mode bytes.
static const uint8_t MODE_OFF = 0x00;
static const uint8_t MODE_OFF_VIA_APP = 0x20;  // modern units, newer firmware
static const uint8_t MODE_ELEMENT_MODERN = 0x21;
static const uint8_t MODE_ELEMENT_LEGACY = 0x06;

// The element target's range. The radiator stores it in tenths of a degree.
static const float ELEMENT_MIN_C = 30.0f;
static const float ELEMENT_MAX_C = 60.0f;

// Below this the link is marginal: expect failed connects and slow service discovery.
static const int WEAK_RSSI_DBM = -85;

static bool in_element_range(float celsius) { return celsius >= ELEMENT_MIN_C && celsius <= ELEMENT_MAX_C; }

static uint16_t to_u16(uint8_t first, uint8_t second, bool big_endian) {
  return big_endian ? (first << 8) | second : (second << 8) | first;
}

void TermaMoa::dump_config() {
  ESP_LOGCONFIG(TAG, "Terma MOA Blue: requests link encryption automatically on connect");
  ESP_LOGCONFIG(TAG, "  Protocol: %s", this->is_legacy_() ? "legacy" : "modern");
}

// ---------------------------------------------------------------------------
// Connection
// ---------------------------------------------------------------------------

bool TermaMoa::is_ready() {
  // Also check the parent: it leaves the connected state before nodes see the disconnect.
  return this->ready_ && this->parent() != nullptr && this->parent()->connected();
}

void TermaMoa::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  // The parent ble_client only forwards GATTC events for its own connection.
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      this->on_open_(param->open);
      break;
    case ESP_GATTC_SEARCH_CMPL_EVT:
      this->on_services_discovered_();
      break;
    case ESP_GATTC_READ_CHAR_EVT:
      this->on_read_(param->read);
      break;
    case ESP_GATTC_DISCONNECT_EVT:
    case ESP_GATTC_CLOSE_EVT:
      this->on_disconnect_();
      break;
    default:
      break;
  }
}

void TermaMoa::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // GAP events cover every connection, so each handler checks the address.
  if (this->parent() == nullptr)
    return;
  switch (event) {
    case ESP_GAP_BLE_READ_RSSI_COMPLETE_EVT:
      this->on_rssi_(param->read_rssi_cmpl);
      break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
      this->on_auth_complete_(param->ble_security.auth_cmpl);
      break;
    default:
      break;
  }
}

void TermaMoa::on_open_(esp_ble_gattc_cb_param_t::gattc_open_evt_param &open) {
  if (open.status != ESP_GATT_OK)
    return;
  // Request encryption straight away: with a stored bond this re-encrypts with the
  // saved keys, otherwise it starts passkey pairing.
  esp_err_t err = esp_ble_set_encryption(open.remote_bda, ESP_BLE_SEC_ENCRYPT);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "Link encryption requested");
  } else {
    ESP_LOGW(TAG, "esp_ble_set_encryption failed: %s", esp_err_to_name(err));
  }
  // Ask for the signal strength (logged in on_rssi_): the first thing to check when
  // connects fail.
  esp_ble_gap_read_rssi(open.remote_bda);
}

void TermaMoa::on_rssi_(esp_ble_gap_cb_param_t::ble_read_rssi_cmpl_evt_param &rssi) {
  if (!this->parent()->check_addr(rssi.remote_addr) || rssi.status != ESP_BT_STATUS_SUCCESS)
    return;
  if (rssi.rssi < WEAK_RSSI_DBM) {
    ESP_LOGW(TAG, "Radiator signal: %d dBm - weak, expect failed or slow connections", rssi.rssi);
  } else {
    ESP_LOGI(TAG, "Radiator signal: %d dBm", rssi.rssi);
  }
}

void TermaMoa::on_auth_complete_(esp_ble_auth_cmpl_t &auth) {
  if (!this->parent()->check_addr(auth.bd_addr))
    return;
  if (!auth.success) {
    ESP_LOGW(TAG, "Encryption/pairing failed (reason 0x%02X) - is the radiator bonded, or in pairing mode?",
             auth.fail_reason);
  }
  this->encrypted_ = auth.success;
  this->update_ready_();
}

void TermaMoa::on_services_discovered_() {
  // Look up the handles now: the parent only keeps the discovered services until
  // every node is established.
  using esp32_ble_tracker::ESPBTUUID;
  const ESPBTUUID service = ESPBTUUID::from_raw(SERVICE_UUID);
  auto *mode = this->parent()->get_characteristic(service, ESPBTUUID::from_raw(MODE_UUID));
  auto *element = this->parent()->get_characteristic(service, ESPBTUUID::from_raw(ELEMENT_UUID));
  this->mode_handle_ = mode != nullptr ? mode->handle : 0;
  this->element_handle_ = element != nullptr ? element->handle : 0;
  if (this->mode_handle_ == 0 || this->element_handle_ == 0)
    ESP_LOGW(TAG, "Radiator's mode/element characteristics not found - is this a Terma Blue Line?");

  // Mark this node established, as the built-in ble_client nodes do, so the parent can
  // free its service cache. The parent resets node_state itself on disconnect.
  this->node_state = esp32_ble_tracker::ClientState::ESTABLISHED;

  this->discovered_ = true;
  this->update_ready_();
}

void TermaMoa::on_disconnect_() {
  this->encrypted_ = false;
  this->discovered_ = false;
  this->update_ready_();
  this->reading_ = Reading::NONE;
  this->mode_handle_ = 0;
  this->element_handle_ = 0;
}

// Call after changing encrypted_ or discovered_.
void TermaMoa::update_ready_() {
  const bool ready = this->encrypted_ && this->discovered_;
  if (ready && !this->ready_)
    ESP_LOGI(TAG, "Link encrypted and services discovered - radiator ready");
  this->ready_ = ready;
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

bool TermaMoa::request_read() {
  this->read_mode_ = -1;
  this->read_target_ = NAN;
  this->reading_ = Reading::NONE;
  if (!this->is_ready() || this->mode_handle_ == 0 || this->element_handle_ == 0)
    return false;
  // One read at a time: the mode now, the element target when the mode arrives.
  if (!this->send_read_(this->mode_handle_))
    return false;
  this->reading_ = Reading::MODE;
  return true;
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

void TermaMoa::on_read_(esp_ble_gattc_cb_param_t::gattc_read_char_evt_param &read) {
  // Ignore any answer we aren't waiting for.
  const bool is_mode = this->reading_ == Reading::MODE && read.handle == this->mode_handle_;
  const bool is_element = this->reading_ == Reading::ELEMENT && read.handle == this->element_handle_;
  if (!is_mode && !is_element)
    return;

  this->reading_ = Reading::NONE;
  if (read.status != ESP_GATT_OK) {
    ESP_LOGW(TAG, "Reading the %s failed (status %d)", is_mode ? "mode" : "element target", read.status);
    return;
  }

  const std::vector<uint8_t> payload(read.value, read.value + read.value_len);
  if (is_mode) {
    this->on_mode_read_(payload);
  } else {
    this->on_element_read_(payload);
  }
}

void TermaMoa::on_mode_read_(const std::vector<uint8_t> &payload) {
  this->check_mode_payload_(payload);
  if (payload.empty())
    return;
  this->read_mode_ = payload[0];
  // Now the element target.
  if (this->send_read_(this->element_handle_))
    this->reading_ = Reading::ELEMENT;
}

void TermaMoa::on_element_read_(const std::vector<uint8_t> &payload) {
  if (payload.size() < 4)
    return;
  this->check_element_payload_(payload);
  this->read_target_ = this->decode_element_target_(payload);
}

// ---------------------------------------------------------------------------
// Protocol: building and interpreting values
// ---------------------------------------------------------------------------

int TermaMoa::mode_element() const { return this->is_legacy_() ? MODE_ELEMENT_LEGACY : MODE_ELEMENT_MODERN; }

bool TermaMoa::is_heating(int mode) const {
  if (this->is_legacy_())
    return mode != MODE_OFF;
  // Modern units report off as 0x00 when switched off at the radiator, or 0x20 when
  // switched off from the app on newer firmware.
  return mode != MODE_OFF && mode != MODE_OFF_VIA_APP;
}

std::vector<uint8_t> TermaMoa::encode_mode(int mode) const {
  if (this->is_legacy_())
    return {(uint8_t) mode};
  return {(uint8_t) mode, 0, 0, 0};
}

// The element payload is 4 bytes: bytes 0-1 are zero, bytes 2-3 hold the target in
// tenths of a degree.
std::vector<uint8_t> TermaMoa::encode_element_target(float celsius) const {
  if (std::isnan(celsius))
    celsius = ELEMENT_MIN_C;
  celsius = std::clamp(celsius, ELEMENT_MIN_C, ELEMENT_MAX_C);
  const uint16_t tenths = (uint16_t) std::lround(celsius * 10.0f);
  const uint8_t high = tenths >> 8;
  const uint8_t low = tenths & 0xFF;
  if (this->is_legacy_())
    return {0, 0, high, low};
  return {0, 0, low, high};
}

// Needs payload.size() >= 4.
float TermaMoa::decode_element_target_(const std::vector<uint8_t> &payload) const {
  return to_u16(payload[2], payload[3], this->is_legacy_()) / 10.0f;
}

// ---------------------------------------------------------------------------
// Protocol: warning when the radiator looks like the other generation
// ---------------------------------------------------------------------------

void TermaMoa::check_mode_payload_(const std::vector<uint8_t> &payload) {
  if (this->protocol_warned_)
    return;
  if (!this->is_legacy_() && payload.size() == 1) {
    this->warn_protocol_("Mode characteristic is 1 byte (modern units send 4)");
  } else if (this->is_legacy_() && payload.size() >= 4) {
    this->warn_protocol_("Mode characteristic is 4 bytes (legacy units send 1)");
  }
}

// Needs payload.size() >= 4.
void TermaMoa::check_element_payload_(const std::vector<uint8_t> &payload) {
  if (this->protocol_warned_)
    return;
  // The element target is always within range. If it's only in range when read in the
  // other byte order, the protocol setting is wrong.
  const float as_configured = this->decode_element_target_(payload);
  const float byte_swapped = to_u16(payload[2], payload[3], !this->is_legacy_()) / 10.0f;
  if (!in_element_range(as_configured) && in_element_range(byte_swapped)) {
    char evidence[96];
    snprintf(evidence, sizeof(evidence), "Element target decodes to %.1f C, or %.1f C byte-swapped",
             as_configured, byte_swapped);
    this->warn_protocol_(evidence);
  }
}

void TermaMoa::warn_protocol_(const char *evidence) {
  if (this->protocol_warned_)
    return;
  this->protocol_warned_ = true;
  const char *other = this->is_legacy_() ? "modern" : "legacy";
  ESP_LOGW(TAG,
           "%s - this radiator looks like it speaks the %s protocol. Set the substitution "
           "protocol: %s and reflash (see README \"Protocol versions\").",
           evidence, other, other);
}

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
