"""Binary sensors: the built-in relay, the overpower flag, and reachability."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import (
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_PROBLEM,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

from . import SHELLY_COIOT_CLIENT_SCHEMA, CONF_SHELLY_COIOT_ID

DEPENDENCIES = ["shelly_coiot"]

CONF_RELAY = "relay"
CONF_OVERPOWER = "overpower"
CONF_ONLINE = "online"

# CoIoT sensor ids on SHEM-3
ID_RELAY_OUTPUT = 1101
ID_OVERPOWER = 6102

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_RELAY): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_POWER,
            ),
            cv.Optional(CONF_OVERPOWER): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_PROBLEM,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Not a CoIoT value: goes false when no packet arrived within the
            # hub's `timeout`.
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(SHELLY_COIOT_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SHELLY_COIOT_ID])

    if CONF_RELAY in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_RELAY])
        cg.add(hub.register_bool_sensor(ID_RELAY_OUTPUT, sens))
    if CONF_OVERPOWER in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_OVERPOWER])
        cg.add(hub.register_bool_sensor(ID_OVERPOWER, sens))
    if CONF_ONLINE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_ONLINE])
        cg.add(hub.set_online_sensor(sens))
