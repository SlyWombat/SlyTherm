// heap_debug.cpp — see heap_debug.h.
#include "heap_debug.h"

#ifdef SLYTHERM_HEAPDBG

#include <Arduino.h>
#include <string.h>

#include "esp_cpu.h"
#include "riscv/interrupt.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "telnet_log.h"

// Tunables (build flags so a rebuild isn't needed to re-aim the watchpoint at
// a different address after the layout shifts).
#ifndef SLYTHERM_HEAPDBG_MS
#define SLYTHERM_HEAPDBG_MS 10000       // integrity + canary tick
#endif
#ifndef SLYTHERM_HEAPDBG_ADDR
#define SLYTHERM_HEAPDBG_ADDR 0x4ff5cd90u  // the #193 GDMA TX channel object
#endif
#ifndef SLYTHERM_HEAPDBG_WP_SIZE
#define SLYTHERM_HEAPDBG_WP_SIZE 8      // group+pair pointers; must be pow2
#endif
#ifndef SLYTHERM_HEAPDBG_WP
#define SLYTHERM_HEAPDBG_WP 1           // 0 = canary only, no CPU watchpoint
#endif
// Address of IDF's gdma_default_tx_isr. Self-aiming beats a hardcoded object
// address: every code change reshuffles the heap, so the channel object moves,
// but this ISR lives in the PRECOMPILED lib and stayed at 0x4ff04190 across
// builds. The interrupt table's arg for this handler IS the channel object.
// Re-read after a framework bump:
//   riscv32-esp-elf-nm firmware.elf | grep ' T gdma_default_tx_isr'
#ifndef SLYTHERM_HEAPDBG_ISR
#define SLYTHERM_HEAPDBG_ISR 0x4ff04190u
#endif

namespace heap_debug {
namespace {

constexpr uintptr_t kAddr = SLYTHERM_HEAPDBG_ADDR;
constexpr size_t kCanaryWords = 4;                 // 16 bytes snapshotted
constexpr int kWpNum = 0;                          // 3 slots, none used by IDF
                                                   // (FREERTOS_WATCHPOINT_END_OF_STACK is off)
// HP-SRAM window on the P4. A plausible gdma_channel_t holds a group pointer
// here in its first word; anything else means the heap moved under us and the
// address is stale.
constexpr uintptr_t kSramLo = 0x4ff00000u;
constexpr uintptr_t kSramHi = 0x4ffc0000u;  // P4 HP-SRAM top (768KB @0x4ff00000)

uint32_t gAddr = (uint32_t)SLYTHERM_HEAPDBG_ADDR;  // resolved at begin()
uint32_t gSnap[kCanaryWords];
bool gCanaryArmed = false;
bool gWpArmed = false;

void hexdump(const char* tag, const uint32_t* w) {
  telnet_log::logf("[heapdbg] %s @%08x: %08x %08x %08x %08x", tag,
                   (unsigned)gAddr, (unsigned)w[0], (unsigned)w[1],
                   (unsigned)w[2], (unsigned)w[3]);
}

// esp_cpu_set_watchpoint acts on the CALLING core only, so this runs on both.
void IRAM_ATTR armOnThisCore(void*) {
  esp_cpu_set_watchpoint(kWpNum, (const void*)gAddr, SLYTHERM_HEAPDBG_WP_SIZE,
                         ESP_CPU_WATCHPOINT_STORE);
}

// ---- ISR-argument finder (#193 follow-up) ----
// The 2026-08-17 dump named the GDMA TX channel object at 0x4ff5cd90, but that
// address belongs to the RELEASE image's heap layout; this build allocates
// differently, so the recorded address reads all-zero and arming is refused.
//
// Walking the heap for a "channel-shaped" block was a dead end (133 blocks pass
// any reasonable shape filter — the poison header means user data starts at
// +8, and plenty of structs begin with a pointer). The decisive source is the
// CPU interrupt table itself: IDF registers gdma_default_tx_isr with the
// channel object AS ITS ARGUMENT, so the arg for the GDMA interrupt IS the
// address we need. Print handler+arg for every vector on both cores and map
// the handler to a symbol offline:
//   riscv32-esp-elf-nm firmware.elf | sort | grep -i <handler-addr-prefix>
// then re-aim with -DSLYTHERM_HEAPDBG_ADDR=<arg of gdma_default_tx_isr>.
// IMPORTANT: an esp_ipc_call_blocking callback runs on the target core's `ipc`
// task, whose stack is ~1KB (the 2026-08-17 dump shows ipc0 at 288 used / 596
// free). Calling telnet_log::logf() there overflows it inside _vsnprintf_r —
// that is exactly what killed build 1.4.12 at boot, and the OTA rollback put
// 1.4.10 back. So the cross-core half only COLLECTS; all printing happens on
// the calling task afterwards.
struct IntrRow { uint8_t core; uint8_t n; uint32_t h; uint32_t a; };
constexpr int kMaxRows = 64;
IntrRow gRows[kMaxRows];
volatile int gRowN = 0;

void collectIntr(void*) {
  const uint8_t core = static_cast<uint8_t>(xPortGetCoreID());
  for (int n = 0; n < 32; ++n) {
    void* a = esp_cpu_intr_get_handler_arg(n);
    void* h = reinterpret_cast<void*>(intr_handler_get(n));
    if (h == nullptr && a == nullptr) continue;
    const int i = gRowN;
    if (i >= kMaxRows) return;
    gRows[i].core = core;
    gRows[i].n = static_cast<uint8_t>(n);
    gRows[i].h = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h));
    gRows[i].a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(a));
    gRowN = i + 1;
  }
}

