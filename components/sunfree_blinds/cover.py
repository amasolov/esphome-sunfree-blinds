import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import cover, sensor
from esphome.const import (
    DEVICE_CLASS_BATTERY,
    ENTITY_CATEGORY_DIAGNOSTIC,
    UNIT_PERCENT,
    ICON_BATTERY,
)
from . import sunfree_ns, SunfreeHub

DEPENDENCIES = ["sunfree_blinds"]

CONF_SUNFREE_HUB_ID = "sunfree_hub_id"
CONF_MOTOR_ID = "motor_id"
CONF_BATTERY = "battery"

SunfreeCover = sunfree_ns.class_("SunfreeCover", cover.Cover, cg.Component)

CONFIG_SCHEMA = cover.cover_schema(SunfreeCover).extend(
    {
        cv.GenerateID(CONF_SUNFREE_HUB_ID): cv.use_id(SunfreeHub),
        cv.Required(CONF_MOTOR_ID): cv.string_strict,
        cv.Optional(CONF_BATTERY): sensor.sensor_schema(
            unit_of_measurement=UNIT_PERCENT,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_BATTERY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon=ICON_BATTERY,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await cover.new_cover(config)
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_SUNFREE_HUB_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_motor_id(config[CONF_MOTOR_ID]))

    if CONF_BATTERY in config:
        bat = await sensor.new_sensor(config[CONF_BATTERY])
        cg.add(var.set_battery_sensor(bat))
