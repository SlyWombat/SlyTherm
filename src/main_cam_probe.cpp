// main_cam_probe.cpp — #150 camera bring-up probe (env:remote_p4_cam_probe).
// Flash, read serial, reflash the real firmware. Proves the OV02C10 → CSI →
// memory path: SCCB init over the shared GPIO7/8 bus, MIPI PHY LDO, CSI
// controller capture, then frame-rate + brightness stats (real photons vs
// zeros). No ISP/JPEG yet — RAW8 straight to PSRAM.

#include <Arduino.h>
#include <Wire.h>

#include "driver/i2s_std.h"
#include "driver/isp.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"
#if __has_include("esp_cache.h")
#include "esp_cache.h"
#define CAMP_HAS_CACHE 1
#endif

#include "cam_probe_regs.h"

namespace {

constexpr int kSda = 7, kScl = 8;   // panel/external bus (GT911 + ES8311 + cam)
constexpr uint8_t kAddr = 0x36;     // OV02C10 SCCB (PID 0x5602)
constexpr int kW = 1920, kH = 1080; // RAW10 2-lane table below
constexpr int kXclkPin = 13;        // CODEC_I2S0_MCLK — shared codec+camera MCLK
constexpr size_t kFrameBytes = ((size_t)kW * kH * 10 / 8 + 127) & ~(size_t)127;
constexpr int kMipiLdoChan = 3;     // same PHY rail the DSI panel uses
constexpr int kMipiLdoMv = 2500;

uint8_t* gBuf = nullptr;
volatile uint32_t gFrames = 0;

bool wr(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(val);
  return Wire.endTransmission() == 0;
}
bool rd(uint16_t reg, uint8_t* out) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  // SCCB wants a STOP between the address phase and the read (no repeated
  // start) — with repeated start the low byte reads back garbage.
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)kAddr, 1) != 1) return false;
  *out = Wire.read(); return true;
}
bool writeTable(const ov02c10_reginfo_t* t) {
  for (; t->reg != OV02C10_REG_END; t++) {
    if (t->reg == OV02C10_REG_DELAY) { delay(t->val); continue; }
    if (!wr(t->reg, t->val)) {
      Serial.printf("SCCB write FAILED at 0x%04X\n", t->reg);
      return false;
    }
  }
  return true;
}

