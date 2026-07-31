import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor, binary_sensor, sensor
from esphome.const import CONF_ID

# Namespace and Component
beko_ns = cg.esphome_ns.namespace("beko_dishwasher")
BekoDishwasher = beko_ns.class_("BekoDishwasher", cg.Component)

# Configuration keys
CONF_RAW_MOSI = "raw_mosi"
CONF_PROGRAM = "program"
CONF_REMAINING_TIME = "remaining_time"
CONF_REMAINING_MINUTES = "remaining_time_minutes"
CONF_STATUS = "status"
CONF_LED_STATE = "led_state"
CONF_POWER_ON = "power_on"
CONF_RUNNING = "running"
CONF_HALF_LOAD = "half_load"
CONF_EXTRA_RINSE = "extra_rinse"

AUTO_LOAD = ["text_sensor", "binary_sensor", "sensor"]

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(BekoDishwasher),
        cv.Required("clk_pin"): cv.int_range(min=0, max=39),
        cv.Required("mosi_pin"): cv.int_range(min=0, max=39),

        # Text Sensors
        cv.Optional(CONF_RAW_MOSI): text_sensor.text_sensor_schema(
            entity_category="diagnostic",
            icon="mdi:code-braces",
        ),
        cv.Optional(CONF_PROGRAM): text_sensor.text_sensor_schema(
            icon="mdi:dishwasher",
        ),
        cv.Optional(CONF_REMAINING_TIME): text_sensor.text_sensor_schema(
            icon="mdi:clock-outline",
        ),
        cv.Optional(CONF_STATUS): text_sensor.text_sensor_schema(
            icon="mdi:information-outline",
        ),
        cv.Optional(CONF_LED_STATE): text_sensor.text_sensor_schema(
            icon="mdi:led-on",
        ),

        # Numeric sensor, for graphing/automations in HA (e.g. "notify when < 10 min left")
        cv.Optional(CONF_REMAINING_MINUTES): sensor.sensor_schema(
            unit_of_measurement="min",
            icon="mdi:timer-outline",
            accuracy_decimals=0,
            device_class="duration",
            state_class="measurement",
        ),

        # Binary Sensors
        cv.Optional(CONF_POWER_ON): binary_sensor.binary_sensor_schema(
            device_class="power",
        ),
        cv.Optional(CONF_RUNNING): binary_sensor.binary_sensor_schema(
            device_class="running",
        ),
        cv.Optional(CONF_HALF_LOAD): binary_sensor.binary_sensor_schema(
            icon="mdi:cup-water",
        ),
        cv.Optional(CONF_EXTRA_RINSE): binary_sensor.binary_sensor_schema(
            icon="mdi:water-sync",
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_clk_pin(config["clk_pin"]))
    cg.add(var.set_mosi_pin(config["mosi_pin"]))

    # Text Sensors
    if CONF_RAW_MOSI in config:
        sens = await text_sensor.new_text_sensor(config[CONF_RAW_MOSI])
        cg.add(var.set_raw_mosi_sensor(sens))

    if CONF_PROGRAM in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PROGRAM])
        cg.add(var.set_program_sensor(sens))

    if CONF_REMAINING_TIME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_REMAINING_TIME])
        cg.add(var.set_remaining_time_sensor(sens))

    if CONF_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_STATUS])
        cg.add(var.set_status_sensor(sens))

    if CONF_LED_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LED_STATE])
        cg.add(var.set_led_state_sensor(sens))

    # Numeric Sensor
    if CONF_REMAINING_MINUTES in config:
        sens = await sensor.new_sensor(config[CONF_REMAINING_MINUTES])
        cg.add(var.set_remaining_minutes_sensor(sens))

    # Binary Sensors
    if CONF_POWER_ON in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_POWER_ON])
        cg.add(var.set_power_on_sensor(sens))

    if CONF_RUNNING in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_RUNNING])
        cg.add(var.set_running_sensor(sens))

    if CONF_HALF_LOAD in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_HALF_LOAD])
        cg.add(var.set_half_load_sensor(sens))

    if CONF_EXTRA_RINSE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_EXTRA_RINSE])
        cg.add(var.set_extra_rinse_sensor(sens))

    return var
