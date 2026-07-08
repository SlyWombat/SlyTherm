// boot_guard.cpp — see boot_guard.h.

#include "boot_guard.h"

#include <Arduino.h>
#include <Preferences.h>

#include "HaMqtt.h"  // bootStatusJson (#123)

#include "esp_system.h"
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#define BOOTG_HAS_COREDUMP 1
#endif

#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

namespace boot_guard {
namespace {

constexpr uint32_t kHealthyUptimeMs = 120u * 1000u;
constexpr uint8_t kLatchThreshold = 3;

// Previous-run uptime, RTC noinit: survives any reset, garbage after a true
// power cycle — hence the magic word.
constexpr uint32_t kUpMagic = 0x55505453u;  // "UPTS"
RTC_NOINIT_ATTR uint32_t gUpMagic;
RTC_NOINIT_ATTR uint32_t gUpSeconds;

Preferences gPrefs;
char gReason[12] = "unknown";
bool gAbnormal = false;
bool gCoredump = false;
uint32_t gPrevUptimeS = 0;
uint32_t gCount = 0;
bool gCleared = false;  // healthyTick's once-latch
char gJson[160] = "{}";

const char* mapReason(esp_reset_reason_t r, bool& abnormal) {
  abnormal = false;
  switch (r) {
    case ESP_RST_POWERON: return "power_on";
    case ESP_RST_SW: return "sw_reset";
    case ESP_RST_DEEPSLEEP: return "deep_sleep";
    case ESP_RST_PANIC: abnormal = true; return "panic";
    case ESP_RST_INT_WDT: abnormal = true; return "int_wdt";
    case ESP_RST_TASK_WDT: abnormal = true; return "task_wdt";
    case ESP_RST_WDT: abnormal = true; return "wdt";
    case ESP_RST_BROWNOUT: abnormal = true; return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_EXT: return "ext";
    default: return "unknown";
  }
}

}  // namespace

void begin(const char* nvsNamespace) {
  bool abnormal = false;
  const char* r = mapReason(esp_reset_reason(), abnormal);
  strlcpy(gReason, r, sizeof(gReason));
  gAbnormal = abnormal;

#ifdef BOOTG_HAS_COREDUMP
  gCoredump = (esp_core_dump_image_check() == ESP_OK);
#endif

  if (gUpMagic == kUpMagic) gPrevUptimeS = gUpSeconds;  // else power loss: unknown
  gUpMagic = kUpMagic;
  gUpSeconds = 0;

  gPrefs.begin(nvsNamespace, false);
  // Consecutive-ABNORMAL counter: an abnormal boot increments, a clean boot
  // resets — a reflash/OTA (sw_reset/power_on) can never accumulate a latch.
  gCount = gAbnormal ? gPrefs.getUInt("bc", 0) + 1 : 0;
  gPrefs.putUInt("bc", gCount);

  const std::string j =
      dettson::hamqtt::bootStatusJson(gReason, gCoredump, gPrevUptimeS, SLYTHERM_FW_BUILD, gCount);
  strlcpy(gJson, j.c_str(), sizeof(gJson));
  Serial.printf("[boot] reason=%s coredump=%d prevUptime=%lus abnormalBoots=%lu\n", gReason,
                (int)gCoredump, (unsigned long)gPrevUptimeS, (unsigned long)gCount);
}

void healthyTick(uint32_t nowMs) {
  gUpSeconds = nowMs / 1000u;  // prev-uptime mirror for the NEXT boot
  if (!gCleared && gCount > 0 && nowMs >= kHealthyUptimeMs) {
    gCleared = true;
    gPrefs.putUInt("bc", 0);  // survived long enough: the loop (if any) is over
  }
}

const char* reason() { return gReason; }
bool coredumpPresent() { return gCoredump; }
uint32_t prevUptimeS() { return gPrevUptimeS; }
uint32_t bootCount() { return gCount; }
const char* statusJson() { return gJson; }

bool reducedUi() { return gCount >= kLatchThreshold; }

void clearLatch() {
  gCount = 0;
  gPrefs.putUInt("bc", 0);
}

}  // namespace boot_guard
