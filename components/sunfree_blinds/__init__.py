import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
from esphome.components.cc1101 import CC1101Component

CODEOWNERS = ["@amasolov"]
DEPENDENCIES = ["cc1101", "api"]
AUTO_LOAD = ["cover", "sensor"]

CONF_HUB_ID = "hub_id"
CONF_CC1101_ID = "cc1101_id"
CONF_GROUPS = "groups"
CONF_MOTOR_IDS = "motor_ids"

sunfree_ns = cg.esphome_ns.namespace("sunfree_blinds")
SunfreeHub = sunfree_ns.class_("SunfreeHub", cg.Component)

MULTI_CONF = False

GROUP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Required(CONF_MOTOR_IDS): cv.ensure_list(cv.string_strict),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SunfreeHub),
        cv.Optional(CONF_HUB_ID): cv.string_strict,
        cv.Required(CONF_CC1101_ID): cv.use_id(CC1101Component),
        cv.Optional(CONF_GROUPS, default=[]): cv.ensure_list(GROUP_SCHEMA),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_HUB_ID in config:
        cg.add(var.set_hub_id(config[CONF_HUB_ID]))

    cc1101 = await cg.get_variable(config[CONF_CC1101_ID])
    cg.add(var.set_cc1101(cc1101))

    for group in config.get(CONF_GROUPS, []):
        motor_ids = cg.RawExpression(
            "std::vector<std::string>{"
            + ",".join(f'"{m}"' for m in group[CONF_MOTOR_IDS])
            + "}"
        )
        cg.add(var.add_group(group[CONF_NAME], motor_ids))
