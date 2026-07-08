// ota_client.cpp — see ota_client.h. Entirely compiled out without
// -DSLYTHERM_OTA (the header provides inline no-ops).
//
// Design notes (#61 issue plan):
// - HTTPClient + Update.h rather than raw esp_https_ota: streaming lets the
//   sha256 be computed as bytes arrive, redirects to
//   objects.githubusercontent.com are followed, and the identical code
//   compiles on Arduino core 2.x (S3) and 3.x (P4).
// - Rollback is APP-LEVEL: stock Arduino cores don't enable bootloader
//   rollback, so bootValidate() + the boot-attempt counter + the self-test
//   confirm/rollback protocol below stand in for it.
// - The mqttTask owns all MQTT I/O; this module only exposes status() and
//   accepts request*() flags. The reboot itself is gated on
//   otaSafeToReboot() (#62) — provided by main_thermostat.cpp (furnace idle)
//   or main_remote.cpp (always true).

#ifdef SLYTHERM_OTA

#include "ota_client.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <esp_ota_ops.h>

#include <mbedtls/base64.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/version.h>

#include <cstring>
#include <ctime>
#include <string>

#include "OtaCatalog.h"
#include "ota_certs.h"

// Target identity + version (injected via platformio.ini / version_flag.py).
#ifndef SLYTHERM_OTA_TARGET
#error "SLYTHERM_OTA needs -DSLYTHERM_OTA_TARGET (e.g. \"wall-s3\")"
#endif
#ifndef SLYTHERM_OTA_HWREV
#error "SLYTHERM_OTA needs -DSLYTHERM_OTA_HWREV (e.g. \"s3-43b-r1.1\")"
#endif
#ifndef SLYTHERM_FW_VERSION
#define SLYTHERM_FW_VERSION "0.0.0-dev"
#endif
// Overridable for bench testing against a local HTTPS server.
#ifndef SLYTHERM_OTA_CATALOG_URL
#define SLYTHERM_OTA_CATALOG_URL \
  "https://raw.githubusercontent.com/SlyWombat/SlyTherm/main/firmware/catalog.json"
#endif

extern "C" bool otaSafeToReboot();

namespace ota {
namespace {

constexpr uint32_t kTaskStack = 16384;   // TLS handshake + parse headroom
constexpr UBaseType_t kTaskPrio = 1;     // below MQTT/control
constexpr uint32_t kCheckIntervalMs = 24u * 60u * 60u * 1000u;  // daily
constexpr uint32_t kCheckRetryMs = 5u * 60u * 1000u;  // retry cadence after a failure
constexpr uint32_t kSelfTestTimeoutMs = 5u * 60u * 1000u;       // 5 min
constexpr uint8_t kMaxBootAttempts = 3;
constexpr size_t kCatalogMaxBytes = 16 * 1024;
constexpr uint32_t kStreamTimeoutMs = 30 * 1000;  // stall watchdog per read

// NVS namespace "ota": pend = version being validated ("" = none),
// tries = boot attempts while pending, last = last outcome (for the UI).
constexpr const char* kNvsNs = "ota";

portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;
Status gStatus;
dettson::ota::CatalogEntry gAvailable;   // task-local once resolved
volatile bool gCheckReq = false;
volatile bool gApplyReq = false;
volatile bool gSelfTestPass = false;
bool gPendingAtBoot = false;             // set by bootValidate()
uint32_t gBootMs = 0;

void setStatus(State s, uint8_t pct, const char* avail, const char* err) {
  portENTER_CRITICAL(&gMux);
  gStatus.state = s;
  gStatus.progressPct = pct;
  if (avail) strlcpy(gStatus.available, avail, sizeof(gStatus.available));
  if (err) strlcpy(gStatus.error, err, sizeof(gStatus.error));
  portEXIT_CRITICAL(&gMux);
}

void fail(const char* err) {
  Serial.printf("[ota] FAIL: %s\n", err);
  setStatus(State::kFailed, 0, nullptr, err);
}

// ---- sha256 across mbedtls 2.x (core 2.x) / 3.x (core 3.x) ----
struct Sha256 {
  mbedtls_sha256_context c;
  Sha256() {
    mbedtls_sha256_init(&c);
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_starts(&c, 0);
#else
    mbedtls_sha256_starts_ret(&c, 0);
#endif
  }
  ~Sha256() { mbedtls_sha256_free(&c); }
  void update(const uint8_t* d, size_t n) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_update(&c, d, n);
#else
    mbedtls_sha256_update_ret(&c, d, n);
#endif
  }
  void finish(uint8_t out[32]) {
#if MBEDTLS_VERSION_MAJOR >= 3
    mbedtls_sha256_finish(&c, out);
#else
    mbedtls_sha256_finish_ret(&c, out);
#endif
  }
};

bool hexEqual(const uint8_t digest[32], const std::string& hex) {
  if (hex.size() != 64) return false;
  static const char* kHex = "0123456789abcdef";
  for (int i = 0; i < 32; ++i) {
    if (hex[2 * i] != kHex[digest[i] >> 4] ||
        hex[2 * i + 1] != kHex[digest[i] & 0xF])
      return false;
  }
  return true;
}

// ECDSA-P256 over the image sha256, DER signature, base64 in the catalog.
// An empty/invalid signature REJECTS — sha256 alone is integrity, not
// authenticity (#62; CI signs with the OTA_SIGNING_KEY_PEM secret).
bool signatureOk(const uint8_t digest[32], const std::string& sigB64) {
  if (sigB64.empty()) return false;
  uint8_t sig[96];  // DER ECDSA-P256 sig is ~70-72 bytes
  size_t sigLen = 0;
  if (mbedtls_base64_decode(sig, sizeof(sig), &sigLen,
                            reinterpret_cast<const uint8_t*>(sigB64.data()),
                            sigB64.size()) != 0)
    return false;
  mbedtls_pk_context pk;
  mbedtls_pk_init(&pk);
  const auto* key = reinterpret_cast<const uint8_t*>(ota_certs::kOtaSigningPubKey);
  const size_t keyLen = strlen(ota_certs::kOtaSigningPubKey) + 1;  // incl. nul
  bool ok = false;
  if (mbedtls_pk_parse_public_key(&pk, key, keyLen) == 0) {
    ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, digest, 32, sig, sigLen) == 0;
  }
  mbedtls_pk_free(&pk);
  return ok;
}