bool IRAM_ATTR onGetNewTrans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* t, void*) {
  t->buffer = gBuf;
  t->buflen = kFrameBytes;
  return false;
}
bool IRAM_ATTR onTransFinished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t*, void*) {
  gFrames = gFrames + 1;
  return false;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("\n=== SlyTherm camera probe (#150, OV02C10 RAW10 2-lane 1920x1080) ===");

  // XVCLK first: 24 MHz on GPIO13 = I2S0_MCLK, the line the board shares
  // between the ES8311 codec and the camera. Generate it with the I2S
  // peripheral (LEDC can't do 24 MHz): 62500 Hz x MCLK_MULTIPLE_384 is
  // exactly 24,000,000. TX channel with no data pins — MCLK only.
  {
    i2s_chan_handle_t tx = nullptr;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_std_config_t sc = {};
    sc.clk_cfg.sample_rate_hz = 46875;  // x512 = exactly 24 MHz (16-bit slots need 256/512)
    sc.clk_cfg.clk_src = I2S_CLK_SRC_APLL;  // XTAL(40M) can't make 24M; PLL_160M needs hw_ver3 silicon (boot-loops on ours)
    sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
    sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO);
    sc.gpio_cfg.mclk = (gpio_num_t)kXclkPin;
    sc.gpio_cfg.bclk = GPIO_NUM_12;  // ES8311 BCLK (idle clocking, harmless)
    sc.gpio_cfg.ws = GPIO_NUM_10;    // ES8311 WS
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din = I2S_GPIO_UNUSED;
    esp_err_t e1 = i2s_new_channel(&cc, &tx, nullptr);
    esp_err_t e2 = (e1 == ESP_OK) ? i2s_channel_init_std_mode(tx, &sc) : ESP_FAIL;
    esp_err_t e3 = (e2 == ESP_OK) ? i2s_channel_enable(tx) : ESP_FAIL;
    if (e3 == ESP_OK)
      Serial.println("XCLK 24 MHz on GPIO13 (I2S0 MCLK, 46875x512)");
    else
      Serial.printf("XCLK via I2S FAILED: new=0x%x init=0x%x en=0x%x\n", e1, e2, e3);
  }
  delay(5);

  Wire.begin(kSda, kScl, 400000U);

  // 1. Sensor ID (SCCB sanity)
  uint8_t idh = 0, idl = 0;
  bool ok = rd(0x300A, &idh) && rd(0x300B, &idl);
  Serial.printf("sensor id: %s %02X%02X (OV02C10 wants 5602)\n", ok ? "read" : "READ-FAIL", idh, idl);

  // 2. MIPI PHY rail (shared LDO channel; refcounted with any DSI user)
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t lcfg = {};
  lcfg.chan_id = kMipiLdoChan;
  lcfg.voltage_mv = kMipiLdoMv;
  if (esp_ldo_acquire_channel(&lcfg, &ldo) != ESP_OK) {
    Serial.println("LDO acquire FAILED"); return;
  }
  Serial.println("MIPI PHY LDO up (chan 3 @ 2.5V)");

  // 3. Frame buffer (PSRAM, cache-line aligned)
  gBuf = (uint8_t*)heap_caps_aligned_calloc(128, 1, kFrameBytes,  // P4 cache line = 128B
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!gBuf) { Serial.println("PSRAM alloc FAILED"); return; }

  // 4. CSI controller
  esp_cam_ctlr_csi_config_t ccfg = {};
  ccfg.ctlr_id = 0;
  ccfg.clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
  ccfg.h_res = kW;
  ccfg.v_res = kH;
  ccfg.data_lane_num = 2;
  ccfg.lane_bit_rate_mbps = (int)(OV02C10_MIPI_CSI_LINE_RATE_1920x1080_30FPS / 1000000ULL);
  ccfg.input_data_color_type = CAM_CTLR_COLOR_RAW10;
  ccfg.output_data_color_type = CAM_CTLR_COLOR_RAW10;
  ccfg.queue_items = 1;
  ccfg.bk_buffer_dis = 1;  // driver's internal fb is 0x11E270 B (not 128-aligned) -> msync error; ours is padded
  esp_cam_ctlr_handle_t cam = nullptr;
  esp_err_t e = esp_cam_new_csi_ctlr(&ccfg, &cam);
  if (e != ESP_OK) { Serial.printf("csi ctlr FAILED: 0x%x\n", e); return; }
  // The P4's CSI data path runs THROUGH the ISP block even for RAW: without
  // an ISP processor programmed (bypass + frame geometry), the bridge never
  // delivers a byte to DMA. esp_video does this unconditionally; so must we.
  esp_isp_processor_cfg_t icfg = {};
  icfg.clk_src = ISP_CLK_SRC_DEFAULT;
  icfg.clk_hz = 160000000;  // 240M unsupported on this silicon's default src
  icfg.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
  icfg.input_data_color_type = ISP_COLOR_RAW10;
  icfg.output_data_color_type = ISP_COLOR_RAW10;
  icfg.h_res = kW;
  icfg.v_res = kH;
  icfg.bayer_order = COLOR_RAW_ELEMENT_ORDER_BGGR;
  icfg.flags.bypass_isp = 1;
  isp_proc_handle_t isp = nullptr;
  esp_err_t ei = esp_isp_new_processor(&icfg, &isp);
  if (ei != ESP_OK) { Serial.printf("ISP new FAILED: 0x%x\n", ei); return; }
  Serial.println("ISP processor up (bypass mode)");
  // NOTE: no esp_isp_enable() in bypass — mirror esp_video's raw path.

  esp_cam_ctlr_evt_cbs_t cbs = {};
  cbs.on_get_new_trans = onGetNewTrans;
  cbs.on_trans_finished = onTransFinished;
  esp_cam_ctlr_register_event_callbacks(cam, &cbs, nullptr);
  if (esp_cam_ctlr_enable(cam) != ESP_OK || esp_cam_ctlr_start(cam) != ESP_OK) {
    Serial.println("csi enable/start FAILED"); return;
  }
  Serial.printf("CSI up: %dx%d RAW10, 2 lanes @ %d Mbps\n", kW, kH,
                ccfg.lane_bit_rate_mbps);

  // 5. Sensor init + stream on
  if (!writeTable(ov02c10_mipi_reset_regs)) return;
  if (!writeTable(ov02c10_input_24M_MIPI_2lane_raw10_1920x1080_30fps)) return;
  // Vendor table gates the MIPI clock lane between frames (0x4800=0x64);
  // force CONTINUOUS clock (clear BIT5) — several CSI hosts need it to lock.
  if (!wr(0x4800, 0x44)) return;
  if (!wr(0x0100, 0x01)) { Serial.println("stream-on FAILED"); return; }
  Serial.println("sensor init written, streaming requested");
  delay(50);
  uint8_t v1 = 0xEE, v2 = 0xEE, v3 = 0xEE;
  rd(0x0100, &v1);   // stream bit: want 0x01
  rd(0x3808, &v2);   // format-table reg — readback proves writes are landing
  rd(0x4800, &v3);   // MIPI CTRL
  Serial.printf("readback: 0x0100=%02X 0x3086=%02X 0x4800=%02X\n", v1, v2, v3);
}

void loop() {
  static uint32_t last = 0;
  delay(5000);
  const uint32_t f = gFrames;
  Serial.printf("frames: %lu total (+%lu in 5s = %.1f fps)\n",
                (unsigned long)f, (unsigned long)(f - last), (f - last) / 5.0);
  last = f;
  if (f && gBuf) {
#ifdef CAMP_HAS_CACHE
    esp_cache_msync(gBuf, kFrameBytes,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
#endif
    uint32_t sum = 0, nz = 0;
    for (int i = 0; i < 4096; i++) {
      const uint8_t v = gBuf[i * (kFrameBytes / 4096)];
      sum += v; if (v) nz++;
    }
    Serial.printf("  sample: avg=%lu/255 nonzero=%lu/4096 first16:",
                  (unsigned long)(sum / 4096), (unsigned long)nz);
    for (int i = 0; i < 16; i++) Serial.printf(" %02X", gBuf[i]);
    Serial.println();
  }
}
