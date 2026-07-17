// Matter feasibility spike — epic #179 phase 1.
//
// A bare Matter-over-WiFi thermostat on the ESP32-S3, ENTIRELY SEPARATE from the
// live SlyTherm firmware (its own env:s3_matter_probe, huge_app partition, no
// OTA). Goal: prove the pinned pioarduino platform's precompiled Matter libs
// build + link + commission into Apple Home / Google / Alexa. There is NO
// SlyTherm control integration here — that's phase 3 of the epic; the onChange
// hooks below are where it would route into ModeStateMachine / DemandArbiter.
//
// Flash this to a SPARE S3 only (never the live wall unit — it has no OTA slot
// and no SlyTherm control). Watch serial at 115200 for the pairing code.
#include <Arduino.h>
#include <Matter.h>
#include <WiFi.h>

#include "thermostat_secrets.h"  // THERMOSTAT_WIFI_SSID / THERMOSTAT_WIFI_PASS

MatterThermostat thermostat;

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n[matter-probe] boot");

  WiFi.begin(THERMOSTAT_WIFI_SSID, THERMOSTAT_WIFI_PASS);
  Serial.print("[matter-probe] wifi");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; ++i) { delay(300); Serial.print("."); }
  Serial.printf(" %s\r\n", WiFi.status() == WL_CONNECTED
                               ? WiFi.localIP().toString().c_str() : "FAILED");

  // A cooling+heating thermostat with an Auto mode — maps onto SlyTherm's
  // heat/cool/auto ModeStateMachine.
  thermostat.begin(MatterThermostat::THERMOSTAT_SEQ_OP_COOLING_HEATING,
                   MatterThermostat::THERMOSTAT_AUTO_MODE_ENABLED);
  thermostat.setCoolingHeatingSetpoints(20.0, 23.0);
  thermostat.setLocalTemperature(21.5);

  // Ecosystem -> device: where the full integration would push a mode/setpoint
  // change into the SlyTherm control pipeline (via the intent path, safety
  // pipeline still authoritative). For the spike, just log it.
  thermostat.onChangeMode([](MatterThermostat::ThermostatMode_t m) {
    Serial.printf("[matter-probe] mode -> %s\r\n",
                  MatterThermostat::getThermostatModeString(m));
    return true;
  });

  Matter.begin();
  if (!Matter.isDeviceCommissioned()) {
    Serial.println("[matter-probe] NOT commissioned — pair with:");
    Serial.printf("  manual code: %s\r\n", Matter.getManualPairingCode().c_str());
    Serial.printf("  QR url:      %s\r\n", Matter.getOnboardingQRCodeUrl().c_str());
  } else {
    Serial.println("[matter-probe] already commissioned.");
  }
}

void loop() {
  static bool was = false;
  const bool now = Matter.isDeviceCommissioned();
  if (now != was) {
    was = now;
    Serial.printf("[matter-probe] commissioned=%d\r\n", (int)now);
  }
  delay(500);
}