// Returns the GDMA TX channel object (the ISR's registered argument), or 0.
uint32_t findGdmaTxChannel() {
  gRowN = 0;
  collectIntr(nullptr);
  esp_ipc_call_blocking(xPortGetCoreID() ? 0 : 1, collectIntr, nullptr);
  const int n = gRowN;
  for (int i = 0; i < n; ++i)
    if (gRows[i].h == (uint32_t)SLYTHERM_HEAPDBG_ISR && gRows[i].a != 0) {
      telnet_log::logf("[heapdbg] gdma_default_tx_isr on core%u vector %u -> "
                       "channel %08x", (unsigned)gRows[i].core,
                       (unsigned)gRows[i].n, (unsigned)gRows[i].a);
      return gRows[i].a;
    }
  telnet_log::logf("[heapdbg] no vector with handler %08x in %d entries - "
                   "re-read the symbol from the elf",
                   (unsigned)SLYTHERM_HEAPDBG_ISR, n);
  return 0;
}

void scanCandidates() {
  telnet_log::log("[heapdbg] interrupt table (handler -> arg); the GDMA TX "
                  "entry's arg is the channel object");
  gRowN = 0;
  collectIntr(nullptr);
  esp_ipc_call_blocking(xPortGetCoreID() ? 0 : 1, collectIntr, nullptr);
  const int n = gRowN;
  for (int i = 0; i < n; ++i)
    telnet_log::logf("[heapdbg] intr core%u n=%2u handler=%08x arg=%08x",
                     (unsigned)gRows[i].core, (unsigned)gRows[i].n,
                     (unsigned)gRows[i].h, (unsigned)gRows[i].a);
  telnet_log::logf("[heapdbg] intr table done (%d vectors)", n);
}

void die(const char* why) {
  telnet_log::logf("[heapdbg] %s — aborting to capture a coredump", why);
  Serial.flush();
  vTaskDelay(pdMS_TO_TICKS(250));  // let telnet drain before the panic
  abort();
}

