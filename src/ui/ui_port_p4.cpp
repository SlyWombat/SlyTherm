// ui_port_p4.cpp — Remote board port (#114/#101): GUITION JC4880P443C_I_W
// (ESP32-P4, 4.3" 480x800 ST7701 over MIPI-DSI, GT911 touch @0x5D).
// Panel + touch bring-up moved verbatim from the #97-validated
// remote_board_p4.cpp; what's new here is the ui_port.h contract glue: the
// glass is PORTRAIT 480x800, so the LVGL display registers with sw_rotate +
// LV_DISP_ROT_90 to present the LOGICAL 800x480 landscape the shared screens
// were designed for, and the GT911's native portrait coordinates are mapped
// into that rotated space in touchCb.

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "ui_port.h"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_ldo_regulator.h"
#include "esp_heap_caps.h"

#ifdef SLYTHERM_CAM
#include "remote_camera.h"  // #150 AE follow-up: shared-I2C lock (AE task vs this touch poll)
#endif

extern "C" void uiNoteTouch();   // #90 sleep-state touch note (press edge)

namespace slytherm_ui {
namespace {

// ---- pins (JC4880P443C_I_W, confirmed via schreibfaul1/ESP32-MiniWebRadio
// issue #791 + cross-checked against the vendor ESPHome yaml) ----
constexpr int kLcdReset = 5;
constexpr int kBacklight = 23;
constexpr int kI2cSda = 7;
constexpr int kI2cScl = 8;
constexpr uint8_t kGt911Addr = 0x5D;  // no reset pin on this board (TP_IRQ unwired)

constexpr int kHRes = 480;   // native (portrait) — logical UI is 800x480 via ROT_90
constexpr int kVRes = 800;

esp_lcd_panel_handle_t gPanel = nullptr;
bool gPanelOk = false;

// ---- GT911 raw I2C (same register protocol as the Controller's validated
// recipe, ui_port_s3.cpp -- this board has no CH422G, so no reset dance) ----
bool gTouchOk = false;

int gtReg(uint16_t reg, uint8_t* buf, uint8_t n) {
  Wire.beginTransmission(kGt911Addr);
  Wire.write(reg >> 8);
  Wire.write(reg & 0xFF);
  if (Wire.endTransmission(false) != 0) return 0;
  uint8_t got = Wire.requestFrom(kGt911Addr, n), i = 0;
  while (i < got && Wire.available()) buf[i++] = Wire.read();
  return i;
}

bool gtRead(uint16_t& x, uint16_t& y) {
  static uint16_t lx = 0, ly = 0;
  static bool down = false;
  static uint32_t readyMs = 0;
  uint8_t status = 0;
#ifdef SLYTHERM_CAM
  // #150 AE follow-up: the camera's 2 Hz AE task writes sensor registers on
  // this same Wire bus at runtime — serialize every touch-poll transaction
  // against it. (Boot-time Wire users don't need this: the camera tasks
  // don't exist yet when the port initializes.)
  remote_camera::wireLock();
#endif
  if (gtReg(0x814E, &status, 1) == 1 && (status & 0x80)) {
    readyMs = millis();
    if ((status & 0x0F) > 0) {
      uint8_t d[6] = {0};
      if (gtReg(0x8150, d, 6) >= 4) {
        lx = d[0] | (d[1] << 8);
        ly = d[2] | (d[3] << 8);
        down = true;
      }
    } else {
      down = false;
    }
    Wire.beginTransmission(kGt911Addr);
    Wire.write(0x81);
    Wire.write(0x4E);
    Wire.write(0);
    Wire.endTransmission();
  } else if (down && millis() - readyMs > 150) {
    down = false;  // no fresh GT911 sample -> released (frees LVGL idle timer)
  }
#ifdef SLYTHERM_CAM
  remote_camera::wireUnlock();
#endif
  x = lx;
  y = ly;
  return down;
}

// ---- ST7701 panel bring-up over MIPI-DSI ----
// Init sequence lifted verbatim from a hardware-validated driver for this
// exact SKU (schreibfaul1/ESP32-MiniWebRadio, lib/tftLib/tft_dsi.cpp,
// TFT_CONTROLLER==10 branch), cross-checked byte-for-byte against a second,
// independent source for the same board (conghuy93/4.3p4, a proper ESP-IDF
// board driver using the official esp_lcd_st7701 vendor component) --
// identical. Timing uses the NOMINAL vsync values (2/8/166): the
// MiniWebRadio driver's dpi_cfg population reads vsync_pulse_width/
// vsync_back_porch from the wrong (hsync) struct fields, a bug in that
// specific codebase -- conghuy93/4.3p4's clean board driver uses 2/8/166
// directly, matching the Timing struct's own documented intent, so that's
// what's used here.
void resetPanel() {
  pinMode(kLcdReset, OUTPUT);
  digitalWrite(kLcdReset, HIGH);
  delay(20);
  digitalWrite(kLcdReset, LOW);
  delay(20);
  digitalWrite(kLcdReset, HIGH);
  delay(100);
}

void initBacklight() {
  ledcAttach(kBacklight, 5000, 8);
  ledcWrite(kBacklight, 200);  // visible early -- a dark panel must never look "off"
}

void initPanel() {
  esp_ldo_channel_handle_t ldoMipiPhy = nullptr;
  esp_ldo_channel_config_t ldoCfg = {.chan_id = 3, .voltage_mv = 2500};
  if (esp_ldo_acquire_channel(&ldoCfg, &ldoMipiPhy) != ESP_OK) {
    Serial.println("[panel] LDO acquire failed");
  }

  resetPanel();

  esp_lcd_dsi_bus_handle_t dsiBus = nullptr;
  esp_lcd_dsi_bus_config_t dsiCfg = {
      .bus_id = 0,
      .num_data_lanes = 2,
      .phy_clk_src = MIPI_DSI_PHY_PLLREF_CLK_SRC_DEFAULT_LEGACY,
      .lane_bit_rate_mbps = 500,
  };
  if (esp_lcd_new_dsi_bus(&dsiCfg, &dsiBus) != ESP_OK) {
    Serial.println("[panel] DSI bus create failed");
  }

  esp_lcd_panel_io_handle_t dbiIo = nullptr;
  esp_lcd_dbi_io_config_t dbiCfg = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
  if (esp_lcd_new_panel_io_dbi(dsiBus, &dbiCfg, &dbiIo) != ESP_OK) {
    Serial.println("[panel] DBI IO create failed");
  }

  esp_lcd_panel_io_tx_param(dbiIo, 0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x13}, 5);
  esp_lcd_panel_io_tx_param(dbiIo, 0xEF, (uint8_t[]){0x08}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x10}, 5);
  esp_lcd_panel_io_tx_param(dbiIo, 0xC0, (uint8_t[]){0x63, 0x00}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xC1, (uint8_t[]){0x0D, 0x02}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xC2, (uint8_t[]){0x10, 0x08}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xCC, (uint8_t[]){0x10}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB0,
      (uint8_t[]){0x80, 0x09, 0x53, 0x0C, 0xD0, 0x07, 0x0C, 0x09, 0x09, 0x28, 0x06, 0xD4, 0x13, 0x69, 0x2B, 0x71}, 16);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB1,
      (uint8_t[]){0x80, 0x94, 0x5A, 0x10, 0xD3, 0x06, 0x0A, 0x08, 0x08, 0x25, 0x03, 0xD3, 0x12, 0x66, 0x6A, 0x0D}, 16);
  esp_lcd_panel_io_tx_param(dbiIo, 0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x11}, 5);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB0, (uint8_t[]){0x5D}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB1, (uint8_t[]){0x58}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB2, (uint8_t[]){0x87}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB3, (uint8_t[]){0x80}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB5, (uint8_t[]){0x4E}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB7, (uint8_t[]){0x85}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB8, (uint8_t[]){0x21}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xB9, (uint8_t[]){0x10, 0x1F}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xBB, (uint8_t[]){0x03}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xBC, (uint8_t[]){0x00}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xC1, (uint8_t[]){0x78}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xC2, (uint8_t[]){0x78}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xD0, (uint8_t[]){0x88}, 1);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE0, (uint8_t[]){0x00, 0x3A, 0x02}, 3);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE1,
      (uint8_t[]){0x04, 0xA0, 0x00, 0xA0, 0x05, 0xA0, 0x00, 0xA0, 0x00, 0x40, 0x40}, 11);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE2,
      (uint8_t[]){0x30, 0x00, 0x40, 0x40, 0x32, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00, 0xA0, 0x00}, 13);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE3, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE4, (uint8_t[]){0x44, 0x44}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE5,
      (uint8_t[]){0x09, 0x2E, 0xA0, 0xA0, 0x0B, 0x30, 0xA0, 0xA0, 0x05, 0x2A, 0xA0, 0xA0, 0x07, 0x2C, 0xA0, 0xA0}, 16);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE6, (uint8_t[]){0x00, 0x00, 0x33, 0x33}, 4);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE7, (uint8_t[]){0x44, 0x44}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xE8,
      (uint8_t[]){0x08, 0x2D, 0xA0, 0xA0, 0x0A, 0x2F, 0xA0, 0xA0, 0x04, 0x29, 0xA0, 0xA0, 0x06, 0x2B, 0xA0, 0xA0}, 16);
  esp_lcd_panel_io_tx_param(dbiIo, 0xEB, (uint8_t[]){0x00, 0x00, 0x4E, 0x4E, 0x00, 0x00, 0x00}, 7);
  esp_lcd_panel_io_tx_param(dbiIo, 0xEC, (uint8_t[]){0x08, 0x01}, 2);
  esp_lcd_panel_io_tx_param(dbiIo, 0xED,
      (uint8_t[]){0xB0, 0x2B, 0x98, 0xA4, 0x56, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x65, 0x4A, 0x89, 0xB2, 0x0B}, 16);
  esp_lcd_panel_io_tx_param(dbiIo, 0xEF, (uint8_t[]){0x08, 0x08, 0x08, 0x45, 0x3F, 0x54}, 6);
  esp_lcd_panel_io_tx_param(dbiIo, 0xFF, (uint8_t[]){0x77, 0x01, 0x00, 0x00, 0x00}, 5);
  esp_lcd_panel_io_tx_param(dbiIo, 0x11, (uint8_t[]){0x00}, 1);  // sleep out
  delay(120);
  esp_lcd_panel_io_tx_param(dbiIo, 0x29, (uint8_t[]){0x00}, 1);  // display on
  delay(200);

  esp_lcd_dpi_panel_config_t dpiCfg = {
      .virtual_channel = 0,
      .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
      .dpi_clock_freq_mhz = 34,
      .pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565,
      .in_color_format = LCD_COLOR_FMT_RGB565,
      .out_color_format = LCD_COLOR_FMT_RGB565,
      .num_fbs = 3,
      .video_timing =
          {
              .h_size = kHRes,
              .v_size = kVRes,
              .hsync_pulse_width = 12,
              .hsync_back_porch = 42,
              .hsync_front_porch = 42,
              .vsync_pulse_width = 2,
              .vsync_back_porch = 8,
              .vsync_front_porch = 166,
          },
      .flags = {.use_dma2d = true, .disable_lp = false},
  };
  if (esp_lcd_new_panel_dpi(dsiBus, &dpiCfg, &gPanel) != ESP_OK) {
    Serial.println("[panel] DPI panel create failed");
  }
  if (esp_lcd_panel_init(gPanel) != ESP_OK) {
    Serial.println("[panel] init failed");
    gPanelOk = false;
  } else {
    Serial.println("[panel] ST7701 initialized");
    gPanelOk = true;
  }
}