// TLS certificate validation needs a sane wall clock; before NTP syncs every
// handshake is doomed (notBefore in the future). Gate + say so instead of
// reporting an opaque connection error.
bool clockSynced() { return time(nullptr) > 1600000000; }

// Compose "phase: HTTP <code> (tls=<mbedtls detail>, heap=<free>)" so the
// REAL failure reason reaches the MQTT status without a serial cable.
void failHttp(const char* phase, int code, WiFiClientSecure& tls) {
  char tlsErr[48] = "";
  const int rc = tls.lastError(tlsErr, sizeof(tlsErr));
  char b[sizeof(Status{}.error)];
  snprintf(b, sizeof(b), "%s: HTTP %d (tls=%d %s heap=%u)", phase, code, rc,
           tlsErr, static_cast<unsigned>(ESP.getFreeHeap()));
  fail(b);
}

// ---- catalog check ----
bool doCheck() {
  setStatus(State::kChecking, 0, "", "");
  if (!clockSynced()) {
    fail("catalog: clock unsynced (NTP pending) — will retry");
    return false;
  }
  Serial.printf("[ota] check: heap=%u\n", static_cast<unsigned>(ESP.getFreeHeap()));
  WiFiClientSecure tls;
  tls.setCACert(ota_certs::kOtaCaBundle);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(tls, SLYTHERM_OTA_CATALOG_URL)) {
    fail("catalog: http.begin failed");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    failHttp("catalog", code, tls);
    return false;
  }
  const int len = http.getSize();
  if (len > 0 && static_cast<size_t>(len) > kCatalogMaxBytes) {
    http.end();
    fail("catalog: too large");
    return false;
  }
  std::string body = http.getString().c_str();
  http.end();

  std::vector<dettson::ota::CatalogEntry> entries;
  if (!dettson::ota::parseCatalogJson(body.c_str(), entries)) {
    fail("catalog: parse rejected");
    return false;
  }
  const auto r = dettson::ota::resolve(entries, SLYTHERM_OTA_TARGET,
                                       SLYTHERM_OTA_HWREV, SLYTHERM_FW_VERSION);
  if (!r.targetFound) {
    fail("catalog: no entry for this target/hwRev");
    return false;
  }
  if (r.blockedByMinVersion) {
    fail("catalog: blocked by minVersion (needs stepping-stone release)");
    return false;
  }
  if (!r.updateAvailable) {
    Serial.printf("[ota] up to date (%s)\n", SLYTHERM_FW_VERSION);
    setStatus(State::kUpToDate, 0, "", "");
    return true;
  }
  gAvailable = r.entry;
  Serial.printf("[ota] update available: %s -> %s\n", SLYTHERM_FW_VERSION,
                gAvailable.version.c_str());
  setStatus(State::kUpdateAvailable, 0, gAvailable.version.c_str(), "");
  return true;
}

