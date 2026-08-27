"""Passive CoIoT (CoAP) listener for Gen1 Shelly energy meters.

The component joins the CoIoT multicast group 224.0.1.187:5683 and decodes the
status packets the Shelly pushes out by itself. There is no polling and no
HTTP request at all -- and because every packet carries the sender's IP plus
the device id in CoAP option 3332, the Shelly is discovered rather than
configured. A DHCP lease change is a non-event.
"""

import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_MODEL, CONF_TIMEOUT

CODEOWNERS = ["@kevin"]
DEPENDENCIES = ["network"]
MULTI_CONF = True

shelly_coiot_ns = cg.esphome_ns.namespace("shelly_coiot")
ShellyCoiot = shelly_coiot_ns.class_("ShellyCoiot", cg.Component)

CONF_SHELLY_COIOT_ID = "shelly_coiot_id"
CONF_MAC = "mac"
CONF_MARK_UNAVAILABLE = "mark_unavailable_on_timeout"


def validate_mac(value):
    """Accept AA:BB:CC:DD:EE:FF, aa-bb-..., or plain hex."""
    value = cv.string_strict(value)
    cleaned = value.replace(":", "").replace("-", "").replace(".", "").upper()
    if not re.fullmatch(r"[0-9A-F]{12}", cleaned):
        raise cv.Invalid(
            f"'{value}' is not a valid MAC address. Expected 12 hex digits, "
            "e.g. A4:CF:12:34:56:78 or A4CF12345678."
        )
    return cleaned


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(ShellyCoiot),
        # Optional: pin the listener to exactly one device. Without it, the
        # first device whose id starts with `model` wins.
        cv.Optional(CONF_MAC): validate_mac,
        # Model prefix of the CoIoT device id ("SHEM-3#<mac>#2"). Set to ""
        # to accept any Gen1 Shelly.
        cv.Optional(CONF_MODEL, default="SHEM-3"): cv.string,
        # Shelly's default CoIoT update period is 15 s; 60 s is four missed
        # packets before we call it offline.
        cv.Optional(CONF_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_MARK_UNAVAILABLE, default=True): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_MAC in config:
        cg.add(var.set_mac_filter(config[CONF_MAC]))
    cg.add(var.set_model(config[CONF_MODEL]))
    cg.add(var.set_timeout(config[CONF_TIMEOUT]))
    cg.add(var.set_mark_unavailable(config[CONF_MARK_UNAVAILABLE]))


# Shared schema fragment for the sensor/binary_sensor/text_sensor platforms.
SHELLY_COIOT_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_SHELLY_COIOT_ID): cv.use_id(ShellyCoiot),
    }
)