// ---- LVGL glue ----
// Registered NATIVE 480x800 with rotated=LV_DISP_ROT_90 but sw_rotate=0:
// the shared screens see the logical 800x480 landscape, and THIS driver owns
// the rotation. LVGL's own sw_rotate path was tried and rejected — it draws
// its chunk buffer from the LVGL pool, which lives in PSRAM on this board,
// and the pixel-by-pixel rotate into PSRAM made repaints slow enough to
// starve touch sampling (quick taps fell between indev reads). Rotating
// here goes SRAM draw buffer -> static SRAM bounce buffer -> one DSI DMA.
lv_disp_draw_buf_t gDrawBuf;
// gLineBuf (the LVGL single draw buffer, 37.5KB) is heap-allocated in portInit
// (internal RAM by default; PSRAM ONLY on the camera/VPN build, see there): it
// is NEVER handed to DMA — LVGL renders into it and flushCb reads it
// back sequentially as `px` — so PSRAM is safe (no cache-writeback hazard) and
// the sequential reads stay cache-friendly. This frees ~37.5KB of internal
// DMA-capable RAM that the P4<->C6 hosted-WiFi SDIO RX drainer (lwIP tcpip +
// WireGuard decrypt) needs under heavy INBOUND load; when that heap craters the
// RX pool depletes and the driver asserts (sdio_rx_get_buffer sdio_drv.c:953).
// Allocated in portInit(). gRotBuf STAYS internal on purpose: it is the live DMA
// SOURCE for esp_lcd_panel_draw_bitmap and takes strided CPU writes — in PSRAM
// the DPI blit could read stale (un-written-back) cache lines and it would slow
// repaints (the rejected sw_rotate-into-PSRAM pattern, gotcha #1).
lv_color_t* gLineBuf = nullptr;
lv_color_t gRotBuf[kHRes * 40];    // internal RAM rotation bounce (DMA source — keep internal)
lv_disp_drv_t gDispDrv;
lv_indev_drv_t gIndevDrv;

