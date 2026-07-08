// SlyTherm Remote entrypoint (issue #94 epic, #95 P0 toolchain gate).
// Wall-mounted ESP32-P4 node: NO CT-485/RS-485, NO furnace demand pipeline —
// see docs/11-remote-node-plan.md. #97 adds panel/touch bring-up
// (remote_board_p4.cpp); #96 adds hosted-WiFi bring-up (remote_wifi.cpp);
// #98 adds MQTT bring-up (remote_mqtt.cpp); the real UI lands in #100-101 on
// top of this entrypoint.

#include <Arduino.h>
#include <WiFi.h>

#include "remote_board_p4.h"
#include "remote_mqtt.h"
#include "remote_wifi.h"

void setup() {
  Serial.begin(115200);
  // Give the native USB-CDC link a moment to enumerate before the first print.
  delay(1500);
  Serial.println();
  Serial.println("=== SlyTherm Remote (ESP32-P4) boot ===");
  Serial.printf("Build: %s %s\n", __DATE__, __TIME__);
  Serial.printf("Chip: %s rev v%d.%d, %d MB flash, PSRAM %s\n",
                ESP.getChipModel(), ESP.getChipRevision() / 100,
                ESP.getChipRevision() % 100, ESP.getFlashChipSize() / (1024 * 1024),
                psramFound() ? "OK" : "NOT FOUND");

  remote_board::begin();
  remote_wifi::begin();
  remote_mqtt::begin();
}

void loop() {
  remote_board::loop();
  remote_wifi::loop();
  remote_mqtt::loop();

  static uint32_t lastMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastMs >= 5000) {
    lastMs = nowMs;
    Serial.printf("alive t=%lus heap=%u panel=%s touch=%s wifi=%s ip=%s mqtt=%s\n", nowMs / 1000,
                  static_cast<unsigned>(ESP.getFreeHeap()),
                  remote_board::panelOk() ? "OK" : "FAIL",
                  remote_board::touchOk() ? "OK" : "FAIL",
                  remote_wifi::connected() ? "OK" : "FAIL",
                  remote_wifi::connected() ? WiFi.localIP().toString().c_str() : "-",
                  remote_mqtt::connected() ? "OK" : "FAIL");
  }
}
