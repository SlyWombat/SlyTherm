// boot_guard.cpp — see boot_guard.h.

#include "boot_guard.h"

#include <Arduino.h>
#include <Preferences.h>

#include "HaMqtt.h"  // bootStatusJson (#123)

#include <ctime>

#include "esp_system.h"
#if __has_include("esp_core_dump.h")
#include "esp_core_dump.h"
#define BOOTG_HAS_COREDUMP 1
#endif
#if __has_include("esp_rom_sys.h")
#include "esp_rom_sys.h"
#define BOOTG_HAS_ROM_RESET 1
#endif

#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

namespace boot_guard {
namespace {

constexpr uint32_t kHealthyUptimeMs = 120u * 1000u;
constexpr uint8_t kLatchThreshold = 3;
// #145 last-alive NVS heartbeat cadence. 5 min bounds the post-power-loss
// "when did it die" answer and stays far under NVS wear limits (~576
// entry-writes/day against wear-leveled pages).
constexpr uint32_t kLastAliveEveryMs = 5u * 60u * 1000u;

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
uint32_t gRawReason = 0;         // esp_reset_reason() numeric (#145)
uint32_t gRtcReason0 = 0;        // ROM reset reason per CPU (#145)
uint32_t gRtcReason1 = 0;
uint32_t gLastAliveUptimeS = 0;  // previous run's NVS heartbeat (#145)
uint32_t gLastAliveEpoch = 0;
uint32_t gNextLastAliveMs = kLastAliveEveryMs;

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
  const esp_reset_reason_t rr = esp_reset_reason();
  const char* r = mapReason(rr, abnormal);
  strlcpy(gReason, r, sizeof(gReason));
  gAbnormal = abnormal;
  gRawReason = (uint32_t)rr;
#ifdef BOOTG_HAS_ROM_RESET
  gRtcReason0 = (uint32_t)esp_rom_get_reset_reason(0);
  gRtcReason1 = (uint32_t)esp_rom_get_reset_reason(1);
#endif

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

  // #145: the previous run's last-alive heartbeat, then zero it so a run
  // shorter than one heartbeat reads unknown instead of two runs stale.
  gLastAliveUptimeS = gPrefs.getUInt("laU", 0);
  gLastAliveEpoch = gPrefs.getUInt("laE", 0);
  if (gLastAliveUptimeS) gPrefs.putUInt("laU", 0);
  if (gLastAliveEpoch) gPrefs.putUInt("laE", 0);

  Serial.printf("[boot] reason=%s raw=%lu/%lu/%lu coredump=%d prevUptime=%lus "
                "lastAlive=%lus@%lu abnormalBoots=%lu\n",
                gReason, (unsigned long)gRawReason, (unsigned long)gRtcReason0,
                (unsigned long)gRtcReason1, (int)gCoredump,
                (unsigned long)gPrevUptimeS, (unsigned long)gLastAliveUptimeS,
                (unsigned long)gLastAliveEpoch, (unsigned long)gCount);
}

void healthyTick(uint32_t nowMs) {
  gUpSeconds = nowMs / 1000u;  // prev-uptime mirror for the NEXT boot
  if (!gCleared && gCount > 0 && nowMs >= kHealthyUptimeMs) {
    gCleared = true;
    gPrefs.putUInt("bc", 0);  // survived long enough: the loop (if any) is over
  }
  if (nowMs >= gNextLastAliveMs) {  // #145 last-alive NVS heartbeat
    gNextLastAliveMs += kLastAliveEveryMs;
    gPrefs.putUInt("laU", nowMs / 1000u);
    const time_t e = time(nullptr);
    gPrefs.putUInt("laE", e > 1600000000 ? (uint32_t)e : 0u);
  }
}

const char* reason() { return gReason; }
bool coredumpPresent() { return gCoredump; }
uint32_t prevUptimeS() { return gPrevUptimeS; }
uint32_t bootCount() { return gCount; }

std::string statusJson(uint32_t uptimeS) {
  dettson::hamqtt::BootStatus s;
  s.reason = gReason;
  s.coredump = gCoredump;
  s.prevUptimeS = gPrevUptimeS;
  s.version = SLYTHERM_FW_BUILD;
  s.bootCount = gCount;
  s.rawReason = gRawReason;
  s.rtcReason0 = gRtcReason0;
  s.rtcReason1 = gRtcReason1;
  s.lastAliveUptimeS = gLastAliveUptimeS;
  s.lastAliveEpoch = gLastAliveEpoch;
  s.uptimeS = uptimeS;
  return dettson::hamqtt::bootStatusJson(s);
}

bool reducedUi() { return gCount >= kLatchThreshold; }

void clearLatch() {
  gCount = 0;
  gPrefs.putUInt("bc", 0);
}

}  // namespace boot_guard