// ---- download + verify + stage ----
bool doApply() {
  if (gAvailable.appUrl.empty()) {
    fail("apply: no update resolved (check first)");
    return false;
  }
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  if (next == nullptr) {
    fail("apply: no inactive OTA slot (#64)");
    return false;
  }
  if (gAvailable.appSize && gAvailable.appSize > next->size) {
    fail("apply: image exceeds OTA slot");
    return false;
  }

  setStatus(State::kDownloading, 0, gAvailable.version.c_str(), "");
  WiFiClientSecure tls;
  tls.setCACert(ota_certs::kOtaCaBundle);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(15000);
  http.setTimeout(15000);
  if (!http.begin(tls, gAvailable.appUrl.c_str())) {
    fail("apply: http.begin failed");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    failHttp("apply", code, tls);
    return false;
  }
  const int total = http.getSize();  // -1 = chunked/unknown
  if (gAvailable.appSize && total > 0 &&
      static_cast<uint32_t>(total) != gAvailable.appSize) {
    http.end();
    fail("apply: content-length != catalog appSize");
    return false;
  }
  const size_t expect = gAvailable.appSize ? gAvailable.appSize
                        : (total > 0 ? static_cast<size_t>(total)
                                     : UPDATE_SIZE_UNKNOWN);
  if (!Update.begin(expect, U_FLASH)) {
    http.end();
    fail("apply: Update.begin failed");
    return false;
  }

  Sha256 sha;
  WiFiClient* stream = http.getStreamPtr();
  static uint8_t buf[4096];
  size_t written = 0;
  uint32_t lastDataMs = millis();
  while (http.connected() && (total < 0 || written < static_cast<size_t>(total))) {
    const size_t avail = stream->available();
    if (avail == 0) {
      if (millis() - lastDataMs > kStreamTimeoutMs) {
        Update.abort();
        http.end();
        fail("apply: stream stalled");
        return false;
      }
      vTaskDelay(pdMS_TO_TICKS(20));
      // A finished chunked transfer closes the connection; loop re-checks.
      continue;
    }
    lastDataMs = millis();
    const int n = stream->readBytes(
        buf, avail > sizeof(buf) ? sizeof(buf) : avail);
    if (n <= 0) continue;
    sha.update(buf, static_cast<size_t>(n));
    if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      Update.abort();
      http.end();
      fail("apply: flash write failed");
      return false;
    }
    written += static_cast<size_t>(n);
    if (total > 0)
      setStatus(State::kDownloading,
                static_cast<uint8_t>((written * 100u) / total),
                gAvailable.version.c_str(), "");
  }
  http.end();
  if (total > 0 && written != static_cast<size_t>(total)) {
    Update.abort();
    fail("apply: truncated download");
    return false;
  }

  setStatus(State::kVerifying, 100, gAvailable.version.c_str(), "");
  uint8_t digest[32];
  sha.finish(digest);
  if (!hexEqual(digest, gAvailable.sha256)) {
    Update.abort();
    fail("verify: sha256 mismatch");
    return false;
  }
  if (!signatureOk(digest, gAvailable.sig)) {
    Update.abort();
    fail("verify: signature rejected");
    return false;
  }
  // Activates the new slot in otadata; the running image keeps control until
  // the (gated) reboot below. A power-cycle while staged boots the new image
  // early — acceptable: boot-to-no-demand holds and bootValidate() arms.
  if (!Update.end(true)) {
    fail("apply: Update.end failed");
    return false;
  }
  Preferences p;
  p.begin(kNvsNs, false);
  p.putString("pend", gAvailable.version.c_str());
  p.putUChar("tries", 0);
  p.end();
  Serial.printf("[ota] staged %s -> slot %s; waiting for safe reboot window\n",
                gAvailable.version.c_str(), next->label);
  setStatus(State::kStaged, 100, gAvailable.version.c_str(), "");
  return true;
}

