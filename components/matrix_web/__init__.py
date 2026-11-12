"""matrix_web — власна веб-панель для Wall Matrix.

Віддає web/index.html (стиснутий gzip і вшитий у прошивку) та невеликий REST API:
  GET  /api/state      — стан усіх налаштувань (JSON)
  GET  /api/live       — поточний кадр матриці (hex RGB, порядково)
  POST /api/set        — type=switch|number|text|select|light, id=<yaml id>, v=<значення>
  POST /api/press      — id=<yaml id кнопки>
  /api/v1/...          — API у стилі AWTRIX NG: apps, notifications, display, icons
                         (див. api_v1.cpp і README)
"""

import gzip
from pathlib import Path

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    button,
    light,
    number,
    select,
    sensor,
    switch,
    text,
    text_sensor,
    web_server_base,
    wifi,
)
from esphome.const import CONF_ID, CONF_PASSWORD, CONF_USERNAME
from esphome.core import CORE, HexInt

DEPENDENCIES = ["network", "light"]
AUTO_LOAD = ["web_server_base", "json"]

CONF_HTML = "html"
CONF_RAW_HTML_ID = "raw_html_id"
CONF_LIGHT = "light"
CONF_SERPENTINE = "serpentine"
CONF_PIXEL_OFFSET = "pixel_offset"
CONF_WIDTH = "width"
CONF_HEIGHT = "height"

ENTITY_LISTS = {
    "switches": ("add_switch", switch.Switch),
    "numbers": ("add_number", number.Number),
    "texts": ("add_text", text.Text),
    "selects": ("add_select", select.Select),
    "buttons": ("add_button", button.Button),
    "sensors": ("add_sensor", sensor.Sensor),
    "text_sensors": ("add_text_sensor", text_sensor.TextSensor),
}

matrix_web_ns = cg.esphome_ns.namespace("matrix_web")
MatrixWeb = matrix_web_ns.class_("MatrixWeb", cg.Component)


def _schema():
    schema = {
        cv.GenerateID(): cv.declare_id(MatrixWeb),
        cv.GenerateID(web_server_base.CONF_WEB_SERVER_BASE_ID): cv.use_id(
            web_server_base.WebServerBase
        ),
        cv.GenerateID(CONF_RAW_HTML_ID): cv.declare_id(cg.uint8),
        cv.Required(CONF_HTML): cv.file_,
        cv.Inclusive(CONF_USERNAME, "auth"): cv.string_strict,
        cv.Inclusive(CONF_PASSWORD, "auth"): cv.string_strict,
        cv.Required(CONF_LIGHT): cv.use_id(light.LightState),
        cv.Optional(CONF_WIDTH, default=32): cv.int_range(1, 256),
        cv.Optional(CONF_HEIGHT, default=16): cv.int_range(1, 256),
        cv.Optional(CONF_SERPENTINE, default=False): cv.boolean,
        cv.Optional(CONF_PIXEL_OFFSET, default=0): cv.int_range(0, 4096),
    }
    for key, (_, cls) in ENTITY_LISTS.items():
        schema[cv.Optional(key, default=[])] = cv.ensure_list(cv.use_id(cls))
    return cv.Schema(schema).extend(cv.COMPONENT_SCHEMA)


CONFIG_SCHEMA = _schema()


async def to_code(config):
    base = await cg.get_variable(config[web_server_base.CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)

    html_path = Path(CORE.relative_config_path(str(config[CONF_HTML])))
    data = gzip.compress(html_path.read_bytes(), compresslevel=9, mtime=0)
    arr = cg.progmem_array(config[CONF_RAW_HTML_ID], [HexInt(b) for b in data])
    cg.add(var.set_index(arr, len(data)))

    if CONF_USERNAME in config:
        cg.add_define("USE_WEBSERVER_AUTH")
        cg.add_define("USE_WEBSERVER_AUTH_DIGEST")
        cg.add(base.set_auth_username(config[CONF_USERNAME]))
        cg.add(base.set_auth_password(config[CONF_PASSWORD]))
        cg.add(var.set_auth(config[CONF_USERNAME], config[CONF_PASSWORD]))

    # Налаштування Wi-Fi у веб-панелі: список мереж читаємо з потоку httpd
    wifi.request_wifi_scan_results()
    wifi.request_wifi_scan_results_lock()

    cg.add(var.set_light(await cg.get_variable(config[CONF_LIGHT])))
    cg.add(var.set_geometry(config[CONF_WIDTH], config[CONF_HEIGHT]))
    cg.add(var.set_serpentine(config[CONF_SERPENTINE]))
    cg.add(var.set_pixel_offset(config[CONF_PIXEL_OFFSET]))

    for key, (method, _) in ENTITY_LISTS.items():
        for ent_id in config[key]:
            ent = await cg.get_variable(ent_id)
            cg.add(getattr(var, method)(str(ent_id), ent))