void onFailedAlloc(size_t size, uint32_t caps, const char* fn) {
  telnet_log::logf("[heapdbg] ALLOC FAILED %u bytes caps=0x%x in %s "
                   "(internal free=%u largest=%u)",
                   (unsigned)size, (unsigned)caps, fn ? fn : "?",
                   (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void task(void*) {
  uint32_t ticks = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(SLYTHERM_HEAPDBG_MS));
    ++ticks;

    // 1. Poison canaries. Internal DRAM only: the #193 victim is HP-SRAM, and
    // walking the 32MB PSRAM heap on every tick would stall the camera path
    // for no benefit. print_errors=true makes IDF name the bad block first.
    if (!heap_caps_check_integrity(MALLOC_CAP_INTERNAL, true))
      die("internal heap integrity FAILED");

    // 2. The struct canary — the net that can actually see #193.
    if (gCanaryArmed) {
      uint32_t now[kCanaryWords];
      memcpy(now, (const void*)gAddr, sizeof(now));
      if (memcmp(now, gSnap, sizeof(now)) != 0) {
        hexdump("was", gSnap);
        hexdump("now", now);
        die("watched struct CHANGED");
      }
    }

    // 3. Trend line every ~5 min: a slow internal-heap slide is its own
    // suspect (the corruption showed up 7.2 days into an uptime).
    // Unarmed means we are still hunting the address: reprint the interrupt
    // table every 60 s so ANY telnet session catches it (the log ring replays
    // only recent lines, and boot chatter pushes begin()'s dump out of it).
    if (!gCanaryArmed && ticks % (300000u / SLYTHERM_HEAPDBG_MS) == 0)
      scanCandidates();
    if (ticks % (300000u / SLYTHERM_HEAPDBG_MS) == 0) {
      telnet_log::logf("[heapdbg] internal free=%u min=%u largest=%u | "
                       "canary=%s wp=%s",
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                       gCanaryArmed ? "on" : "OFF", gWpArmed ? "on" : "OFF");
    }
  }
}

}  // namespace

void begin() {
  heap_caps_register_failed_alloc_callback(onFailedAlloc);

  // Snapshot first, and refuse to trust a stale address: if word 0 isn't a
  // plausible HP-SRAM pointer, the allocation layout has moved and both the
  // canary and the watchpoint would be aimed at the wrong object. Say so
  // loudly rather than reporting a false positive every 10 s.
  // Self-aim, and REFUSE to arm if it fails. A watchpoint on an unidentified
  // object is worse than no watchpoint: a trip is a panic, it happens long
  // after the image is confirmed (so OTA rollback cannot save us), and on
  // reboot begin() re-arms the same address — an indefinite panic loop on a
  // panel we can only reach over the tunnel. 1.4.14 armed on the fallback
  // address without ever establishing what lived there; that is the hole this
  // closes. Only the ISR-derived address is defensible.
  const uint32_t found = findGdmaTxChannel();
  if (!found) {
    telnet_log::log("[heapdbg] self-aim FAILED — arming NOTHING. The heap "
                    "check, failed-alloc callback and trend log stay on; the "
                    "vector table is dumped below every 5 min for analysis.");
    scanCandidates();
    xTaskCreatePinnedToCore(task, "heapdbg", 3072, nullptr, 1, nullptr, 0);
    telnet_log::logf("[heapdbg] watchdog up: tick=%ums (unarmed)",
                     (unsigned)SLYTHERM_HEAPDBG_MS);
    return;
  }
  gAddr = found;

  uint32_t w[kCanaryWords];
  memcpy(w, (const void*)gAddr, sizeof(w));
  hexdump("arm", w);
  const bool plausible = (w[0] >= kSramLo && w[0] < kSramHi);
  if (plausible) {
    memcpy(gSnap, w, sizeof(gSnap));
    gCanaryArmed = true;
  } else {
    telnet_log::logf("[heapdbg] word0=%08x is not an HP-SRAM pointer — heap "
                     "layout moved; canary/watchpoint NOT armed. Re-aim with "
                     "-DSLYTHERM_HEAPDBG_ADDR after reading a fresh dump.",
                     (unsigned)w[0]);
    scanCandidates();  // print re-aim candidates instead of waiting for a panic
  }

#if SLYTHERM_HEAPDBG_WP
  if (plausible) {
    armOnThisCore(nullptr);                         // this core
    esp_ipc_call_blocking(xPortGetCoreID() ? 0 : 1, armOnThisCore, nullptr);
    gWpArmed = true;
    telnet_log::logf("[heapdbg] store watchpoint armed on both cores: "
                     "%u bytes @%08x (wp%d)",
                     (unsigned)SLYTHERM_HEAPDBG_WP_SIZE, (unsigned)gAddr, kWpNum);
  }
#endif

  xTaskCreatePinnedToCore(task, "heapdbg", 3072, nullptr, 1, nullptr, 0);
  telnet_log::logf("[heapdbg] watchdog up: tick=%ums", (unsigned)SLYTHERM_HEAPDBG_MS);
}

}  // namespace heap_debug

#endif  // SLYTHERM_HEAPDBG
