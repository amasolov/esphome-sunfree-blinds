import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import button
from . import sunfree_ns, SunfreeHub

DEPENDENCIES = ["sunfree_blinds"]

CONF_SUNFREE_HUB_ID = "sunfree_hub_id"
CONF_MOTOR_ID = "motor_id"
CONF_ACTION_TYPE = "action_type"

SunfreeButton = sunfree_ns.class_("SunfreeButton", button.Button, cg.Component)

ACTION_TYPES = {
    "direction_forward": 0,
    "direction_reverse": 1,
    "set_open_limit": 2,
    "set_close_limit": 3,
    "save_favourite": 4,
    "goto_favourite": 5,
}

CONFIG_SCHEMA = button.button_schema(SunfreeButton).extend(
    {
        cv.GenerateID(CONF_SUNFREE_HUB_ID): cv.use_id(SunfreeHub),
        cv.Required(CONF_MOTOR_ID): cv.string_strict,
        cv.Required(CONF_ACTION_TYPE): cv.enum(ACTION_TYPES, lower=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = await button.new_button(config)
    await cg.register_component(var, config)

    hub = await cg.get_variable(config[CONF_SUNFREE_HUB_ID])
    cg.add(var.set_hub(hub))
    cg.add(var.set_motor_id(config[CONF_MOTOR_ID]))
    cg.add(var.set_action_type(config[CONF_ACTION_TYPE]))
