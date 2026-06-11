// ct485_sniffer.h — SPIKE evidence for issue #38, not product code.
//
// RX-only CT-485 frame logger as an ESPHome external component. Wraps the
// repo's pure-C++ lib/Ct485Frame (gap-delimited reassembly + Fletcher) and
// lib/Ct485Parser (labeled decode); those libs are pulled in unmodified via
// lib_extra_dirs in the spike YAML's platformio_options.
//
// Known timing limitation (deliberate — it is part of the evidence): the
// 3.5 ms inter-frame gap is inferred from esphome::micros() at loop()
// granularity (~1-16 ms, cooperative scheduler), not from the UART RX-timeout
// interrupt. Back-to-back frames drained in one loop() pass would merge.
// Good enough for sniffing (CT-485 packets are >=100 ms apart); NOT good
// enough for the production TX path.

#pragma once

#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

#include "Ct485Frame.h"
#include "Ct485Parser.h"

namespace esphome {
namespace ct485_sniffer {

class Ct485Sniffer : public Component, public uart::UARTDevice {
 public:
  void set_frame_count_sensor(sensor::Sensor *s) { frame_count_sensor_ = s; }
  void set_last_decode_sensor(text_sensor::TextSensor *s) { last_decode_sensor_ = s; }

  void setup() override;
  void loop() override;
  void dump_config() override;

 protected:
  void on_valid_frame_();
  void publish_throttled_(uint32_t now_ms);

  ct485::FrameAccumulator acc_;
  uint32_t last_byte_us_{0};
  bool rx_in_progress_{false};

  uint32_t frames_ok_{0};
  std::string last_decode_{};
  bool dirty_{false};
  uint32_t last_publish_ms_{0};

  sensor::Sensor *frame_count_sensor_{nullptr};
  text_sensor::TextSensor *last_decode_sensor_{nullptr};
};

}  // namespace ct485_sniffer
}  // namespace esphome
