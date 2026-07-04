// main_ui_smoke.cpp — Stage 1 of the SlyTherm wall UI (issue #37): prove the
// LVGL + RGB-panel flush + GT911 touch stack on the Waveshare ESP32-S3-4.3B
// BEFORE any real screens (advisor: de-risk the buffer/touch crux first).
// Throwaway once the ui/lvgl binding lands; env:ui_smoke_s3.
//
// Display recipe (proven): LVGL v8, single 800x40 PARTIAL line buffer in
// INTERNAL RAM (a PSRAM buffer tears against the panel's framebuffer DMA),
// flushed via Arduino_GFX draw16bitRGBBitmap. Panel timing matches the board's
// Arduino_GFX reference exactly.
//
// Touch: raw GT911 over I2C. The GT911 RESET is on the CH422G expander (EXIO1),
// and its I2C address latches from the INT pin (GPIO4) level at reset release —
// so we drive that sequence by hand (bb_captouch can't, it only knows GPIOs),
// then poll the GT911 status/point registers directly at 0x5D.

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>

// ---- CH422G (raw I2C): backlight + LCD/touch reset --------------------------
static constexpr uint8_t kCh422ModeAddr = 0x24, kCh422OutAddr = 0x38;
static constexpr uint8_t kBitTpRst = 1 << 1, kBitLcdBl = 1 << 2, kBitLcdRst = 1 << 3;
static uint8_t gCh422Out = 0;
static void ch422Mode(uint8_t v) {
  Wire.beginTransmission(kCh422ModeAddr); Wire.write(v); Wire.endTransmission();
}
static void ch422SetOut(uint8_t v) {
  gCh422Out = v;
  Wire.beginTransmission(kCh422OutAddr); Wire.write(v); Wire.endTransmission();
}

// ---- RGB panel (proven pins/timing) ----------------------------------------
static Arduino_ESP32RGBPanel* panel = nullptr;
static Arduino_RGB_Display*   gfx   = nullptr;

// ---- GT911 raw driver ------------------------------------------------------
static constexpr int     kSda = 8, kScl = 9, kTouchInt = 4;
static constexpr uint8_t kGt911Addr = 0x5D;   // INT low at reset release -> 0x5D

// Reset the GT911 for address 0x5D: RESET low (via CH422G TP_RST), INT low,
// release RESET (chip samples INT = low), then free INT for polling.
static void gt911Reset() {
  pinMode(kTouchInt, OUTPUT);
  digitalWrite(kTouchInt, LOW);
  ch422SetOut(gCh422Out & ~kBitTpRst);          // TP_RST low (assert reset)
  delay(10);
  digitalWrite(kTouchInt, LOW);                 // INT low selects 0x5D
  delayMicroseconds(120);
  ch422SetOut(gCh422Out | kBitTpRst);           // release reset; INT latched low
  delay(5);
  digitalWrite(kTouchInt, LOW);
  delay(50);
  pinMode(kTouchInt, INPUT);                     // release INT for operation
  delay(50);
}

// Read a GT911 16-bit register block with a repeated-START (endTransmission
// false) — the address pointer must not be released by a STOP before the read.
static int gt911ReadReg(uint16_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(kGt911Addr);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) return 0;   // repeated start, no STOP
  const uint8_t got = Wire.requestFrom(kGt911Addr, n);
  uint8_t i = 0;
  while (i < got && Wire.available()) buf[i++] = Wire.read();
  return i;
}

static bool gt911Read(uint16_t& x, uint16_t& y) {
  uint8_t status = 0;
  if (gt911ReadReg(0x814E, &status, 1) != 1) return false;
  bool hit = false;
  if ((status & 0x80) && (status & 0x0F) > 0) {
    uint8_t d[6] = {0};
    // 0x8150 starts point 0 coordinates directly (track id is at 0x814F):
    // d = [xLow, xHigh, yLow, yHigh, sizeLow, sizeHigh].
    if (gt911ReadReg(0x8150, d, 6) >= 4) {
      x = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
      y = (uint16_t)d[2] | ((uint16_t)d[3] << 8);
      hit = true;
    }
  }
  if (status & 0x80) {  // clear ready flag for the next sample
    Wire.beginTransmission(kGt911Addr);
    Wire.write(0x81); Wire.write(0x4E); Wire.write(0x00);
    Wire.endTransmission();
  }
  return hit;
}

// ---- LVGL ------------------------------------------------------------------
static constexpr uint16_t kHor = 800, kVer = 480;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t         buf1[kHor * 40];   // partial line buffer, INTERNAL RAM
static lv_disp_drv_t      disp_drv;
static lv_indev_drv_t     indev_drv;
static lv_obj_t*          touchLabel = nullptr;
static lv_obj_t*          touchDot   = nullptr;
static uint32_t           touchCount = 0;
static uint32_t           gLastTouchLogMs = 0;
static bool               gGt911Ok = false;

