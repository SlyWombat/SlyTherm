// sniffer_display.cpp — optional on-screen dashboard for the Waveshare
// ESP32-S3-Touch-LCD-4.3B. Compiled ONLY in env:sniffer_s3_display
// (-DSNIFFER_DISPLAY + build_src_filter). See sniffer_display.h.
//
// RX-ONLY: this file only READS the Stats snapshot and draws it. It includes no
// UART/RS-485 code and drives no bus pin — it cannot affect the sniffer's
// RX-only guarantee.
//
// ============================ HARDWARE-VERIFY ==============================
// The values below are lifted from the Waveshare ESP32-S3-Touch-LCD-4.3
// Arduino_GFX reference (github.com/Westcott1/...). They are NOT bench-verified
// on this exact board. If the panel is BLANK or GARBLED, check, in order:
//   1. Backlight: the CH422G raw-I2C sequence (kCh422* below). If the panel is
//      clearly initialised but dark, the backlight bit/logic is the suspect —
//      flip kCh422OutBringup / the LCD_BL bit.
//   2. PSRAM: env must set board_build.arduino.memory_type=qio_opi +
//      -DBOARD_HAS_PSRAM, or the framebuffer alloc fails (black/reboot).
//   3. Panel timing (porches/pclk) and the RGB pin map — must come from ONE
//      known-good source; do not mix.
// The sniffer + web console run fine regardless of anything in this file.
// ===========================================================================

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <math.h>

#include "sniffer_display.h"

