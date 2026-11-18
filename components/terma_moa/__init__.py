# AI-Provenance:
#   model: claude-opus-4-8[1m]
#   harness: Claude Code
#   skills:
#     - brainstorming
#
# ESPHome external component for Terma Blue Line (MOA Blue) radiators.
#
# This file is only used at build time: it defines the YAML the component accepts
# and generates the C++ that creates it. The behaviour is in terma_moa.cpp.
#
# The component attaches to an existing `ble_client` and:
#   - requests link encryption as soon as the connection opens (the radiator
#     insists on it, and ESPHome never starts it itself),
#   - says when the link is ready to use: is_ready(),
#   - reads back the radiator's mode and element target: request_read(),
#   - builds and interprets values for the radiator's protocol generation.
# Pairing (the passkey) and bond handling stay with the native `ble_client` in YAML.
#
# Example:
#   terma_moa:
#     id: terma_hub
#     ble_client_id: terma_moa_ble_client
#     protocol: legacy   # optional, default modern

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client"]

CONF_PROTOCOL = "protocol"

# The C++ names this file refers to.
terma_moa_ns = cg.esphome_ns.namespace("terma_moa")
TermaMoa = terma_moa_ns.class_("TermaMoa", cg.Component, ble_client.BLEClientNode)
TermaProtocol = terma_moa_ns.enum("TermaProtocol", is_class=True)

# Protocol generations (see README "Protocol versions" and TermaProtocol in terma_moa.h).
PROTOCOLS = {
    "modern": TermaProtocol.MODERN,
    "legacy": TermaProtocol.LEGACY,
}

CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TermaMoa),
            cv.Optional(CONF_PROTOCOL, default="modern"): cv.enum(PROTOCOLS, lower=True),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)  # adds ble_client_id
    .extend(cv.COMPONENT_SCHEMA),
    # The C++ uses the ESP32's Bluetooth APIs: reject other chips here rather than
    # with a confusing compile error.
    cv.only_on_esp32,
)


async def to_code(config):
    # Generates, roughly:
    #   auto *terma_hub = new terma_moa::TermaMoa();
    #   App.register_component(terma_hub);
    #   terma_moa_ble_client->register_ble_node(terma_hub);
    #   terma_hub->set_protocol(terma_moa::TermaProtocol::LEGACY);
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_protocol(config[CONF_PROTOCOL]))
