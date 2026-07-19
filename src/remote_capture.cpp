// remote_capture.cpp — see remote_capture.h. #181 audit capture.
//
// Task shape: noteIntent() (UI task, model mutex held) only copies a small
// POD into a queue. The capture task photographs IMMEDIATELY on the first
// event of a burst (the person is at the panel *now*), then drains follow-on
// events for a settle window so a setpoint-arc drag coalesces into ONE POST
// (kind of the first event, detail of the last — the final value — plus a
// count), then ships it. One photo per kPhotoCooldownMs; events inside the
// cooldown POST metadata-only.

#ifdef SLYTHERM_CAM

#include "remote_capture.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "remote_camera.h"
#include "telnet_log.h"
#include "wifi_prov.h"

#ifndef SLYTHERM_FW_VERSION
#define SLYTHERM_FW_VERSION "0.0.0-dev"
#endif
// Compile-time default receiver; overridable via slytherm/cmd/capture_url.
#ifndef SLYTHERM_CAPTURE_URL
#define SLYTHERM_CAPTURE_URL "http://192.168.10.12:8093/capture"
#endif

namespace remote_capture {
namespace {

using namespace dettson;
using namespace dettson::ui;

// Half-res q60 (the proven /snapshot.jpg geometry, ~60 KB typical); 256 KB
// leaves generous headroom for a busy scene.
constexpr size_t kJpgCap = 256 * 1024;
constexpr uint8_t kJpgQuality = 60;
constexpr int kJpgScaleDiv = 2;
constexpr uint32_t kEncodeWaitMs = 5000;    // a serve mid-send can hold the encoder
constexpr uint32_t kPhotoCooldownMs = 30000;
constexpr uint32_t kSettleMs = 1200;        // burst coalescing window
constexpr uint32_t kHttpTimeoutMs = 10000;

struct Event {
  char kind[12];    // "setpoints","mode","preset","hold","resume","vacation","vac_clear"
  char detail[48];  // human summary ("heat 20.5 cool 24.0", "away", ...)
};

QueueHandle_t gQ = nullptr;
uint8_t* gJpg = nullptr;  // PSRAM
char gUrl[96] = "";       // "" until begin() loads NVS/default; "off" = disabled
char gId[16] = "";        // last-3-MAC hex, same derivation as remote_mqtt
Preferences gPrefs;
uint32_t gDropped = 0;

void loadUrl() {
  gPrefs.getString("url", gUrl, sizeof(gUrl));
  if (!gUrl[0]) strlcpy(gUrl, SLYTHERM_CAPTURE_URL, sizeof(gUrl));
}

// Query-string escape into out (RFC 3986 unreserved pass through).
void urlEncode(const char* s, char* out, size_t cap) {
  static const char* kHex = "0123456789ABCDEF";
  size_t o = 0;
  for (; *s && o + 4 < cap; ++s) {
    const char c = *s;
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.') {
      out[o++] = c;
    } else {
      out[o++] = '%';
      out[o++] = kHex[(c >> 4) & 0xF];
      out[o++] = kHex[c & 0xF];
    }
  }
  out[o] = '\0';
}

// POST the event (+ optional JPEG body) to <gUrl>?id=..&fw=..&kind=..&detail=..
// &n=..&photo=0|1. Returns true on 2xx.
bool post(const Event& ev, uint32_t n, const uint8_t* jpg, size_t jpgLen) {
  char detailEnc[160];
  urlEncode(ev.detail, detailEnc, sizeof(detailEnc));
  char url[320];
  snprintf(url, sizeof(url), "%s?id=%s&fw=%s&kind=%s&detail=%s&n=%lu&photo=%d",
           gUrl, gId, SLYTHERM_FW_VERSION, ev.kind, detailEnc,
           static_cast<unsigned long>(n), jpgLen > 0 ? 1 : 0);
  HTTPClient http;
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  WiFiClient net;
  if (!http.begin(net, url)) return false;
  if (jpgLen > 0) http.addHeader("Content-Type", "image/jpeg");
  const int code = http.POST(const_cast<uint8_t*>(jpg), jpgLen);
  http.end();
  if (code < 200 || code >= 300) {
    telnet_log::logf("[capture] POST failed: HTTP %d (%s n=%lu photo=%d)", code,
                     ev.kind, static_cast<unsigned long>(n), jpgLen > 0 ? 1 : 0);
    return false;
  }
  return true;
}

void captureTask(void*) {
  uint32_t lastPhotoMs = 0;
  bool everPhotographed = false;
  for (;;) {
    Event ev;
    if (xQueueReceive(gQ, &ev, portMAX_DELAY) != pdTRUE) continue;

    // Photograph FIRST — the person is at the panel right now. Cooldown keeps
    // a tap-burst or back-to-back tweaks from spamming near-identical frames.
    size_t jpgLen = 0;
    const uint32_t nowMs = millis();
    if (!everPhotographed || nowMs - lastPhotoMs >= kPhotoCooldownMs) {
      if (gJpg != nullptr) {
        jpgLen = remote_camera::captureStill(gJpg, kJpgCap, kJpgQuality,
                                             kJpgScaleDiv, kEncodeWaitMs);
        if (jpgLen > 0) { lastPhotoMs = nowMs; everPhotographed = true; }
        else telnet_log::logf("[capture] no frame for %s — metadata-only", ev.kind);
      }
    }

    // Coalesce the burst: keep the FIRST kind, the LAST detail (a setpoint
    // drag's final value), count the rest.
    uint32_t n = 1;
    Event more;
    while (xQueueReceive(gQ, &more, pdMS_TO_TICKS(kSettleMs)) == pdTRUE) {
      strlcpy(ev.detail, more.detail, sizeof(ev.detail));
      ++n;
    }

    if (strcmp(gUrl, "off") == 0) continue;
    if (!wifi_prov::connected()) {
      telnet_log::logf("[capture] wifi down — %s event dropped", ev.kind);
      continue;
    }
    bool ok = post(ev, n, gJpg, jpgLen);
    if (!ok) {  // one retry — the receiver may have been mid-restart
      vTaskDelay(pdMS_TO_TICKS(5000));
      ok = post(ev, n, gJpg, jpgLen);
    }
    telnet_log::logf("[capture] %s n=%lu photo=%uB -> %s", ev.kind,
                     static_cast<unsigned long>(n), static_cast<unsigned>(jpgLen),
                     ok ? "saved" : "LOST");
  }
}

}  // namespace