static void flushCb(lv_disp_drv_t* d, const lv_area_t* area, lv_color_t* px) {
  const uint32_t w = area->x2 - area->x1 + 1;
  const uint32_t h = area->y2 - area->y1 + 1;
  gfx->draw16bitRGBBitmap(area->x1, area->y1, reinterpret_cast<uint16_t*>(&px->full),
                          w, h);
  lv_disp_flush_ready(d);
}

static void touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  if (gGt911Ok && gt911Read(x, y)) {
    data->state   = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    const uint32_t nowMs = millis();
    if (nowMs - gLastTouchLogMs > 120) {
      gLastTouchLogMs = nowMs;
      Serial.printf("[ui_smoke] touch x=%u y=%u\n", x, y);
    }
    if (touchDot) {
      lv_obj_clear_flag(touchDot, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_pos(touchDot, (int)x - 12, (int)y - 12);
    }
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

static void onBtn(lv_event_t* e) {
  if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
  ++touchCount;
  lv_label_set_text_fmt(touchLabel, "button clicks: %lu", (unsigned long)touchCount);
  Serial.printf("[ui_smoke] button click #%lu\n", (unsigned long)touchCount);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("[ui_smoke] SlyTherm LVGL stage-1 bring-up");

  Wire.begin(kSda, kScl);
  Wire.setClock(400000);
  ch422Mode(0x01);                                   // EXIO push-pull outputs
  ch422SetOut(kBitTpRst | kBitLcdRst);               // release resets, backlight off
  delay(20);

  panel = new Arduino_ESP32RGBPanel(
      5, 3, 46, 7, 1, 2, 42, 41, 40, 39, 0, 45, 48, 47, 21, 14, 38, 18, 17, 10,
      0, 40, 48, 88, 0, 13, 3, 32, 1, 16000000);
  gfx = new Arduino_RGB_Display(kHor, kVer, panel, 0, true);
  if (!gfx->begin()) Serial.println("[ui_smoke] gfx begin FAILED");
  gfx->fillScreen(BLACK);
  ch422SetOut(kBitTpRst | kBitLcdRst | kBitLcdBl);   // backlight on

  // GT911: reset for 0x5D, then probe.
  gt911Reset();
  Wire.beginTransmission(kGt911Addr);
  gGt911Ok = (Wire.endTransmission() == 0);
  Serial.printf("[ui_smoke] GT911 @0x5D %s\n", gGt911Ok ? "ACK (present)" : "NO ACK");

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, kHor * 40);
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = kHor;
  disp_drv.ver_res  = kVer;
  disp_drv.flush_cb = flushCb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touchCb;
  lv_indev_drv_register(&indev_drv);

  lv_obj_t* scr = lv_scr_act();
  lv_obj_set_style_bg_color(scr, lv_color_hex(0x0A0E13), 0);

  lv_obj_t* frame = lv_obj_create(scr);
  lv_obj_set_size(frame, 800, 480);
  lv_obj_set_pos(frame, 0, 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(frame, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_border_width(frame, 4, 0);
  lv_obj_set_style_radius(frame, 0, 0);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);

  touchDot = lv_obj_create(scr);
  lv_obj_set_size(touchDot, 24, 24);
  lv_obj_set_style_radius(touchDot, 12, 0);
  lv_obj_set_style_bg_color(touchDot, lv_color_hex(0xFF7A18), 0);
  lv_obj_set_style_border_width(touchDot, 0, 0);
  lv_obj_add_flag(touchDot, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "SlyTherm  -  LVGL OK");
  lv_obj_set_style_text_color(title, lv_color_hex(0x38BDF8), 0);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

  lv_obj_t* btn = lv_btn_create(scr);
  lv_obj_set_size(btn, 260, 90);
  lv_obj_align(btn, LV_ALIGN_CENTER, 0, -10);
  lv_obj_add_event_cb(btn, onBtn, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* bl = lv_label_create(btn);
  lv_label_set_text(bl, "TAP ME");
  lv_obj_center(bl);

  touchLabel = lv_label_create(scr);
  lv_label_set_text(touchLabel, gGt911Ok ? "touch ready - drag a finger"
                                         : "GT911 not detected");
  lv_obj_set_style_text_color(touchLabel, lv_color_hex(0xC5C2C5), 0);
  lv_obj_align(touchLabel, LV_ALIGN_CENTER, 0, 90);

  Serial.println("[ui_smoke] LVGL up; drag a finger / tap the button");
}

void loop() {
  static uint32_t last = 0;
  const uint32_t now = millis();
  lv_tick_inc(now - last);
  last = now;
  lv_timer_handler();
  delay(5);
}
