"""light: platform matrix_web — стрічка WS2812 з підключенням, що задається з веб-панелі.

Дві ролі:
  role: matrix — матриця (num_leds пікселів), на неї малює display;
  role: status — вбудований світлодіод плати (1 піксель, індикатор Wi-Fi).

Типові значення (default_*) діють, доки користувач не змінив підключення у веб-панелі
(«Екран» → «Підключення матриці»); далі — збережене у флеші.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import light
from esphome.const import CONF_NUM_LEDS, CONF_OUTPUT_ID

from . import matrix_web_ns

DEPENDENCIES = ["esp32"]

StripLight = matrix_web_ns.class_("StripLight", light.AddressableLight)
StripHub = matrix_web_ns.class_("StripHub")
StripMode = matrix_web_ns.enum("StripMode", is_class=True)

CONF_ROLE = "role"
CONF_DEFAULT_MODE = "default_mode"
CONF_DEFAULT_PIN = "default_pin"
CONF_STATUS_PIN = "status_pin"
CONF_SERPENTINE = "serpentine"
CONF_RESERVED_PINS = "reserved_pins"

MODES = {"chain": StripMode.CHAIN, "direct": StripMode.DIRECT}

CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(StripLight),
        cv.Required(CONF_ROLE): cv.one_of("matrix", "status", lower=True),
        cv.Optional(CONF_DEFAULT_MODE, default="chain"): cv.one_of(*MODES, lower=True),
        cv.Optional(CONF_DEFAULT_PIN, default=5): cv.int_range(0, 48),
        cv.Optional(CONF_STATUS_PIN, default=48): cv.int_range(0, 48),
        cv.Optional(CONF_NUM_LEDS, default=512): cv.int_range(1, 1024),
        cv.Optional(CONF_SERPENTINE, default=False): cv.boolean,
        cv.Optional(CONF_RESERVED_PINS, default=[]): cv.ensure_list(cv.int_range(0, 48)),
    }
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_OUTPUT_ID])
    await light.register_light(var, config)
    await cg.register_component(var, config)
    role = 0 if config[CONF_ROLE] == "matrix" else 1
    cg.add(var.set_role(role))
    if role == 0:
        hub = cg.MockObj("matrix_web::StripHub::get()", "->")
        cg.add(
            hub.set_defaults(
                MODES[config[CONF_DEFAULT_MODE]],
                config[CONF_DEFAULT_PIN],
                config[CONF_STATUS_PIN],
                config[CONF_NUM_LEDS],
                config[CONF_SERPENTINE],
            )
        )
        for p in config[CONF_RESERVED_PINS]:
            cg.add(hub.add_reserved_pin(p))