void begin() {
  if (gQ != nullptr) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(gId, sizeof(gId), "%02x%02x%02x", mac[3], mac[4], mac[5]);
  gPrefs.begin("capture", false);
  loadUrl();
  gJpg = static_cast<uint8_t*>(ps_malloc(kJpgCap));
  if (gJpg == nullptr) telnet_log::logf("[capture] PSRAM alloc FAILED — metadata-only");
  gQ = xQueueCreate(8, sizeof(Event));
  // Same core/prio neighborhood as the camera's http task; POSTs are small
  // and the encode is hardware, so this never competes with the UI.
  xTaskCreatePinnedToCore(captureTask, "cap_post", 8192, nullptr, 1, nullptr, 1);
  telnet_log::logf("[capture] up: url=%s cooldown=%lus", gUrl,
                   static_cast<unsigned long>(kPhotoCooldownMs / 1000));
}

void noteIntent(const UiIntent& it, const char* presetName) {
  if (gQ == nullptr) return;
  Event ev = {};
  switch (it.type) {
    case IntentType::kSetSetpoints:
      strlcpy(ev.kind, "setpoints", sizeof(ev.kind));
      snprintf(ev.detail, sizeof(ev.detail), "heat %.1f cool %.1f",
               static_cast<double>(it.heatC), static_cast<double>(it.coolC));
      break;
    case IntentType::kSetMode: {
      strlcpy(ev.kind, "mode", sizeof(ev.kind));
      const char* m = "off";
      if (it.mode == UserMode::kHeat) m = "heat";
      else if (it.mode == UserMode::kCool) m = "cool";
      else if (it.mode == UserMode::kAuto) m = "auto";
      else if (it.mode == UserMode::kEmergencyHeat) m = "em_heat";
      strlcpy(ev.detail, m, sizeof(ev.detail));
      break;
    }
    case IntentType::kSetPreset:
      strlcpy(ev.kind, "preset", sizeof(ev.kind));
      strlcpy(ev.detail, presetName ? presetName : "", sizeof(ev.detail));
      break;
    case IntentType::kSetHold: {
      strlcpy(ev.kind, "hold", sizeof(ev.kind));
      const char* h = "?";
      switch (it.hold) {
        case HoldType::kUntilNextPreset: h = "next-preset"; break;
        case HoldType::kTwoHours: h = "2h"; break;
        case HoldType::kFourHours: h = "4h"; break;
        case HoldType::kIndefinite: h = "indefinite"; break;
        default: break;
      }
      strlcpy(ev.detail, h, sizeof(ev.detail));
      break;
    }
    case IntentType::kClearHold:
      strlcpy(ev.kind, "resume", sizeof(ev.kind));
      break;
    case IntentType::kSetVacation:
      strlcpy(ev.kind, "vacation", sizeof(ev.kind));
      snprintf(ev.detail, sizeof(ev.detail), "start+%ud %un heat %.1f cool %.1f",
               static_cast<unsigned>(it.vacStartDays), static_cast<unsigned>(it.vacNights),
               static_cast<double>(it.heatC), static_cast<double>(it.coolC));
      break;
    case IntentType::kClearVacation:
      strlcpy(ev.kind, "vac_clear", sizeof(ev.kind));
      break;
    default:
      return;  // alarm ack / OTA — not an audit-capture change
  }
  if (xQueueSend(gQ, &ev, 0) != pdTRUE) ++gDropped;  // never block the UI task
}

void setUrl(const char* url) {
  char clean[sizeof(gUrl)] = "";
  if (url) strlcpy(clean, url, sizeof(clean));
  size_t len = strlen(clean);
  while (len && clean[len - 1] == '/') clean[--len] = '\0';
  if (len && strcmp(clean, "off") != 0 && strncmp(clean, "http://", 7) != 0 &&
      strncmp(clean, "https://", 8) != 0) {
    telnet_log::logf("[capture] url REJECTED (not a URL): %s", clean);
    return;
  }
  gPrefs.putString("url", clean);  // "" persists too -> loadUrl falls back to default
  if (!clean[0]) strlcpy(gUrl, SLYTHERM_CAPTURE_URL, sizeof(gUrl));
  else strlcpy(gUrl, clean, sizeof(gUrl));
  telnet_log::logf("[capture] url set: %s", gUrl);
}

}  // namespace remote_capture

#endif  // SLYTHERM_CAM
