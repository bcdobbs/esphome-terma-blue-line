# AI-Provenance:
#   model: claude-opus-4-8[1m]
#   harness: Claude Code
#   skills:
#     - brainstorming
#
# Minimal ESPHome external component for the Terma MOA Blue radiator.
#
# It registers itself as a node on an existing `ble_client` and proactively
# requests link encryption the moment the connection opens. This is the one
# piece of behavior ESPHome does not provide natively (it accepts encryption
# when the peer initiates it, but never initiates it itself). Bond clearing and
# passkey handling are left to native `ble_client` features in the YAML.

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client
from esphome.const import CONF_ID

DEPENDENCIES = ["ble_client"]

terma_moa_ns = cg.esphome_ns.namespace("terma_moa")
TermaMoa = terma_moa_ns.class_("TermaMoa", cg.Component, ble_client.BLEClientNode)

# Byte order of the radiator's 16-bit values (temperatures x10, timer seconds).
# Most units are little-endian; some report big-endian. See README "Byte order".
CONF_BYTE_ORDER = "byte_order"
TermaByteOrder = terma_moa_ns.enum("TermaByteOrder", is_class=True)
BYTE_ORDERS = {
    "little": TermaByteOrder.LITTLE,
    "big": TermaByteOrder.BIG,
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TermaMoa),
            cv.Optional(CONF_BYTE_ORDER, default="little"): cv.enum(BYTE_ORDERS, lower=True),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_byte_order(config[CONF_BYTE_ORDER]))
