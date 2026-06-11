# ct485_sniffer — SPIKE evidence for issue #38, not product code.
# RX-only ESPHome external component wrapping lib/Ct485Frame + lib/Ct485Parser.
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, text_sensor, uart
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_TOTAL_INCREASING,
)

CODEOWNERS = ["@SlyWombat"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["sensor", "text_sensor"]

ct485_sniffer_ns = cg.esphome_ns.namespace("ct485_sniffer")
Ct485Sniffer = ct485_sniffer_ns.class_("Ct485Sniffer", cg.Component, uart.UARTDevice)

CONF_FRAME_COUNT = "frame_count"
CONF_LAST_DECODE = "last_decode"

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(Ct485Sniffer),
            cv.Optional(CONF_FRAME_COUNT): sensor.sensor_schema(
                state_class=STATE_CLASS_TOTAL_INCREASING,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_LAST_DECODE): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(uart.UART_DEVICE_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)
    if CONF_FRAME_COUNT in config:
        sens = await sensor.new_sensor(config[CONF_FRAME_COUNT])
        cg.add(var.set_frame_count_sensor(sens))
    if CONF_LAST_DECODE in config:
        ts = await text_sensor.new_text_sensor(config[CONF_LAST_DECODE])
        cg.add(var.set_last_decode_sensor(ts))
