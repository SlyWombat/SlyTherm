// One-time repair sketch for issue #96: this board's factory-programmed
// ESP32-C6 co-processor firmware (esp_hosted slave, v2.3.2) is far older
// than what arduino-esp32 3.x's host driver expects (v2.12.8), causing WiFi
// association to fail with AUTH_EXPIRE regardless of network/signal/AP
// (known issue: espressif/esp-hosted-mcu#2). Pushes the update firmware
// directly over the existing SDIO hosted link -- no WiFi connection needed,
// sidestepping the chicken-and-egg problem that blocks the normal
// HTTPS-based ESP_HostedOTA path. Run once per board; after it reports
// SUCCESS, reflash the real env:remote_p4 firmware.
//
// Regenerate the embedded blob first: tools/fetch_c6_hosted_fw.sh

#include <Arduino.h>
#include <WiFi.h>

#include "esp32-hal-hosted.h"
#include "esp32c6_fw_blob.h"

namespace {
constexpr uint32_t kChunk = 2048;
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println();
  Serial.println("=== C6 hosted-firmware repair helper ===");

  WiFi.STA.begin();  // brings up the hosted transport, no AP association needed
  delay(500);

  if (!hostedIsInitialized()) {
    Serial.println("FAIL: hosted transport did not initialize");
    return;
  }

  uint32_t hMaj, hMin, hPatch, sMaj, sMin, sPatch;
  hostedGetHostVersion(&hMaj, &hMin, &hPatch);
  hostedGetSlaveVersion(&sMaj, &sMin, &sPatch);
  Serial.printf("Host version:  %u.%u.%u\n", hMaj, hMin, hPatch);
  Serial.printf("Slave version (before): %u.%u.%u\n", sMaj, sMin, sPatch);
  Serial.printf("Blob size: %u bytes\n", kC6FwBlobLen);

  Serial.println("Beginning update...");
  if (!hostedBeginUpdate()) {
    Serial.println("FAIL: hostedBeginUpdate()");
    return;
  }

  uint32_t written = 0;
  while (written < kC6FwBlobLen) {
    uint32_t n = kC6FwBlobLen - written;
    if (n > kChunk) n = kChunk;
    if (!hostedWriteUpdate(const_cast<uint8_t*>(kC6FwBlob) + written, n)) {
      Serial.printf("FAIL: hostedWriteUpdate() at offset %u\n", written);
      return;
    }
    written += n;
    if (written % (kChunk * 50) == 0 || written == kC6FwBlobLen) {
      Serial.printf("  %u / %u bytes (%u%%)\n", written, kC6FwBlobLen,
                    written * 100 / kC6FwBlobLen);
    }
  }

  Serial.println("Finalizing...");
  if (!hostedEndUpdate()) {
    Serial.println("FAIL: hostedEndUpdate()");
    return;
  }

  Serial.println("Activating...");
  if (!hostedActivateUpdate()) {
    Serial.println("FAIL: hostedActivateUpdate()");
    return;
  }

  Serial.println("=====================================");
  Serial.println("SUCCESS: C6 hosted firmware updated");
  Serial.println("=====================================");
}

void loop() {
  static uint32_t lastMs = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastMs >= 3000) {
    lastMs = nowMs;
    Serial.println("repair helper idle -- reflash env:remote_p4 now");
  }
  delay(10);
}