void otaTask(void*) {
  uint32_t lastAutoCheckMs = 0;
  bool staged = false;
  for (;;) {
    // Pending-update self-test window (this boot IS the new image).
    if (gPendingAtBoot) {
      if (gSelfTestPass) {
        Preferences p;
        p.begin(kNvsNs, false);
        String v = p.getString("pend", "");
        p.putString("pend", "");
        p.putUChar("tries", 0);
        p.putString("last", (std::string("ok ") + v.c_str()).c_str());
        p.end();
        gPendingAtBoot = false;
        Serial.printf("[ota] self-test passed — %s confirmed\n", v.c_str());
      } else if (millis() - gBootMs > kSelfTestTimeoutMs) {
        Serial.println("[ota] self-test TIMEOUT — rolling back");
        const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
        Preferences p;
        p.begin(kNvsNs, false);
        p.putString("pend", "");
        p.putString("last", "rollback timeout");
        p.end();
        if (prev) esp_ota_set_boot_partition(prev);
        vTaskDelay(pdMS_TO_TICKS(200));
        ESP.restart();
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      // Daily cadence after a good check; 5-min retry after a failed one
      // (first boot attempt races WiFi/NTP — don't wait a day to recover).
      static bool lastCheckOk = false;
      const uint32_t interval = lastCheckOk ? kCheckIntervalMs : kCheckRetryMs;
      const bool auto_due = lastAutoCheckMs == 0 ||
                            millis() - lastAutoCheckMs >= interval;
      if (gCheckReq || (auto_due && !staged)) {
        gCheckReq = false;
        lastAutoCheckMs = millis();
        lastCheckOk = doCheck();
      }
      if (gApplyReq && !staged) {
        gApplyReq = false;
        if (status().state == State::kUpdateAvailable) staged = doApply();
        else if (status().state != State::kDownloading &&
                 status().state != State::kVerifying)
          fail("apply: nothing available (check first)");
      }
    }

    if (staged && otaSafeToReboot()) {
      Serial.println("[ota] safe window — rebooting into the new image");
      setStatus(State::kRebooting, 100, nullptr, "");
      vTaskDelay(pdMS_TO_TICKS(500));  // let the MQTT surface publish
      ESP.restart();
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

}  // namespace

bool bootValidate() {
  gBootMs = millis();
  Preferences p;
  p.begin(kNvsNs, false);
  String pend = p.getString("pend", "");
  if (pend.length() == 0) {
    p.end();
    return false;
  }
  uint8_t tries = p.getUChar("tries", 0);
  if (tries + 1 >= kMaxBootAttempts) {
    // Crash-looping new image (complements the #80 reset-loop latch): give
    // up and boot the previous slot.
    p.putString("pend", "");
    p.putString("last", "rollback crash-loop");
    p.end();
    const esp_partition_t* prev = esp_ota_get_next_update_partition(nullptr);
    Serial.printf("[ota] boot attempt %u with %s pending — ROLLBACK to %s\n",
                  tries + 1, pend.c_str(), prev ? prev->label : "?");
    if (prev) {
      esp_ota_set_boot_partition(prev);
      delay(200);
      ESP.restart();
    }
    setStatus(State::kRolledBack, 0, "", "crash-loop rollback");
    return true;
  }
  p.putUChar("tries", tries + 1);
  p.end();
  gPendingAtBoot = true;
  Serial.printf("[ota] validating %s (boot attempt %u/%u) — self-test armed\n",
                pend.c_str(), tries + 1, kMaxBootAttempts);
  return false;
}

void noteSelfTestPass() { gSelfTestPass = true; }

void begin() {
  xTaskCreatePinnedToCore(otaTask, "ota", kTaskStack, nullptr, kTaskPrio,
                          nullptr, 0);
}

void requestCheck() { gCheckReq = true; }
void requestApply() { gApplyReq = true; }

Status status() {
  portENTER_CRITICAL(&gMux);
  Status s = gStatus;
  portEXIT_CRITICAL(&gMux);
  return s;
}

}  // namespace ota

#endif  // SLYTHERM_OTA
