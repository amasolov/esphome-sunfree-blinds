import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components.cc1101 import CC1101Component

CODEOWNERS = ["@amasolov"]
DEPENDENCIES = ["cc1101"]
AUTO_LOAD = ["cover", "sensor"]

CONF_HUB_ID = "hub_id"
CONF_CC1101_ID = "cc1101_id"

sunfree_ns = cg.esphome_ns.namespace("sunfree_blinds")
SunfreeHub = sunfree_ns.class_("SunfreeHub", cg.Component)

MULTI_CONF = False

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SunfreeHub),
        cv.Required(CONF_HUB_ID): cv.string_strict,
        cv.Required(CONF_CC1101_ID): cv.use_id(CC1101Component),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_hub_id(config[CONF_HUB_ID]))

    cc1101 = await cg.get_variable(config[CONF_CC1101_ID])
    cg.add(var.set_cc1101(cc1101))
