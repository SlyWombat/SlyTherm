#include "remote_wifi.h"

#include <Arduino.h>
#include <WiFi.h>

#include <Preferences.h>

#include <cstring>

// Credentials: NVS first (namespace "wifi", the SAME one the Controller's
// wifi_prov uses — the future on-device provisioning screen, #100/#101,
// writes there too), falling back to the gitignored compile-time secret,
// which is then SEEDED into NVS so an OTA'd image built WITHOUT secrets
// (CI release assets, #111/#59) inherits the network. Without this, the
// first OTA hop onto a release image would strand the Remote off WiFi.
#if __has_include("remote_secrets.h")
#include "remote_secrets.h"
#else
#define REMOTE_WIFI_SSID ""
#define REMOTE_WIFI_PASS ""
#endif

namespace remote_wifi {
namespace {

uint32_t gLastTryMs = 0;
char gSsid[48] = {};
char gPass[64] = {};
// #109: retry backoff — 10s after the first failure, *1.5 per retry, 60s
// ceiling, reset on success. Never a 5s hammer, retries forever.
constexpr uint32_t kRetryMinMs = 10000;
constexpr uint32_t kRetryMaxMs = 60000;
uint32_t gRetryMs = kRetryMinMs;
uint32_t gAttempts = 0;
uint8_t gBestBssid[6] = {};
int32_t gBestChannel = 0;
bool gHaveBest = false;
uint8_t gFailedTries = 0;
// Rescan after this many consecutive failed retries: the boot-time strongest
// BSSID can degrade (mesh nodes come and go at the edge of 2.4GHz range) and
// staying pinned to a dead node would strand the link forever.
constexpr uint8_t kRescanAfterTries = 3;

void scanForBest() {
  Serial.println("[wifi] scanning...");
  int n = WiFi.scanNetworks();
  Serial.printf("[wifi] scan found %d network(s)\n", n);
  gHaveBest = false;
  int32_t bestRssi = -999;
  for (int i = 0; i < n; i++) {
    Serial.printf("[wifi]   \"%s\" ch=%d rssi=%d enc=%d\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), static_cast<int>(WiFi.encryptionType(i)));
    if (WiFi.SSID(i) == gSsid && WiFi.RSSI(i) > bestRssi) {
      bestRssi = WiFi.RSSI(i);
      gBestChannel = WiFi.channel(i);
      memcpy(gBestBssid, WiFi.BSSID(i), 6);
      gHaveBest = true;
    }
  }
  if (gHaveBest) {
    Serial.printf("[wifi] strongest \"%s\" match: ch=%d rssi=%d\n", gSsid, gBestChannel,
                  bestRssi);
  }
}

void connectToBest() {
  ++gAttempts;
  if (gHaveBest) {
    Serial.printf("[wifi] connecting to strongest \"%s\" BSSID...\n", gSsid);
    WiFi.begin(gSsid, gPass, gBestChannel, gBestBssid);
  } else {
    Serial.printf("[wifi] connecting to \"%s\" (no scan match, letting driver pick)...\n",
                  gSsid);
    WiFi.begin(gSsid, gPass);
  }
}

}  // namespace

void begin() {
  // NVS creds win; compile-time secret is the seed (and is persisted so
  // the next image needs no secret). Neither present -> log and idle;
  // retries are pointless with no SSID.
  Preferences p;
  p.begin("wifi", false);
  String s = p.getString("ssid", ""), pw = p.getString("pass", "");
  if (s.length() == 0 && strlen(REMOTE_WIFI_SSID) > 0) {
    s = REMOTE_WIFI_SSID;
    pw = REMOTE_WIFI_PASS;
    p.putString("ssid", s);
    p.putString("pass", pw);
    Serial.println("[wifi] seeded NVS creds from compiled-in secret");
  }
  p.end();
  strlcpy(gSsid, s.c_str(), sizeof(gSsid));
  strlcpy(gPass, pw.c_str(), sizeof(gPass));
  if (gSsid[0] == 0) {
    Serial.println("[wifi] NO credentials (NVS empty, no remote_secrets.h) — WiFi idle");
    return;
  }
  WiFi.mode(WIFI_STA);
  scanForBest();
  connectToBest();
  gLastTryMs = millis();
}

void loop() {
  if (gSsid[0] == 0) return;  // unprovisioned: nothing to retry
  const uint32_t nowMs = millis();
  if (WiFi.status() == WL_CONNECTED) {
    gFailedTries = 0;
    gRetryMs = kRetryMinMs;
    return;
  }
  if (nowMs - gLastTryMs >= gRetryMs) {
    gLastTryMs = nowMs;
    gRetryMs = gRetryMs + gRetryMs / 2;
    if (gRetryMs > kRetryMaxMs) gRetryMs = kRetryMaxMs;
    Serial.println("[wifi] retrying...");
    WiFi.disconnect();
    if (++gFailedTries >= kRescanAfterTries) {
      gFailedTries = 0;
      scanForBest();  // re-pin: the boot-time best node may have degraded
    }
    connectToBest();
  }
}

bool connected() { return WiFi.status() == WL_CONNECTED; }
uint32_t attempts() { return gAttempts; }

}  // namespace remote_wifi
