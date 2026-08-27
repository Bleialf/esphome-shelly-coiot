"""Sensor platform for the Shelly 3EM CoIoT listener.

Sensor id map for SHEM-3 (verified against the /cit/d description used by the
ioBroker Shelly adapter and the openHAB Shelly binding):

    channel 0 (emeter_0): 4105 power, 4106 energy, 4107 energyReturned,
                          4108 voltage, 4109 current, 4110 powerFactor
    channel 1 (emeter_1): 4205 .. 4210
    channel 2 (emeter_2): 4305 .. 4310

Energy values arrive in Wh and are converted to kWh here so they drop straight
into the Home Assistant energy dashboard.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_CURRENT,
    CONF_ENERGY,
    CONF_POWER,
    CONF_POWER_FACTOR,
    CONF_VOLTAGE,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_ENERGY,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_POWER_FACTOR,
    DEVICE_CLASS_VOLTAGE,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_AMPERE,
    UNIT_KILOWATT_HOURS,
    UNIT_VOLT,
    UNIT_WATT,
)

from . import SHELLY_COIOT_CLIENT_SCHEMA, CONF_SHELLY_COIOT_ID

DEPENDENCIES = ["shelly_coiot"]

CONF_PHASE_A = "phase_a"
CONF_PHASE_B = "phase_b"
CONF_PHASE_C = "phase_c"
CONF_ENERGY_RETURNED = "energy_returned"
CONF_TOTAL_POWER = "total_power"
CONF_TOTAL_ENERGY = "total_energy"
CONF_TOTAL_ENERGY_RETURNED = "total_energy_returned"

PHASE_KEYS = [CONF_PHASE_A, CONF_PHASE_B, CONF_PHASE_C]

POWER_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_WATT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_POWER,
    state_class=STATE_CLASS_MEASUREMENT,
)
VOLTAGE_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_VOLT,
    accuracy_decimals=1,
    device_class=DEVICE_CLASS_VOLTAGE,
    state_class=STATE_CLASS_MEASUREMENT,
)
CURRENT_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_AMPERE,
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_CURRENT,
    state_class=STATE_CLASS_MEASUREMENT,
)
POWER_FACTOR_SCHEMA = sensor.sensor_schema(
    accuracy_decimals=2,
    device_class=DEVICE_CLASS_POWER_FACTOR,
    state_class=STATE_CLASS_MEASUREMENT,
)
ENERGY_SCHEMA = sensor.sensor_schema(
    unit_of_measurement=UNIT_KILOWATT_HOURS,
    accuracy_decimals=3,
    device_class=DEVICE_CLASS_ENERGY,
    state_class=STATE_CLASS_TOTAL_INCREASING,
)

# quantity -> (id offset within the channel block, scaling factor)
QUANTITIES = {
    CONF_POWER: (5, 1.0, POWER_SCHEMA),
    CONF_ENERGY: (6, 0.001, ENERGY_SCHEMA),
    CONF_ENERGY_RETURNED: (7, 0.001, ENERGY_SCHEMA),
    CONF_VOLTAGE: (8, 1.0, VOLTAGE_SCHEMA),
    CONF_CURRENT: (9, 1.0, CURRENT_SCHEMA),
    CONF_POWER_FACTOR: (10, 1.0, POWER_FACTOR_SCHEMA),
}

PHASE_SCHEMA = cv.Schema(
    {cv.Optional(name): schema for name, (_, _, schema) in QUANTITIES.items()}
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.Optional(CONF_PHASE_A): PHASE_SCHEMA,
            cv.Optional(CONF_PHASE_B): PHASE_SCHEMA,
            cv.Optional(CONF_PHASE_C): PHASE_SCHEMA,
            cv.Optional(CONF_TOTAL_POWER): POWER_SCHEMA,
            cv.Optional(CONF_TOTAL_ENERGY): ENERGY_SCHEMA,
            cv.Optional(CONF_TOTAL_ENERGY_RETURNED): ENERGY_SCHEMA,
        }
    )
    .extend(SHELLY_COIOT_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    hub = await cg.get_variable(config[CONF_SHELLY_COIOT_ID])

    for channel, phase_key in enumerate(PHASE_KEYS):
        if phase_key not in config:
            continue
        phase = config[phase_key]
        for quantity, (offset, factor, _) in QUANTITIES.items():
            if quantity not in phase:
                continue
            sens = await sensor.new_sensor(phase[quantity])
            sensor_id = 4100 + channel * 100 + offset
            cg.add(hub.register_value_sensor(sensor_id, sens, factor))

    if CONF_TOTAL_POWER in config:
        sens = await sensor.new_sensor(config[CONF_TOTAL_POWER])
        cg.add(hub.set_total_power_sensor(sens))
    if CONF_TOTAL_ENERGY in config:
        sens = await sensor.new_sensor(config[CONF_TOTAL_ENERGY])
        cg.add(hub.set_total_energy_sensor(sens))
    if CONF_TOTAL_ENERGY_RETURNED in config:
        sens = await sensor.new_sensor(config[CONF_TOTAL_ENERGY_RETURNED])
        cg.add(hub.set_total_energy_returned_sensor(sens))
