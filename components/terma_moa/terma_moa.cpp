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

namespace esphome {
namespace terma_moa {

static const char *const TAG = "terma_moa";

void TermaMoa::dump_config() {
  ESP_LOGCONFIG(TAG, "Terma MOA Blue: requesting link encryption automatically on connect");
  ESP_LOGCONFIG(TAG, "  Byte order: %s", this->is_big_endian() ? "big" : "little");
}

uint16_t TermaMoa::decode_u16(const std::vector<uint8_t> &x, size_t offset) const {
  const uint8_t a = x[offset];
  const uint8_t b = x[offset + 1];
  return this->is_big_endian() ? (uint16_t) ((a << 8) | b) : (uint16_t) ((b << 8) | a);
}

std::vector<uint8_t> TermaMoa::encode_u16_pair(uint16_t first, uint16_t second) const {
  if (this->is_big_endian()) {
    return {(uint8_t) (first >> 8), (uint8_t) (first & 0xFF), (uint8_t) (second >> 8),
            (uint8_t) (second & 0xFF)};
  }
  return {(uint8_t) (first & 0xFF), (uint8_t) (first >> 8), (uint8_t) (second & 0xFF),
          (uint8_t) (second >> 8)};
}

void TermaMoa::check_element_byte_order(const std::vector<uint8_t> &x) {
  if (this->byte_order_warned_ || x.size() < 4)
    return;
  const float as_configured = this->decode_u16(x, 2) / 10.0f;
  const uint16_t swapped_raw = this->is_big_endian() ? (uint16_t) ((x[3] << 8) | x[2])
                                                     : (uint16_t) ((x[2] << 8) | x[3]);
  const float swapped = swapped_raw / 10.0f;
  const bool configured_ok = as_configured >= 30.0f && as_configured <= 60.0f;
  const bool swapped_ok = swapped >= 30.0f && swapped <= 60.0f;
  if (!configured_ok && swapped_ok) {
    this->byte_order_warned_ = true;
    ESP_LOGW(TAG,
             "Element target reads %.1f C with byte_order: %s, but %.1f C the other way round. "
             "This radiator looks %s-endian: set byte_order: %s (substitution) and reflash.",
             as_configured, this->is_big_endian() ? "big" : "little", swapped,
             this->is_big_endian() ? "little" : "big", this->is_big_endian() ? "little" : "big");
  }
}

void TermaMoa::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_OPEN_EVT:
      // The parent BLEClient only forwards events for our own connection, and it has
      // already verified the peer address, so param->open.remote_bda is the radiator's
      // address. Request encryption the instant the link opens.
      if (param->open.status == ESP_GATT_OK) {
        esp_err_t err = esp_ble_set_encryption(param->open.remote_bda, ESP_BLE_SEC_ENCRYPT);
        if (err == ESP_OK) {
          ESP_LOGI(TAG, "Encryption requested for bonded device");
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
      break;
    default:
      break;
  }
}

}  // namespace terma_moa
}  // namespace esphome

#endif  // USE_ESP32
