import re

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
from esphome.components.cc1101 import CC1101Component

CODEOWNERS = ["@amasolov"]
DEPENDENCIES = ["cc1101", "api"]
AUTO_LOAD = ["cover", "sensor"]

CONF_HUB_ID = "hub_id"
CONF_CC1101_ID = "cc1101_id"
CONF_GDO0_PIN = "gdo0_pin"
CONF_GROUPS = "groups"
CONF_MOTOR_IDS = "motor_ids"


def hex_id(value):
    """Validate an 8-hex-char motor/hub ID and normalize to lowercase.

    RX dispatch formats incoming IDs as lowercase hex, so config values
    must be lowercase too or cover/group lookups silently fail.
    """
    value = cv.string_strict(value)
    if not re.fullmatch(r"[0-9a-fA-F]{8}", value):
        raise cv.Invalid(
            f"ID must be exactly 8 hex characters (0-9, a-f), got '{value}'"
        )
    return value.lower()

sunfree_ns = cg.esphome_ns.namespace("sunfree_blinds")
SunfreeHub = sunfree_ns.class_("SunfreeHub", cg.Component)

MULTI_CONF = False

GROUP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Required(CONF_MOTOR_IDS): cv.ensure_list(hex_id),
    }
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SunfreeHub),
        cv.Optional(CONF_HUB_ID): hex_id,
        cv.Required(CONF_CC1101_ID): cv.use_id(CC1101Component),
        # Must match the cc1101 gdo0_pin — used for direct bit-bang TX.
        # Plain int (not a pin schema): the pin is shared with the cc1101
        # component, which already registers it.
        cv.Optional(CONF_GDO0_PIN, default=4): cv.int_range(min=0, max=48),
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
    cg.add(var.set_gdo0_pin(config[CONF_GDO0_PIN]))

    for group in config.get(CONF_GROUPS, []):
        motor_ids = cg.RawExpression(
            "std::vector<std::string>{"
            + ",".join(f'"{m}"' for m in group[CONF_MOTOR_IDS])
            + "}"
        )
        cg.add(var.add_group(group[CONF_NAME], motor_ids))
