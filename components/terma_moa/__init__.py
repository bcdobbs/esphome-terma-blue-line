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

# Two protocol generations exist in the field (see README "Protocol versions"):
#   modern - little-endian values, 4-byte mode characteristic, modes 0x11 / 0x21,
#            native auto-off timer (the units this project was developed on).
#   legacy - big-endian values, 1-byte mode characteristic, modes 5 (room) / 6
#            (element), no timer (the protocol J1mbo's homebridge plugin targets).
CONF_PROTOCOL = "protocol"
TermaProtocol = terma_moa_ns.enum("TermaProtocol", is_class=True)
PROTOCOLS = {
    "modern": TermaProtocol.MODERN,
    "legacy": TermaProtocol.LEGACY,
}

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(TermaMoa),
            cv.Optional(CONF_PROTOCOL, default="modern"): cv.enum(PROTOCOLS, lower=True),
        }
    )
    .extend(ble_client.BLE_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await ble_client.register_ble_node(var, config)
    cg.add(var.set_protocol(config[CONF_PROTOCOL]))
