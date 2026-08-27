"""Text sensors exposing what discovery found: the Shelly's current IP and its
CoIoT device id. Both update by themselves when the Shelly gets a new lease."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import ENTITY_CATEGORY_DIAGNOSTIC

from . import SHELLY_COIOT_CLIENT_SCHEMA, CONF_SHELLY_COIOT_ID

DEPENDENCIES = ["shelly_coiot"]

CONF_IP_ADDRESS = "ip_address"
CONF_DEVICE_ID = "device_id"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_IP_ADDRESS): text_sensor.text_sensor_schema(
                icon="mdi:ip-network",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_DEVICE_ID): text_sensor.text_sensor_schema(
                icon="mdi:identifier",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(SHELLY_COIOT_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SHELLY_COIOT_ID])

    if CONF_IP_ADDRESS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_IP_ADDRESS])
        cg.add(hub.set_ip_text_sensor(sens))
    if CONF_DEVICE_ID in config:
        sens = await text_sensor.new_text_sensor(config[CONF_DEVICE_ID])
        cg.add(hub.set_device_id_text_sensor(sens))
