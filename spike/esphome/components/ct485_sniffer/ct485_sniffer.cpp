// ct485_sniffer.cpp — SPIKE evidence for issue #38, not product code.

#include "ct485_sniffer.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace ct485_sniffer {

static const char *const TAG = "ct485_sniffer";

// HA truncates text-sensor states at 255 chars; keep one compact line.
static constexpr size_t kMaxDecodeLen = 250;

void Ct485Sniffer::setup() { last_byte_us_ = micros(); }

void Ct485Sniffer::dump_config() {
  ESP_LOGCONFIG(TAG, "CT-485 sniffer (RX-only, lib/Ct485Frame + lib/Ct485Parser)");
  LOG_SENSOR("  ", "Frame count", this->frame_count_sensor_);
  LOG_TEXT_SENSOR("  ", "Last decode", this->last_decode_sensor_);
}

void Ct485Sniffer::loop() {
  const uint32_t now_ms = millis();
  while (this->available() > 0) {
    uint8_t byte;
    if (!this->read_byte(&byte)) break;
    const uint32_t now_us = micros();
    const bool gap_before =
        !rx_in_progress_ || (now_us - last_byte_us_) >= ct485::kInterFrameGapUs;
    last_byte_us_ = now_us;
    rx_in_progress_ = true;
    if (acc_.feed(byte, gap_before)) this->on_valid_frame_();
  }
  if (rx_in_progress_ &&
      static_cast<uint32_t>(micros() - last_byte_us_) >= ct485::kInterFrameGapUs) {
    rx_in_progress_ = false;
    if (acc_.flush()) this->on_valid_frame_();
  }
  this->publish_throttled_(now_ms);
}

void Ct485Sniffer::on_valid_frame_() {
  ct485::Frame f;
  if (!ct485::decode(acc_.frame(), acc_.frameLen(), f)) return;
  frames_ok_++;

  // One-line summary: header essentials + msgType name from the parser.
  char head[96];
  snprintf(head, sizeof(head), "#%u %s src=%02X dst=%02X len=%u",
           static_cast<unsigned>(frames_ok_), ct485::msgTypeName(f.msgType).c_str(),
           f.src, f.dst, f.payloadLen);
  last_decode_ = head;
  if (f.baseMsgType() == 0x03) {
    const auto d = ct485::decodeSetControl(f);
    last_decode_ += " cmd=" + d.command;
    if (d.varA.valid)
      last_decode_ += " A:" + std::to_string(static_cast<int>(d.varA.demandPct)) + "%";
    if (d.varB.valid)
      last_decode_ += " B:" + std::to_string(static_cast<int>(d.varB.demandPct)) + "%";
  }
  if (last_decode_.size() > kMaxDecodeLen) last_decode_.resize(kMaxDecodeLen);
  ESP_LOGD(TAG, "%s", last_decode_.c_str());
  dirty_ = true;
}

void Ct485Sniffer::publish_throttled_(uint32_t now_ms) {
  if (!dirty_ || now_ms - last_publish_ms_ < 1000) return;
  last_publish_ms_ = now_ms;
  dirty_ = false;
  if (frame_count_sensor_ != nullptr)
    frame_count_sensor_->publish_state(static_cast<float>(frames_ok_));
  if (last_decode_sensor_ != nullptr) last_decode_sensor_->publish_state(last_decode_);
}

}  // namespace ct485_sniffer
}  // namespace esphome
