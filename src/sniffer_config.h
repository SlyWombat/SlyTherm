// sniffer_config.h — build-time configuration for the Phase 1/2 RX-only
// sniffer (src/main_sniffer.cpp). Bench rig: ESP32-DevKitC, see docs/05
// Phase 1 and docs/02 §8. Protocol timing constants live in
// lib/Ct485Core/Ct485Core.h — only sniffer-rig knobs belong here.
//
// Wi-Fi/MQTT credentials come from src/sniffer_secrets.h (gitignored; copy
// src/sniffer_secrets.h.example) or -D build flags — never from this file.

#pragma once
#include <cstddef>
#include <cstdint>

#if __has_include("sniffer_secrets.h")
#include "sniffer_secrets.h"
#endif
// Absent/partial secrets -> empty defaults: builds everywhere; with no SSID
// the console falls back to its own SoftAP (kApSsid below).
#ifndef SNIFFER_WIFI_SSID
#define SNIFFER_WIFI_SSID ""
#endif
#ifndef SNIFFER_WIFI_PASS
#define SNIFFER_WIFI_PASS ""
#endif

namespace sniffer {

// UART pins for the RS-485 transceiver's RO ("receive out") TTL signal. TX is
// ALWAYS -1 (never attached) on every board — part of the RX-only guarantee
// enforced by the static_assert in main_sniffer.cpp. Do NOT set a real TX pin
// in this firmware; TX belongs to Phase 3 only.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
// Waveshare ESP32-S3-Touch-LCD-4.3B: the 800x480 RGB panel, octal PSRAM, I2C
// touch and USB/UART0 consume almost every GPIO. kRxPin MUST be a pin that is
// FREE and broken out on the board's header. Pins not taken by the panel /
// PSRAM / I2C(8,9) / USB(19,20) / UART0(43,44) are roughly {6, 15, 16} (minus
// whatever the "B" variant's CAN transceiver claims).
//   >>> HARDWARE-VERIFY: confirm 16 is free & exposed on YOUR board's
//   >>> silkscreen before wiring; change it here if not. Wire the RS-485
//   >>> transceiver's RO/RXD output to this pin.
constexpr int kRxPin = 16;
#else
// ESP32-DevKitC bench rig (docs/05 Phase 1): UART2 RX on GPIO16.
constexpr int kRxPin = 16;
#endif
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

// ---- Web console (issue #57) ----
constexpr uint16_t kHttpPort = 80;
constexpr uint16_t kWsPort   = 81;
constexpr uint32_t kStatusPeriodMs = 1000;  // WS status/counters broadcast

// STA connect grace before falling back to SoftAP. The AP password is a
// fixed bench convenience, NOT a secret (the console is read-only and the
// rig only exists on the bench); change it here if your bench is hostile.
constexpr uint32_t kWifiStaTimeoutMs = 15000;
constexpr const char* kApSsid = "dettson-sniffer";
constexpr const char* kApPass = "ct485sniff";

}  // namespace sniffer

#ifdef SNIFFER_MQTT
// Optional MQTT frame push. Compile the client in with -DSNIFFER_MQTT
// (default OFF: no MQTT dependency in the default build); even when compiled
// in, pushing starts DISABLED and is toggled at runtime from the web console.
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
