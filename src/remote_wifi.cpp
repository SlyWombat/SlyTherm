#include "remote_wifi.h"

#include <Arduino.h>
#include <WiFi.h>

#include <cstring>

#include "remote_secrets.h"

namespace remote_wifi {
namespace {

uint32_t gLastTryMs = 0;
constexpr uint32_t kRetryMs = 10000;
uint8_t gBestBssid[6] = {};
int32_t gBestChannel = 0;
bool gHaveBest = false;

void connectToBest() {
  if (gHaveBest) {
    Serial.println("[wifi] connecting to strongest \"" REMOTE_WIFI_SSID "\" BSSID...");
    WiFi.begin(REMOTE_WIFI_SSID, REMOTE_WIFI_PASS, gBestChannel, gBestBssid);
  } else {
    Serial.printf("[wifi] connecting to \"%s\" (no scan match, letting driver pick)...\n",
                  REMOTE_WIFI_SSID);
    WiFi.begin(REMOTE_WIFI_SSID, REMOTE_WIFI_PASS);
  }
}

}  // namespace

void begin() {
  WiFi.mode(WIFI_STA);

  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d network(s)\n", n);
  int32_t bestRssi = -999;
  for (int i = 0; i < n; i++) {
    Serial.printf("[wifi]   \"%s\" ch=%d rssi=%d enc=%d\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), static_cast<int>(WiFi.encryptionType(i)));
    if (WiFi.SSID(i) == REMOTE_WIFI_SSID && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      gBestChannel = WiFi.channel(i);
      memcpy(gBestBssid, WiFi.BSSID(i), 6);
      gHaveBest = true;
    }
  }
  if (gHaveBest) {
    Serial.printf("[wifi] strongest \"%s\" match: ch=%d rssi=%d\n", REMOTE_WIFI_SSID, gBestChannel,
                  bestRssi);
  }

  connectToBest();
  gLastTryMs = millis();
}

void loop() {
  const uint32_t nowMs = millis();
  if (WiFi.status() != WL_CONNECTED && nowMs - gLastTryMs >= kRetryMs) {
    gLastTryMs = nowMs;
    Serial.println("[wifi] retrying...");
    WiFi.disconnect();
    connectToBest();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

}  // namespace remote_wifi
