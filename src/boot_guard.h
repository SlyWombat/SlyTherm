// boot_guard.h — boot/crash accounting shared by both entrypoints
// (#122 Remote reset-loop latch + #123 boot telemetry).
//
// One mechanism, two consumers: begin() counts this boot and captures the
// reset reason + on-flash coredump presence + previous-run uptime (RTC
// noinit — survives resets, reads unknown after true power loss); the
// telemetry consumer publishes statusJson() retained on every MQTT connect;
// the latch consumer (Remote-only — the Controller keeps its richer
// SafetySupervisor ResetLoopAccountant) checks reducedUi() to boot the #80
// safe screen after 3 consecutive ABNORMAL boots. A clean power-on or
// sw-reset never trips the latch, so reflash/OTA cycles are invisible to it.
#pragma once

#include <cstdint>

namespace boot_guard {

// Count this boot + capture reason/coredump/prev-uptime. Call ONCE, early in
// setup(), before tasks start. nvsNamespace: per-role Preferences namespace.
void begin(const char* nvsNamespace);

// Call ~1 Hz from a task that dies with the app (the loop/mqtt task, NOT a
// watchdog-immune context). After kHealthyUptimeMs of continuous life it
// zeroes the consecutive-abnormal counter (once), and it refreshes the
// prev-uptime RTC mirror every call.
void healthyTick(uint32_t nowMs);

const char* reason();        // fixed esp_reset_reason() mapping (see .cpp)
bool coredumpPresent();      // a crash dump is waiting on flash (#124 pulls it)
uint32_t prevUptimeS();      // previous run's uptime; 0 = unknown (power loss)
uint32_t bootCount();        // consecutive ABNORMAL boots (0 after a clean one)
const char* statusJson();    // cached #123 payload, built once at begin()

// #122 (Remote): >=3 consecutive abnormal boots -> boot the reduced safe UI.
bool reducedUi();
// Safe screen's "Restore full screen": zero the counter (caller reboots).
void clearLatch();

}  // namespace boot_guard
