#include "remote_board_p4.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_ldo_regulator.h"

namespace remote_board {
namespace {

// ---- pins (JC4880P443C_I_W, confirmed via schreibfaul1/ESP32-MiniWebRadio
// issue #791 + cross-checked against the vendor ESPHome yaml) ----
constexpr int kLcdReset = 5;
constexpr int kBacklight = 23;
constexpr int kI2cSda = 7;
constexpr int kI2cScl = 8;
constexpr uint8_t kGt911Addr = 0x5D;  // no reset pin on this board (TP_IRQ unwired)

constexpr int kHRes = 480;
constexpr int kVRes = 800;

esp_lcd_panel_handle_t gPanel = nullptr;
bool gPanelOk = false;

// ---- GT911 raw I2C (same register protocol as the Controller's validated
// recipe, src/slytherm_ui.cpp -- this board has no CH422G, so no reset dance) ----
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
    down = false;
  }
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
lv_disp_draw_buf_t gDrawBuf;
lv_color_t gLineBuf[kHRes * 40];  // internal RAM, matches the Controller's recipe
lv_disp_drv_t gDispDrv;
lv_indev_drv_t gIndevDrv;

void flushCb(lv_disp_drv_t* drv, const lv_area_t* area, lv_color_t* px) {
  esp_lcd_panel_draw_bitmap(gPanel, area->x1, area->y1, area->x2 + 1, area->y2 + 1, px);
  lv_disp_flush_ready(drv);
}

void touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
  uint16_t x, y;
  if (gTouchOk && gtRead(x, y)) {
    data->state = LV_INDEV_STATE_PR;
    data->point.x = x;
    data->point.y = y;
    Serial.printf("[touch] x=%u y=%u\n", x, y);
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

// Unambiguous R/G/B/white full-screen cycle (#97's acceptance bar: "I see the
// colors") plus a dot that follows touch, so an owner glancing at the panel
// gets immediate visual confirmation of both flush and touch.
lv_obj_t* gTouchDot = nullptr;

void tickColorCycle() {
  static uint32_t lastMs = 0;
  static int idx = 0;
  const uint32_t nowMs = millis();
  if (nowMs - lastMs < 1500) return;
  lastMs = nowMs;
  static const lv_color_t kColors[] = {
      lv_color_hex(0xFF0000), lv_color_hex(0x00FF00), lv_color_hex(0x0000FF), lv_color_hex(0xFFFFFF)};
  lv_obj_set_style_bg_color(lv_scr_act(), kColors[idx], LV_PART_MAIN);
  idx = (idx + 1) % 4;
}

}  // namespace

void begin() {
  Serial.println("[remote_board] begin() entered");
  initBacklight();
  initPanel();

  Wire.begin(kI2cSda, kI2cScl, 400000U);
  uint8_t probe = 0;
  gTouchOk = gtReg(0x814E, &probe, 1) == 1;
  Serial.printf("[touch] GT911 %s\n", gTouchOk ? "present" : "NO ACK");

  lv_init();
  lv_disp_draw_buf_init(&gDrawBuf, gLineBuf, nullptr, kHRes * 40);
  lv_disp_drv_init(&gDispDrv);
  gDispDrv.hor_res = kHRes;
  gDispDrv.ver_res = kVRes;
  gDispDrv.flush_cb = flushCb;
  gDispDrv.draw_buf = &gDrawBuf;
  lv_disp_drv_register(&gDispDrv);

  lv_indev_drv_init(&gIndevDrv);
  gIndevDrv.type = LV_INDEV_TYPE_POINTER;
  gIndevDrv.read_cb = touchCb;
  lv_indev_drv_register(&gIndevDrv);

  gTouchDot = lv_obj_create(lv_scr_act());
  lv_obj_set_size(gTouchDot, 40, 40);
  lv_obj_set_style_radius(gTouchDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(gTouchDot, lv_color_hex(0x000000), LV_PART_MAIN);
  lv_obj_set_pos(gTouchDot, kHRes / 2 - 20, kVRes / 2 - 20);

  Serial.println("[panel] LVGL up");
}

void loop() {
  static uint32_t lastTickMs = 0;
  const uint32_t nowMs = millis();
  lv_tick_inc(nowMs - lastTickMs);
  lastTickMs = nowMs;
  lv_timer_handler();

  tickColorCycle();

  uint16_t x, y;
  if (gTouchOk && gtRead(x, y)) {
    lv_obj_set_pos(gTouchDot, x - 20, y - 20);
  }
}

bool panelOk() { return gPanelOk; }
bool touchOk() { return gTouchOk; }

}  // namespace remote_board