// The DPI panel's draw_bitmap is ASYNC (DMA2D copy into the framebuffer);
// signalling flush_ready immediately let LVGL start the next chunk while the
// previous copy was in flight -> "previous draw operation is not finished"
// errors + dropped draws under the full UI's flush rate. And signalling it
// from the done-ISR alone fed LVGL's sw-rotate busy-wait
// (while(draw_buf->flushing)) which starves IDLE0 -> task WDT abort whenever
// a done event goes missing. So: flush SYNCHRONOUSLY — block on a semaphore
// the done-ISR gives, with a bounded timeout as the lost-event escape hatch.
SemaphoreHandle_t gFlushDone = nullptr;

bool drawDoneCb(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
  BaseType_t hp = pdFALSE;
  xSemaphoreGiveFromISR(gFlushDone, &hp);
  return hp == pdTRUE;
}

void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  // `area` arrives in LOGICAL 800x480 coords (sw_rotate=0 + rotated=ROT_90
  // means the driver rotates). Mapping (matches LVGL's own ROT_90 flush and
  // the pointer transform lv_indev applies): logical(x,y) -> native(y, 799-x).
  const int w = area->x2 - area->x1 + 1, h = area->y2 - area->y1 + 1;
  for (int ly = 0; ly < h; ly++) {
    const lv_color_t* src = px + (size_t)ly * w;
    for (int lx = 0; lx < w; lx++) gRotBuf[(size_t)(w - 1 - lx) * h + ly] = src[lx];
  }
  xSemaphoreTake(gFlushDone, 0);  // drain a stale give from a timed-out flush
  esp_lcd_panel_draw_bitmap(gPanel, area->y1, (kVRes - 1) - area->x2,
                            area->y2 + 1, kVRes - area->x1, gRotBuf);
  xSemaphoreTake(gFlushDone, pdMS_TO_TICKS(100));  // done-ISR, or bounded bail-out
  lv_disp_flush_ready(drv);
}

void touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  static bool wasDown = false;
  if (gTouchOk && gtRead(x, y)) {
    data->state = LV_INDEV_STATE_PR;
    // RAW native portrait coords, on purpose: LVGL v8 rotates pointer input
    // itself when drv.rotated is set (lv_indev.c indev_pointer_proc applies
    // the ROT_90 inverse) — transforming here too would double-rotate.
    data->point.x = x;
    data->point.y = y;
    if (!wasDown) { wasDown = true; uiNoteTouch(); }   // #90: press edge -> sleep-state wake
  } else {
    data->state = LV_INDEV_STATE_REL;
    wasDown = false;
  }
}

}  // namespace

bool portInit() {
  gFlushDone = xSemaphoreCreateBinary();
  initBacklight();
  initPanel();

  Wire.begin(kI2cSda, kI2cScl, 400000U);
  uint8_t probe = 0;
  gTouchOk = gtReg(0x814E, &probe, 1) == 1;
  Serial.printf("[ui] GT911 %s\n", gTouchOk ? "present" : "NO ACK");

  lv_init();
  // Draw-buffer placement is build-gated. ONLY the camera/VPN build
  // (SLYTHERM_WG/SLYTHERM_CAM) puts gLineBuf in PSRAM — that frees ~37.5KB of
  // internal DMA RAM for the P4<->C6 SDIO RX drainer, curing the
  // sdio_rx_get_buffer assert under heavy inbound-over-WireGuard load. Plain P4
  // wall remotes have no such network load and no SDIO-starvation problem, so
  // they keep the buffer in FAST internal RAM (PSRAM would only slow repaints).
#if defined(SLYTHERM_WG) || defined(SLYTHERM_CAM)
  // Fall back to internal if PSRAM is somehow unavailable so the UI still comes up.
  gLineBuf = (lv_color_t*)heap_caps_malloc(sizeof(lv_color_t) * kHRes * 40,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gLineBuf) {
    Serial.println("[ui] WARN: gLineBuf PSRAM alloc failed, using internal");
    gLineBuf = (lv_color_t*)heap_caps_malloc(sizeof(lv_color_t) * kHRes * 40,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
#else
  gLineBuf = (lv_color_t*)heap_caps_malloc(sizeof(lv_color_t) * kHRes * 40,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
  lv_disp_draw_buf_init(&gDrawBuf, gLineBuf, nullptr, kHRes * 40);
  lv_disp_drv_init(&gDispDrv);
  gDispDrv.hor_res = kHRes;
  gDispDrv.ver_res = kVRes;
  gDispDrv.flush_cb = flushCb;
  gDispDrv.draw_buf = &gDrawBuf;
  gDispDrv.sw_rotate = 0;              // rotation handled in flushCb (SRAM bounce)
  gDispDrv.rotated = LV_DISP_ROT_90;   // portrait glass -> logical 800x480 landscape
  lv_disp_drv_register(&gDispDrv);
  esp_lcd_dpi_panel_event_callbacks_t cbs = {};
  cbs.on_color_trans_done = drawDoneCb;
  esp_lcd_dpi_panel_register_event_callbacks(gPanel, &cbs, &gDispDrv);

  lv_indev_drv_init(&gIndevDrv);
  gIndevDrv.type = LV_INDEV_TYPE_POINTER;
  gIndevDrv.read_cb = touchCb;
  lv_indev_drv_register(&gIndevDrv);

  Serial.printf("[ui] panel %s, LVGL up (logical %dx%d)\n",
                gPanelOk ? "OK" : "FAIL", (int)lv_disp_get_hor_res(nullptr),
                (int)lv_disp_get_ver_res(nullptr));
  return gPanelOk;
}

void portBacklight(bool on) { ledcWrite(kBacklight, on ? 200 : 0); }

bool portTouchOk() { return gTouchOk; }

}  // namespace slytherm_ui
