// sniffer_display.h — optional on-screen dashboard for the Waveshare
// ESP32-S3-Touch-LCD-4.3B (compiled only with -DSNIFFER_DISPLAY, S3 only).
//
// The display is a READ-ONLY view of the sniffer's live counters. It never
// touches the RS-485 UART or transceiver, so it cannot affect the RX-only
// guarantee in main_sniffer.cpp. If the panel fails to init, the sniffer keeps
// running headless (web console + serial unaffected).
#pragma once

#include <cstdint>

namespace display {

// Snapshot of the sniffer state, filled by main_sniffer.cpp from the same
// counters the web console uses. Plain data — no Arduino/bus types here so the
// header stays cheap to include.
struct Stats {
  bool     wifiConnected;
  bool     ap;            // true = SoftAP fallback, false = joined STA
  char     ip[20];
  int      rssi;
  uint32_t baud;
  bool     locked;        // auto-baud locked
  uint32_t bytes;
  uint32_t ok;            // Fletcher-valid frames
  uint32_t badcrc;
  uint32_t badlen;
  uint32_t overrun;
  uint32_t v9600;         // valid frames seen at each candidate baud
  uint32_t v38400;
  uint32_t capRecords;    // capture buffer fill
  uint32_t capBytes;
  uint32_t uptimeMs;
  uint32_t heap;
  uint8_t  topType[4];    // busiest msgType bytes by census
  uint32_t topCount[4];
  int      topN;
};

// Bring up the CH422G backlight + RGB panel and draw the static header. Safe to
// call once from setup(); logs and no-ops on failure.
void begin();

// Redraw the live rows from a fresh snapshot. Call ~1 Hz from loop(); writes
// straight to the RGB framebuffer (no heavy double-buffer flush) so it never
// stalls the byte pump past the CT-485 inter-frame gap.
void update(const Stats& s);

}  // namespace display
