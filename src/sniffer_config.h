// sniffer_config.h — build-time configuration for the Phase 1 RX-only sniffer
// (src/main_sniffer.cpp). Bench rig: ESP32-DevKitC, see docs/05 Phase 1 and
// docs/02 §8. Protocol timing constants live in lib/Ct485Core/Ct485Core.h —
// only sniffer-rig knobs belong here.

#pragma once
#include <cstddef>
#include <cstdint>

namespace sniffer {

// UART2 pins. TX is -1 (never attached): part of the RX-only guarantee in
// main_sniffer.cpp — do not change to a real pin in this firmware.
constexpr int kRxPin = 16;
constexpr int kTxPin = -1;

constexpr uint32_t kUsbBaud = 115200;

// UART driver RX ring >= 1024 so a full burst (frames are <= 252 B, but the
// loop may stall ~10 ms on USB output) cannot drop bytes.
constexpr size_t kUartRxBufBytes = 2048;

// Large USB TX buffer so Serial.print() never blocks mid-pump and distorts
// the micros()-based gap measurement.
constexpr size_t kUsbTxBufBytes = 4096;

// Heartbeat with byte/frame/error counters — the near-silent-bus diagnostic
// (docs/05 Phase 1 caveat: a conventional install may carry no traffic).
constexpr uint32_t kHeartbeatMs = 10000;

// Auto-baud: dwell this long per candidate baud, alternating until one baud
// "clearly wins" — >= kAutobaudLockFrames Fletcher-valid frames AND at least
// kAutobaudLockRatio x the other baud's valid-frame count.
constexpr uint32_t kAutobaudDwellMs    = 5000;
constexpr uint32_t kAutobaudLockFrames = 3;
constexpr uint32_t kAutobaudLockRatio  = 4;

}  // namespace sniffer

#ifdef SNIFFER_MQTT
// Optional MQTT streaming (default OFF). Enable with -DSNIFFER_MQTT and pass
// credentials as build flags generated from .env — NEVER commit them:
//   -DSNIFFER_WIFI_SSID='"..."' -DSNIFFER_WIFI_PASS='"..."'
//   -DSNIFFER_MQTT_HOST='"..."' [-DSNIFFER_MQTT_PORT=1883]
//   [-DSNIFFER_MQTT_USER='"..."' -DSNIFFER_MQTT_PASS='"..."']
#ifndef SNIFFER_WIFI_SSID
#define SNIFFER_WIFI_SSID ""
#endif
#ifndef SNIFFER_WIFI_PASS
#define SNIFFER_WIFI_PASS ""
#endif
#ifndef SNIFFER_MQTT_HOST
#define SNIFFER_MQTT_HOST ""
#endif
#ifndef SNIFFER_MQTT_PORT
#define SNIFFER_MQTT_PORT 1883
#endif
#ifndef SNIFFER_MQTT_USER
#define SNIFFER_MQTT_USER ""
#endif
#ifndef SNIFFER_MQTT_PASS
#define SNIFFER_MQTT_PASS ""
#endif

namespace sniffer {
constexpr const char* kMqttClientId    = "dettson-sniffer";
constexpr const char* kMqttTopicFrames = "dettson/sniffer/frames";
constexpr uint32_t    kMqttReconnectMs = 5000;
constexpr uint16_t    kMqttBufBytes    = 1200;  // frame lines exceed PubSubClient's 256 default
}  // namespace sniffer
#endif  // SNIFFER_MQTT