namespace display {
namespace {

// ---- CH422G I/O expander (raw I2C; controls backlight + panel/touch reset) --
// CH422G is addressed by device address, not register: 0x24 sets the IO mode,
// 0x38 writes the 8 EXIO outputs. On this board EXIO1=touch-reset,
// EXIO2=LCD-backlight, EXIO3=LCD-reset. >>> VERIFY if the screen stays dark. <<<
constexpr uint8_t kCh422ModeAddr = 0x24;   // WR_SET: IO mode/enable
constexpr uint8_t kCh422OutAddr  = 0x38;   // WR_IO : 8 EXIO output bits
constexpr uint8_t kBitTpRst      = 1 << 1; // EXIO1  touch reset
constexpr uint8_t kBitLcdBl      = 1 << 2; // EXIO2  LCD backlight
constexpr uint8_t kBitLcdRst     = 1 << 3; // EXIO3  LCD reset
constexpr uint8_t kCh422ModePP   = 0x01;   // push-pull output enable
// Resets released, backlight off during bring-up; backlight added after begin().
constexpr uint8_t kCh422OutBringup = kBitTpRst | kBitLcdRst;

constexpr int kSdaPin = 8;
constexpr int kSclPin = 9;

uint8_t gCh422Out = 0;

void ch422Write(uint8_t addr, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(val);
  Wire.endTransmission();
}
void ch422SetOut(uint8_t val) {
  gCh422Out = val;
  ch422Write(kCh422OutAddr, gCh422Out);
}

// ---- RGB panel + display (pins/timing from the Waveshare 4.3 reference) -----
Arduino_ESP32RGBPanel* gPanel = nullptr;
Arduino_RGB_Display*   gGfx   = nullptr;
bool                   gReady = false;

// RGB565 palette.
constexpr uint16_t kBg     = 0x0000;  // black
constexpr uint16_t kFg     = 0xC618;  // light grey
constexpr uint16_t kHi     = 0x07E0;  // green (good / active)
constexpr uint16_t kWarn   = 0xF800;  // red (down / error)
constexpr uint16_t kAccent = 0x3D9F;  // cyan-ish header

constexpr int kRowH   = 30;   // px per live row (text size 2 = 16px tall)
constexpr int kRowTop = 86;   // first live row y

void row(int idx, const char* label, const char* val, uint16_t vc) {
  const int y = kRowTop + idx * kRowH;
  gGfx->fillRect(0, y, 800, kRowH, kBg);      // clear the row (direct FB write)
  gGfx->setTextSize(2);
  gGfx->setTextColor(kFg, kBg);
  gGfx->setCursor(10, y);
  gGfx->print(label);
  gGfx->setTextColor(vc, kBg);
  gGfx->print(val);
}

// ---- SlyTherm splash graphics (dual-fuel emblem: flame + snowflake) ---------

// Layered teardrop flame (dark-red -> orange -> yellow -> white core).
void drawFlame(int cx, int cy, int s) {
  gGfx->fillCircle(cx, cy + s / 2, s, 0x8800);
  gGfx->fillTriangle(cx - s, cy + s / 2, cx + s, cy + s / 2, cx, cy - 2 * s, 0x8800);
  const int s2 = s * 7 / 10;
  gGfx->fillCircle(cx, cy + s / 2, s2, 0xFB40);
  gGfx->fillTriangle(cx - s2, cy + s / 2, cx + s2, cy + s / 2, cx, cy - s * 3 / 2, 0xFB40);
  const int s3 = s * 4 / 10;
  gGfx->fillCircle(cx, cy + s / 2, s3, 0xFEA0);
  gGfx->fillTriangle(cx - s3, cy + s / 2, cx + s3, cy + s / 2, cx, cy - s, 0xFEA0);
  gGfx->fillCircle(cx, cy + s / 3, s * 2 / 10, 0xFFFF);
}

// Six-armed snowflake with side branches.
void drawSnowflake(int cx, int cy, int r) {
  const uint16_t c = 0x9FFF;  // light cyan
  for (int i = 0; i < 6; ++i) {
    const float a  = i * 3.14159265f / 3.0f;
    const int   ex = cx + static_cast<int>(r * cosf(a));
    const int   ey = cy + static_cast<int>(r * sinf(a));
    gGfx->drawLine(cx, cy, ex, ey, c);
    for (int k = 0; k < 2; ++k) {
      const float br = (k ? 0.85f : 0.6f) * r;
      const int   bx = cx + static_cast<int>(br * cosf(a));
      const int   by = cy + static_cast<int>(br * sinf(a));
      const int   bl = r / 4;
      gGfx->drawLine(bx, by, bx + static_cast<int>(bl * cosf(a + 1.05f)),
                     by + static_cast<int>(bl * sinf(a + 1.05f)), c);
      gGfx->drawLine(bx, by, bx + static_cast<int>(bl * cosf(a - 1.05f)),
                     by + static_cast<int>(bl * sinf(a - 1.05f)), c);
    }
  }
  gGfx->fillCircle(cx, cy, 3, 0xFFFF);
}

// Full-screen boot splash. Held briefly by begin(), then replaced by the
// live dashboard.
void showSplash() {
  gGfx->fillScreen(kBg);
  gGfx->fillRect(0, 0, 800, 6, kAccent);
  gGfx->fillRect(0, 474, 800, 6, kAccent);

  drawFlame(150, 150, 46);
  drawSnowflake(650, 146, 54);

  // Wordmark with a drop shadow for depth.
  gGfx->setTextSize(7);
  gGfx->setTextColor(0x18E3);
  gGfx->setCursor(256, 116);
  gGfx->print("SlyTherm");
  gGfx->setTextColor(0xFFFF);
  gGfx->setCursor(253, 113);
  gGfx->print("SlyTherm");
  gGfx->fillRect(253, 178, 336, 5, kAccent);

  gGfx->setTextSize(2);
  gGfx->setTextColor(kFg);
  gGfx->setCursor(232, 250);
  gGfx->print("DUAL-FUEL  CLIMATE  CONTROL");
  gGfx->setTextColor(0x7BEF);
  gGfx->setCursor(214, 296);
  gGfx->print("CT-485  -  RX-only listener  -  local");

  gGfx->setTextSize(1);
  gGfx->setTextColor(0x5AEB);
  gGfx->setCursor(292, 452);
  gGfx->print("Dettson C105-MV   -   SlyTherm firmware");
}

}  // namespace

void begin() {
  // Backlight/reset lines live behind the CH422G, not on GPIOs.
  Wire.begin(kSdaPin, kSclPin);
  ch422Write(kCh422ModeAddr, kCh422ModePP);   // EXIO -> push-pull outputs
  ch422SetOut(kCh422OutBringup);              // release resets, backlight off
  delay(20);

  gPanel = new Arduino_ESP32RGBPanel(
      5 /*DE*/, 3 /*VSYNC*/, 46 /*HSYNC*/, 7 /*PCLK*/,
      1, 2, 42, 41, 40,          // R0..R4
      39, 0, 45, 48, 47, 21,     // G0..G5
      14, 38, 18, 17, 10,        // B0..B4
      0 /*hsync_pol*/, 40 /*hs_front*/, 48 /*hs_pulse*/, 88 /*hs_back*/,
      0 /*vsync_pol*/, 13 /*vs_front*/, 3 /*vs_pulse*/, 32 /*vs_back*/,
      1 /*pclk_active_neg*/, 16000000 /*prefer_speed*/);
  gGfx = new Arduino_RGB_Display(800, 480, gPanel, 0 /*rotation*/,
                                 true /*auto_flush*/);

  if (!gGfx->begin()) {
    Serial.println("display: Arduino_RGB_Display begin() failed — headless");
    gReady = false;
    return;
  }
  gGfx->fillScreen(kBg);
  ch422SetOut(kCh422OutBringup | kBitLcdBl);   // backlight on now that panel is up

  // Boot splash, then the live dashboard header.
  showSplash();
  delay(1600);
  gGfx->fillScreen(kBg);

  gGfx->setTextSize(3);
  gGfx->setTextColor(kAccent, kBg);
  gGfx->setCursor(10, 8);
  gGfx->print("SlyTherm");
  gGfx->setTextSize(2);
  gGfx->setTextColor(kFg, kBg);
  gGfx->setCursor(148, 16);
  gGfx->print("CT-485 RX-only listener");
  gGfx->drawFastHLine(0, 74, 800, kAccent);

  gReady = true;
  Serial.println("display: SlyTherm RGB panel up (800x480)");
}

void update(const Stats& s) {
  if (!gReady) return;
  char buf[80];

  snprintf(buf, sizeof(buf), "%s  http://%s/",
           s.ap ? "SoftAP" : (s.wifiConnected ? "WiFi" : "WiFi(down)"), s.ip);
  row(0, "net : ", buf, s.wifiConnected ? kHi : kWarn);

  snprintf(buf, sizeof(buf), "%lu (%s)  9600:%lu  38400:%lu",
           static_cast<unsigned long>(s.baud), s.locked ? "locked" : "scanning",
           static_cast<unsigned long>(s.v9600),
           static_cast<unsigned long>(s.v38400));
  row(1, "baud: ", buf, s.locked ? kHi : kFg);

  snprintf(buf, sizeof(buf), "ok %lu  badcrc %lu  badlen %lu  ovr %lu",
           static_cast<unsigned long>(s.ok), static_cast<unsigned long>(s.badcrc),
           static_cast<unsigned long>(s.badlen),
           static_cast<unsigned long>(s.overrun));
  row(2, "frm : ", buf, s.ok ? kHi : kFg);

  snprintf(buf, sizeof(buf), "%lu bytes   cap %lu rec / %lu B",
           static_cast<unsigned long>(s.bytes),
           static_cast<unsigned long>(s.capRecords),
           static_cast<unsigned long>(s.capBytes));
  row(3, "rx  : ", buf, kFg);

  int pos = 0;
  buf[0] = '\0';
  for (int i = 0; i < s.topN; ++i) {
    pos += snprintf(buf + pos, sizeof(buf) - pos, "%s0x%02X:%lu",
                    i ? "  " : "", s.topType[i],
                    static_cast<unsigned long>(s.topCount[i]));
    if (pos >= static_cast<int>(sizeof(buf)) - 14) break;
  }
  if (s.topN == 0) snprintf(buf, sizeof(buf), "(none yet)");
  row(4, "type: ", buf, kFg);

  snprintf(buf, sizeof(buf), "up %lus   heap %lu B   rssi %d",
           static_cast<unsigned long>(s.uptimeMs / 1000),
           static_cast<unsigned long>(s.heap), s.rssi);
  row(5, "sys : ", buf, kFg);
}

}  // namespace display
