// remote_camera.cpp — #150 tier 2: OV02C10 → CSI → ISP(RGB565) → hardware
// JPEG → HTTP MJPEG/snapshot server on :8080. See remote_camera.h for the
// privacy model. Capture recipe is the PROVEN main_cam_probe.cpp path
// (1080p RAW10 2-lane 30fps), with the ISP un-bypassed to demosaic to RGB565.
//
// I2C discipline: ALL SCCB traffic happens inside begin(), which main_remote
// calls after the UI port has initialized Wire (GPIO7/8, shared with GT911 +
// ES8311) and BEFORE the UI task starts polling touch. After begin() returns
// the camera never touches the bus again.

#include "remote_camera.h"

#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>

#include <algorithm>
#include <cmath>

#include "driver/i2s_std.h"
#include "driver/isp.h"
#include "driver/jpeg_encode.h"
#include "driver/ppa.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_ldo_regulator.h"

#include "cam_probe_regs.h"  // OV02C10 tables (proven by the #150 probe)
#include "telnet_log.h"
#include "wifi_prov.h"

namespace remote_camera {
namespace {

constexpr uint8_t kAddr = 0x36;      // OV02C10 SCCB (PID 0x5602)
constexpr int kW = 1920, kH = 1080;  // RAW10 2-lane table in cam_probe_regs.h
constexpr int kXclkPin = 13;         // CODEC_I2S0_MCLK — shared codec+camera MCLK
constexpr int kMipiLdoChan = 3;      // same PHY rail the DSI panel holds (refcounted)
constexpr int kMipiLdoMv = 2500;
// RGB565 1080p = 4,147,200 B — already a multiple of the P4's 128B cache line.
constexpr size_t kFrameBytes = (size_t)kW * kH * 2;
constexpr size_t kJpgOutBytes = 1024 * 1024;
constexpr uint16_t kHttpPort = 8080;
constexpr uint32_t kStreamMinIntervalMs = 100;  // ~10 fps cap on /stream

// Round-4 memory hardening (#150 OOM fix). The big camera buffers already live
// in PSRAM and gJpgOut is reused, but SERVING through the WireGuard tunnel is
// INTERNAL-RAM heavy: lwIP TCP pbufs + mbedtls encryption buffers + the listen
// backlog (httpTask is single-threaded, so extra concurrent connections queue
// in lwIP). Under a 3-stream + rapid-snapshot hammer, internal heap cratered
// from ~74KB idle to 21,636 B and allocations panicked. Defenses:
//  - refuse to start a serve (or continue a stream) below a heap floor -> 503
//  - bound concurrency: one serve at a time; extras get a fast 503 and a tiny
//    lwIP listen backlog so they can't pile up RAM
//  - smaller snapshots (half-res q60) so peak transfer/buffer pressure is ~4x
//    lower than the old 606KB full-res q75 outlier
//  - THROTTLED sends: the tunnel push (lwIP pbufs + WG/mbedtls crypto) is what
//    actually drains internal RAM, and it happens AFTER the start gate — so
//    sendAll chunks small, yields to let lwIP drain ACKs/free pbufs between
//    chunks, and HARD-ABORTS mid-send if internal heap hits a hard floor.
//    Tiered thresholds: start-serve > continue-stream > send-abort.
// The real OOM victim under the hammer is the P4<->C6 WiFi-over-SDIO driver:
// at ~21KB internal it asserted in sdio_rx_get_buffer (couldn't get an RX
// buffer for the incoming connection flood). So the camera must leave the
// WiFi/SDIO stack a healthy internal reserve at ALL times, INCLUDING the
// momentary per-frame send dip. Levers: (1) small stream frames (quarter-res)
// so the dip is shallow; (2) guard floors high enough that heap never nears
// the SDIO panic point. httpTask is single-threaded (one serve at a time); the
// hammer's extra connections are refused via the tiny backlog.
constexpr uint32_t kServeStartHeap    = 50 * 1024;  // gate to BEGIN a serve
constexpr uint32_t kStreamContinueHeap = 40 * 1024; // per-frame: stop a stream below this
constexpr uint32_t kSendAbortHeap     = 34 * 1024;  // hard-abort a send in progress
constexpr size_t   kSendChunk         = 4096;       // per write() — finer heap-abort granularity
constexpr uint8_t  kHttpBacklog       = 1;          // lwIP listen backlog (was default 4)

inline uint32_t internalFree() { return heap_caps_get_free_size(MALLOC_CAP_INTERNAL); }

// AE follow-up (host-side loop; the sensor has no working internal AEC):
// register semantics per Linux drivers/media/i2c/ov02c10.c — exposure is a
// 16-bit line count at 0x3501/02 (max VTS-8), analog gain is a 16-bit value
// at 0x3508/09 equal to code<<4 with code 0x10 = 1x .. 0xf8 = 15.5x.
constexpr uint16_t kVts = 0x0918;          // from the format table (0x380e/f)
constexpr uint16_t kExpMin = 64, kExpMax = kVts - 8;
constexpr uint16_t kGainMin = 0x10, kGainMax = 0xf8;  // code units, 16 = 1x
// Digital gain (24-bit @0x350a = code<<6, code 0x400 = 1x): the last rung of
// the ladder for scenes darker than exp-max + 15.5x analog can reach. Capped
// at 4x — beyond that it's just amplified noise.
constexpr uint32_t kDgainMin = 0x400, kDgainMax = 0x1000;
constexpr uint32_t kAeTargetLuma = 22;     // green-channel mean, 0..63 (~88/255).
                                           // Round 3: lowered from 26 to shrink the
                                           // clipped (green-tinting) lamp cores.
constexpr uint32_t kAeDeadband = 4;
constexpr uint32_t kAePeriodMs = 500;      // 2 Hz — slow, per the I2C-sharing rule

// Round-3 AWB (host-side, ISP-only — zero SCCB, no Wire mutex): the round-2
// FIXED gray-world CCM hit the global mean only because clipped-green lamp
// highlights cancelled magenta midtones (owner-measured: midtones R/G=1.09
// B/G=1.10, highlights 0.87). This loop meters ONLY unclipped midtone pixels
// of the latest post-CCM frame and walks the CCM diagonal toward neutralizing
// THOSE (closed loop), heavily rate-limited and gated on AE being settled so
// the two loops can't chase each other.
constexpr uint32_t kAwbEveryNthTick = 4;   // 4 x 500ms = ~0.5 Hz
constexpr uint32_t kAwbAeSettleMs = 3000;  // AE quiet this long before AWB moves
constexpr float kAwbGainMin = 0.6f, kAwbGainMax = 1.6f;
constexpr float kAwbStepMax = 1.04f;       // <=4% gain change per step
constexpr float kAwbDeadband = 0.01f;      // stop inside +-1% of neutral (EMA'd)
constexpr float kAwbEmaAlpha = 0.3f;       // smooth the per-frame metering noise
                                           // (bench: +-3.5% frame-to-frame) so the
                                           // loop centers instead of hunting
constexpr uint32_t kAwbLumaLo = 60, kAwbLumaHi = 160;  // midtone band (8-bit luma)
constexpr uint32_t kAwbClip = 240;         // any channel at/above = excluded
constexpr uint32_t kAwbMinSamples = 400;   // else scene has no usable midtones
// Pipeline calibration (bench 2026-07-10): the SERVED JPEG's midtone ratios
// read slightly above the device's RGB565 metering (hw JPEG chroma path +
// different band populations after downsampling) — B by ~+2%, R by ~+1% on a
// stable scene, with some scene dependence. Acceptance is measured on the
// JPEG, so steer the device-domain ratios a hair below neutral. (Bracketed
// on-bench: B target 1.00 -> JPEG 1.03; 0.98 -> JPEG 0.95-0.99.)
constexpr float kAwbTargetRG = 0.995f;
constexpr float kAwbTargetBG = 0.99f;

// state
bool gBegun = false;
bool gInitOk = false;
SemaphoreHandle_t gWireMux = nullptr;  // shared-I2C lock (AE task vs touch poll)
uint16_t gAeExp = 0x046c;              // running AE state, seeded from the table
uint16_t gAeGain = 0x80;               // code units (0x80 = 8x, table default)
uint32_t gAeDgain = kDgainMin;         // code units (0x400 = 1x)
volatile uint32_t gAeLastAdjMs = 0;    // last AE register write (gates AWB)
isp_proc_handle_t gIsp = nullptr;      // kept for runtime CCM (AWB) updates
float gWbR = 0.77f, gWbB = 0.78f;      // live CCM diagonal (seed = round-2 bench fit)
volatile bool gEnabled = true;   // privacy switch — default ON (pilot device)
volatile bool gClientActive = false;
volatile uint32_t gFrames = 0;   // every frame the CSI DMA finished
volatile uint32_t gSeq = 0;      // published (completed) frames
volatile int gLatest = 0;        // index of the last completed buffer
int gSup = 0;                    // next buffer to hand the CSI driver (ISR-only)

uint8_t* gBuf[2] = {nullptr, nullptr};  // alternating RGB565 PSRAM buffers
esp_cam_ctlr_handle_t gCam = nullptr;
jpeg_encoder_handle_t gEnc = nullptr;
uint8_t* gJpgOut = nullptr;
size_t gJpgOutSize = 0;
uint8_t* gJpgIn = nullptr;       // lazy fallback if the encoder rejects the CSI buffer
size_t gJpgInSize = 0;
// Follow-up #1/#2: the sensor is portrait-mounted, so PPA hardware rotates
// each SERVED frame 90 deg CCW (1920x1080 -> 1080x1920) at encode time — the
// 30fps capture path stays untouched and PPA+JPEG cost is only paid per
// served frame.
ppa_client_handle_t gPpa = nullptr;
uint8_t* gRotBuf = nullptr;      // 1080x1920 RGB565, PSRAM, 128-aligned
jpeg_down_sampling_type_t gSubSample = JPEG_DOWN_SAMPLING_YUV420;  // demoted to 422 at runtime if 1080-wide 420 is rejected
// Stream fps follow-up: the tunnel is ~7Mbps, so the MJPEG stream serves a
// PPA half-scale 536x960 frame (input cropped 1080->1072 rows so the rotated
// scaled width stays a multiple of 8 for the JPEG encoder) while snapshots
// keep full 1080x1920.
// Snapshot = half-res (536x960); stream = quarter-res (264x480) so its
// per-frame send dip is shallow (round-4 SDIO panic was the deep half-res dip
// coinciding with the WiFi RX flood). PPA rotates 90 then scales; output width
// must be a multiple of 8, so the input rows are cropped to keep it aligned:
// half  -> crop 1080->1072 (=2*536), out 536x960
// quarter-> crop 1080->1056 (=4*264), out 264x480
constexpr int kHalfW = 536, kHalfH = 960;
constexpr size_t kHalfBytes = (size_t)kHalfW * kHalfH * 2;      // 1,029,120 = 128*8040
constexpr int kQuartW = 264, kQuartH = 480;
constexpr size_t kQuartBytes = (size_t)kQuartW * kQuartH * 2;   // 253,440 = 128*1980
uint8_t* gHalfBuf = nullptr;
uint8_t* gQuartBuf = nullptr;
bool gScaleBroken = false;       // runtime fallback to full-res if a scaled PPA path fails

// ---- SCCB (all traffic confined to begin()) ----
bool wr(uint16_t reg, uint8_t val) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF); Wire.write(val);
  return Wire.endTransmission() == 0;
}
bool rd(uint16_t reg, uint8_t* out) {
  Wire.beginTransmission(kAddr);
  Wire.write(reg >> 8); Wire.write(reg & 0xFF);
  // SCCB wants a STOP between address phase and read (repeated start garbles).
  if (Wire.endTransmission(true) != 0) return false;
  if (Wire.requestFrom((int)kAddr, 1) != 1) return false;
  *out = Wire.read(); return true;
}
bool writeTable(const ov02c10_reginfo_t* t) {
  for (; t->reg != OV02C10_REG_END; t++) {
    if (t->reg == OV02C10_REG_DELAY) { delay(t->val); continue; }
    if (!wr(t->reg, t->val)) {
      telnet_log::logf("[cam] SCCB write FAILED at 0x%04X", t->reg);
      return false;
    }
  }
  return true;
}

// Program the live WB gains into the ISP CCM, NORMALIZED so the smallest
// diagonal entry is 1.0 (G scales UP instead of R/B scaling down). Ratios —
// and thus the midtone correction — are identical, but sensor-clipped whites
// (lamps, a sunlit shirt) saturate to WHITE instead of blooming green, and
// the extra G headroom lets AE shed digital gain. saturation=true clamps
// in-hardware; entries stay well under the 4.0 matrix limit.
esp_err_t applyCcm() {
  if (!gIsp) return ESP_FAIL;
  const float k = 1.0f / std::min(std::min(gWbR, gWbB), 1.0f);
  esp_isp_ccm_config_t ccm = {};
  ccm.matrix[0][0] = gWbR * k;
  ccm.matrix[1][1] = k;
  ccm.matrix[2][2] = gWbB * k;
  ccm.saturation = true;
  ccm.flags.update_once_configured = 1;
  return esp_isp_ccm_configure(gIsp, &ccm);
}

// ---- CSI callbacks (ISR context) ----
// The whole capture path runs through these (the probe-proven pattern).
// BENCH FINDING: with on_get_new_trans registered, the driver services every
// frame from the callback and NEVER drains the esp_cam_ctlr_receive() queue —
// receive() timed out at 1000ms while hw frames flowed at 33fps. So the
// callbacks ARE the capture loop: getNewTrans ping-pongs the two buffers (the
// driver prefetches a trans ahead of the in-flight one, so the supply index
// must advance on SUPPLY, not on completion), and transFinished publishes
// whichever buffer just completed. The privacy gate is the HTTP layer, per
// remote_camera.h — frames landing in PSRAM while disabled is by design;
// they never leave the device.
bool IRAM_ATTR onGetNewTrans(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* t, void*) {
  t->buffer = gBuf[gSup];
  t->buflen = kFrameBytes;
  gSup ^= 1;
  return false;
}
bool IRAM_ATTR onTransFinished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t* t, void*) {
  gLatest = (t->buffer == gBuf[0]) ? 0 : 1;
  gSeq = gSeq + 1;
  gFrames = gFrames + 1;
  return false;
}

// ---- hardware bring-up (runs once, in begin()) ----
bool initXclk() {
  // 24 MHz on GPIO13 = I2S0_MCLK: 46875 Hz x MCLK_MULTIPLE_512 = exactly 24 MHz.
  // APLL is mandatory: XTAL(40M) can't divide to 24M, PLL_160M boot-loops this
  // silicon, LEDC can't reach 24M. Std mode needs real BCLK/WS pins — the
  // ES8311 codec's GPIO12/GPIO10 idle-clock harmlessly.
  i2s_chan_handle_t tx = nullptr;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  i2s_std_config_t sc = {};
  sc.clk_cfg.sample_rate_hz = 46875;
  sc.clk_cfg.clk_src = I2S_CLK_SRC_APLL;
  sc.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_512;
  sc.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                    I2S_SLOT_MODE_STEREO);
  sc.gpio_cfg.mclk = (gpio_num_t)kXclkPin;
  sc.gpio_cfg.bclk = GPIO_NUM_12;
  sc.gpio_cfg.ws = GPIO_NUM_10;
  sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
  sc.gpio_cfg.din = I2S_GPIO_UNUSED;
  esp_err_t e1 = i2s_new_channel(&cc, &tx, nullptr);
  esp_err_t e2 = (e1 == ESP_OK) ? i2s_channel_init_std_mode(tx, &sc) : ESP_FAIL;
  esp_err_t e3 = (e2 == ESP_OK) ? i2s_channel_enable(tx) : ESP_FAIL;
  if (e3 != ESP_OK) {
    telnet_log::logf("[cam] XCLK via I2S FAILED: new=0x%x init=0x%x en=0x%x", e1, e2, e3);
    return false;
  }
  return true;
}

bool initHw() {
  if (!initXclk()) return false;
  delay(5);

  // 1. Sensor identity (SCCB sanity — Wire already begun by the UI port).
  uint8_t idh = 0, idl = 0;
  const bool idOk = rd(0x300A, &idh) && rd(0x300B, &idl);
  telnet_log::logf("[cam] sensor id: %s %02X%02X (want 5602)", idOk ? "read" : "READ-FAIL", idh, idl);
  if (!idOk || idh != 0x56 || idl != 0x02) return false;

  // 2. MIPI PHY rail (refcounted LDO channel; the DSI panel also holds it).
  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t lcfg = {};
  lcfg.chan_id = kMipiLdoChan;
  lcfg.voltage_mv = kMipiLdoMv;
  if (esp_ldo_acquire_channel(&lcfg, &ldo) != ESP_OK) {
    telnet_log::logf("[cam] LDO acquire FAILED");
    return false;
  }

  // 3. Frame buffers (PSRAM, 128B cache-line aligned, size is a 128 multiple).
  for (int i = 0; i < 2; i++) {
    gBuf[i] = (uint8_t*)heap_caps_aligned_calloc(128, 1, kFrameBytes,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!gBuf[i]) { telnet_log::logf("[cam] PSRAM frame alloc FAILED"); return false; }
  }

  // 4. CSI controller: RAW10 in, RGB565 out (the ISP demosaics in between).
  esp_cam_ctlr_csi_config_t ccfg = {};
  ccfg.ctlr_id = 0;
  ccfg.clk_src = MIPI_CSI_PHY_CLK_SRC_DEFAULT;
  ccfg.h_res = kW;
  ccfg.v_res = kH;
  ccfg.data_lane_num = 2;
  ccfg.lane_bit_rate_mbps = (int)(OV02C10_MIPI_CSI_LINE_RATE_1920x1080_30FPS / 1000000ULL);
  ccfg.input_data_color_type = CAM_CTLR_COLOR_RAW10;
  ccfg.output_data_color_type = CAM_CTLR_COLOR_RGB565;
  ccfg.queue_items = 2;
  ccfg.bk_buffer_dis = 1;  // our padded buffers, not the driver's unaligned fb
  esp_err_t e = esp_cam_new_csi_ctlr(&ccfg, &gCam);
  if (e != ESP_OK) { telnet_log::logf("[cam] csi ctlr FAILED: 0x%x", e); return false; }

  // 5. ISP processor, REAL (not bypass): RAW10 from CSI -> demosaic -> RGB565.
  esp_isp_processor_cfg_t icfg = {};
  icfg.clk_src = ISP_CLK_SRC_DEFAULT;
  icfg.clk_hz = 160000000;  // 240M unsupported on this silicon's default src
  icfg.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
  icfg.input_data_color_type = ISP_COLOR_RAW10;
  icfg.output_data_color_type = ISP_COLOR_RGB565;
  icfg.h_res = kW;
  icfg.v_res = kH;
  icfg.bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG;
  esp_err_t ei = esp_isp_new_processor(&icfg, &gIsp);
  if (ei != ESP_OK) { telnet_log::logf("[cam] ISP new FAILED: 0x%x", ei); return false; }
  ei = esp_isp_enable(gIsp);
  if (ei != ESP_OK) { telnet_log::logf("[cam] ISP enable FAILED: 0x%x", ei); return false; }
  // Round-3 WB: the CCM diagonal is now LIVE — awbStep() re-fits it from
  // midtone statistics at ~0.5Hz (see constants above). Seed with the round-2
  // bench fit so the first frames aren't wildly off under this room's light.
  { esp_err_t ec = applyCcm();
    if (ec == ESP_OK) ec = esp_isp_ccm_enable(gIsp);
    if (ec != ESP_OK) telnet_log::logf("[cam] CCM FAILED: 0x%x (colors uncorrected)", ec);
  }
  // Round-3 BLC: subtract the OmniVision sensor pedestal (64 in the RAW10
  // domain — matching the format table's BLC target, reg 0x4003=0x40) in the
  // ISP so shadows don't carry the cast through the CCM gains. Non-fatal.
  { esp_isp_blc_config_t blc = {};
    blc.window.top_left.x = 0;
    blc.window.top_left.y = 0;
    blc.window.btm_right.x = kW - 1;
    blc.window.btm_right.y = kH - 1;
    blc.filter_enable = false;
    blc.flags.update_once_configured = 1;
    esp_err_t eb = esp_isp_blc_configure(gIsp, &blc);
    if (eb == ESP_OK) eb = esp_isp_blc_enable(gIsp);
    if (eb == ESP_OK) {
      esp_isp_blc_offset_t off = {};
      off.top_left_chan_offset = 64;
      off.top_right_chan_offset = 64;
      off.bottom_left_chan_offset = 64;
      off.bottom_right_chan_offset = 64;
      eb = esp_isp_blc_set_correction_offset(gIsp, &off);
    }
    if (eb != ESP_OK) telnet_log::logf("[cam] BLC FAILED: 0x%x (pedestal uncorrected)", eb);
  }

  esp_cam_ctlr_evt_cbs_t cbs = {};
  cbs.on_get_new_trans = onGetNewTrans;
  cbs.on_trans_finished = onTransFinished;
  esp_cam_ctlr_register_event_callbacks(gCam, &cbs, nullptr);
  if (esp_cam_ctlr_enable(gCam) != ESP_OK || esp_cam_ctlr_start(gCam) != ESP_OK) {
    telnet_log::logf("[cam] csi enable/start FAILED");
    return false;
  }

  // 6. Sensor init + stream on — the LAST SCCB traffic this firmware ever does.
  if (!writeTable(ov02c10_mipi_reset_regs)) return false;
  if (!writeTable(ov02c10_input_24M_MIPI_2lane_raw10_1920x1080_30fps)) return false;
  if (!wr(0x4800, 0x44)) return false;  // continuous MIPI clock (CSI host needs it to lock)
  // AE note: internal AEC was tried and does NOT exist on this part — 0x3503
  // (POR 0xA8) accepted 0x00 but exposure never adapted (luma pinned across
  // scene changes). Exposure control is therefore the host-side aeTask below,
  // matching what both the vendor SDK (stubbed AE) and Linux (V4L2 manual
  // controls + host AE) do with this sensor. 0x3503 stays at its proven
  // vendor default.
  if (!wr(0x0100, 0x01)) { telnet_log::logf("[cam] stream-on FAILED"); return false; }

  // 7. Hardware JPEG engine + output buffer.
  jpeg_encode_engine_cfg_t jcfg = {};
  jcfg.intr_priority = 0;
  jcfg.timeout_ms = 200;
  if (jpeg_new_encoder_engine(&jcfg, &gEnc) != ESP_OK) {
    telnet_log::logf("[cam] jpeg engine FAILED");
    return false;
  }
  jpeg_encode_memory_alloc_cfg_t mcfg = {};
  mcfg.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
  gJpgOut = (uint8_t*)jpeg_alloc_encoder_mem(kJpgOutBytes, &mcfg, &gJpgOutSize);
  if (!gJpgOut) { telnet_log::logf("[cam] jpeg out alloc FAILED"); return false; }

  // 8. PPA SRM client + rotated frame buffer (rotation follow-up). Non-fatal:
  // without PPA we serve unrotated frames rather than none.
  { ppa_client_config_t pcfg = {};
    pcfg.oper_type = PPA_OPERATION_SRM;
    esp_err_t ep = ppa_register_client(&pcfg, &gPpa);
    if (ep == ESP_OK) {
      gRotBuf = (uint8_t*)heap_caps_aligned_calloc(128, 1, kFrameBytes,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!gRotBuf) { telnet_log::logf("[cam] rot buf alloc FAILED - serving unrotated"); }
      gHalfBuf = (uint8_t*)heap_caps_aligned_calloc(128, 1, kHalfBytes,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      gQuartBuf = (uint8_t*)heap_caps_aligned_calloc(128, 1, kQuartBytes,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    } else {
      telnet_log::logf("[cam] PPA register FAILED: 0x%x - serving unrotated", ep);
      gPpa = nullptr;
    }
  }

  telnet_log::logf("[cam] init OK: %dx%d RAW10->ISP->RGB565, PPA rot90, JPEG hw, http :%u",
                   kW, kH, kHttpPort);
  return true;
}

// Mean green-channel luma (0..63) of the latest frame, ~4096 subsampled px.
uint32_t meanLuma() {
  if (gSeq == 0) return 0;
  uint8_t* b = gBuf[gLatest];
  esp_cache_msync(b, kFrameBytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  uint32_t sum = 0;
  const size_t stride = (kFrameBytes / 2 / 4096) * 2;  // even byte offsets
  for (int i = 0; i < 4096; i++) {
    const uint16_t px = *(const uint16_t*)(b + (size_t)i * stride);
    sum += (px >> 5) & 0x3F;
  }
  return sum / 4096;
}

// ---- round-3 AWB step (~0.5 Hz, called from aeTask): meter midtone-only
// R/G and B/G of the latest post-CCM frame and walk the live CCM diagonal
// toward neutral. ISP-register-only — zero SCCB, no Wire mutex. ----
void awbStep() {
  if (!gIsp || gSeq == 0) return;
  uint8_t* b = gBuf[gLatest];
  esp_cache_msync(b, kFrameBytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
  uint32_t sumR = 0, sumG = 0, sumB = 0, n = 0;
  constexpr int kN = 8192;
  const size_t stride = (kFrameBytes / 2 / kN) * 2;
  for (int i = 0; i < kN; i++) {
    const uint16_t px = *(const uint16_t*)(b + (size_t)i * stride);
    // RGB565 -> 8-bit with bit replication (plain shifts bias the ratios low)
    const uint32_t r = ((px >> 11) << 3) | ((px >> 11) >> 2);
    const uint32_t g = (((px >> 5) & 0x3F) << 2) | (((px >> 5) & 0x3F) >> 4);
    const uint32_t bl = ((px & 0x1F) << 3) | ((px & 0x1F) >> 2);
    const uint32_t luma = (r + 2 * g + bl) >> 2;
    if (luma < kAwbLumaLo || luma > kAwbLumaHi) continue;       // midtones only
    if (r >= kAwbClip || g >= kAwbClip || bl >= kAwbClip) continue;  // no clipped px
    sumR += r; sumG += g; sumB += bl; n++;
  }
  if (n < kAwbMinSamples || sumG == 0) return;  // no usable midtones this frame
  // EMA the per-frame ratios: single-frame metering noise is ~+-3.5% on this
  // scene, larger than the acceptance band — adjust toward the smoothed cast.
  static float sRg = 0.0f, sBg = 0.0f;
  static bool sEmaInit = false;
  const float fRg = (float)sumR / sumG, fBg = (float)sumB / sumG;
  if (!sEmaInit) { sEmaInit = true; sRg = fRg; sBg = fBg; }
  else {
    sRg += kAwbEmaAlpha * (fRg - sRg);
    sBg += kAwbEmaAlpha * (fBg - sBg);
  }
  const float rg = sRg / kAwbTargetRG, bg = sBg / kAwbTargetBG;  // residual vs calibrated neutral
  if (fabsf(rg - 1.0f) < kAwbDeadband && fabsf(bg - 1.0f) < kAwbDeadband) return;
  // Closed loop: divide the residual cast out of the current gains, but move
  // at most kAwbStepMax per step and stay inside the sane range.
  float nR = std::min(std::max(gWbR / rg, gWbR / kAwbStepMax), gWbR * kAwbStepMax);
  float nB = std::min(std::max(gWbB / bg, gWbB / kAwbStepMax), gWbB * kAwbStepMax);
  nR = std::min(std::max(nR, kAwbGainMin), kAwbGainMax);
  nB = std::min(std::max(nB, kAwbGainMin), kAwbGainMax);
  if (nR == gWbR && nB == gWbB) return;
  // The next frames' measured cast scales with the gain change — rescale the
  // EMA by the same factor so it doesn't feed back stale residuals.
  sRg *= nR / gWbR;
  sBg *= nB / gWbB;
  gWbR = nR;
  gWbB = nB;
  const esp_err_t ec = applyCcm();
  telnet_log::logf("[cam] AWB: mid R/G %.3f B/G %.3f (n=%lu) -> gains R %.3f B %.3f (%s)",
                   (double)rg, (double)bg, (unsigned long)n, (double)gWbR, (double)gWbB,
                   ec == ESP_OK ? "ok" : "CCM FAIL");
}

// ---- host-side AE task (2 Hz): meter the latest frame, steer exposure then
// analog gain toward the target. The ONLY runtime SCCB in the firmware; every
// register write is serialized against the touch poll via gWireMux. Also
// paces the ~0.5 Hz AWB step, which only runs while AE has been settled for
// kAwbAeSettleMs (so the two loops never chase each other). ----
void aeTask(void*) {
  uint32_t tick = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kAePeriodMs));
    if (gSeq == 0) continue;
    if (++tick % kAwbEveryNthTick == 0 &&
        millis() - gAeLastAdjMs > kAwbAeSettleMs) {
      awbStep();
    }
    uint32_t m = meanLuma();
    if (m == 0) m = 1;
    const uint32_t lo = kAeTargetLuma - kAeDeadband, hi = kAeTargetLuma + kAeDeadband;
    if (m >= lo && m <= hi) continue;
    // Multiplicative correction, clamped to x0.6..x1.5 per 500ms step so the
    // loop converges smoothly instead of oscillating.
    uint32_t ratioQ8 = kAeTargetLuma * 256 / m;
    if (ratioQ8 < 154) ratioQ8 = 154;
    if (ratioQ8 > 384) ratioQ8 = 384;
    // Total light = exposure(lines) x gain(code, 16 = 1x). Re-split the new
    // total preferring exposure (less noise), spilling into gain at exp max,
    // and into digital gain (noisiest) only when both analog rungs rail.
    uint64_t newL = ((uint64_t)gAeExp * gAeGain * ratioQ8) >> 8;
    uint16_t exp = (uint16_t)std::min<uint64_t>(kExpMax, std::max<uint64_t>(kExpMin, newL / kGainMin));
    uint16_t gain = (uint16_t)std::min<uint64_t>(kGainMax, std::max<uint64_t>(kGainMin, newL / exp));
    uint32_t dgain = gAeDgain;
    if (m < lo && exp == kExpMax && gain == kGainMax) {
      dgain = std::min<uint32_t>(kDgainMax, (uint32_t)(((uint64_t)gAeDgain * ratioQ8) >> 8));
    } else if (m > hi && gAeDgain > kDgainMin) {
      // Too bright while dgain is in play: shed the noisy rung first.
      dgain = std::max<uint32_t>(kDgainMin, (uint32_t)(((uint64_t)gAeDgain * ratioQ8) >> 8));
      exp = gAeExp;
      gain = gAeGain;
    }
    if (exp == gAeExp && gain == gAeGain && dgain == gAeDgain) continue;
    gAeExp = exp;
    gAeGain = gain;
    gAeDgain = dgain;
    gAeLastAdjMs = millis();  // AE moved -> hold AWB off until it settles
    const uint16_t greg = gain << 4;       // 16-bit reg value = code<<4 (Linux semantics)
    const uint32_t dreg = dgain << 6;      // 24-bit reg value = code<<6
    wireLock();
    bool ok = wr(0x3501, exp >> 8) && wr(0x3502, exp & 0xFF) &&
              wr(0x3508, greg >> 8) && wr(0x3509, greg & 0xFF) &&
              wr(0x350a, (dreg >> 16) & 0xFF) && wr(0x350b, (dreg >> 8) & 0xFF) &&
              wr(0x350c, dreg & 0xFF);
    wireUnlock();
    telnet_log::logf("[cam] AE: luma %lu -> exp %u gain %u/16x dgain %lu/1024x (%s)",
                     (unsigned long)m, exp, gain, (unsigned long)dgain,
                     ok ? "ok" : "SCCB FAIL");
  }
}

// ---- capture health task: the ISR callbacks above do the real capture ----
void captureTask(void*) {
  // First-frames fps report, then a stall watchdog (log-only).
  uint32_t f0 = gFrames;
  const uint32_t t0 = millis();
  bool fpsLogged = false;
  uint32_t lastF = f0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(5000));
    const uint32_t f = gFrames;
    if (!fpsLogged && f > f0) {
      fpsLogged = true;
      telnet_log::logf("[cam] capturing: %.1f fps (%lu frames in %lums, seq=%lu)",
                       (double)(f - f0) * 1000.0 / (millis() - t0),
                       (unsigned long)(f - f0), (unsigned long)(millis() - t0),
                       (unsigned long)gSeq);
    } else if (f == lastF) {
      telnet_log::logf("[cam] capture STALLED at %lu frames", (unsigned long)f);
    }
    lastF = f;
    // AE observability + round-4 heap telemetry: mean luma, AE state, and the
    // internal-RAM free + all-time low watermark (the OOM floor is visible here).
    static uint32_t sLumaTick = 0;
    if (++sLumaTick >= 6 && gSeq != 0) {
      sLumaTick = 0;
      telnet_log::logf("[cam] mean luma %lu/63 exp=%u gain=%u/16x heap_int=%lu low=%lu (frame %lu)",
                       (unsigned long)meanLuma(), gAeExp, gAeGain,
                       (unsigned long)internalFree(),
                       (unsigned long)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned long)gSeq);
    }
  }
}

// ---- JPEG encode the latest published frame; returns size (0 = failure).
// scaleDiv: 1 = full 1080x1920 (rot only), 2 = half 536x960 (snapshot),
// 4 = quarter 264x480 (stream — small frames keep the per-frame send dip
// shallow so the WiFi/SDIO stack keeps its internal-RAM reserve, round-4). ----
uint32_t encodeLatest(uint8_t quality, int scaleDiv) {
  if (gSeq == 0) return 0;
  uint8_t* src = gBuf[gLatest];
  // Invalidate cached lines FIRST: both PPA and the JPEG driver write back
  // (C2M) their input internally, and stale CPU cache lines would clobber the
  // fresh CSI DMA data.
  esp_cache_msync(src, kFrameBytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);

  // Pick the scaled output buffer/geometry. Fall back to full-res if a scaled
  // path is requested but unavailable/broken, so the camera never goes dark.
  uint8_t* outBuf = gRotBuf; size_t outBytes = kFrameBytes;
  int outW = kH, outH = kW, inRows = kH; float scale = 1.0f;  // (rotated) full
  if (scaleDiv == 2 && gHalfBuf && !gScaleBroken) {
    outBuf = gHalfBuf; outBytes = kHalfBytes; outW = kHalfW; outH = kHalfH;
    inRows = 2 * kHalfW; scale = 0.5f;
  } else if (scaleDiv == 4 && gQuartBuf && !gScaleBroken) {
    outBuf = gQuartBuf; outBytes = kQuartBytes; outW = kQuartW; outH = kQuartH;
    inRows = 4 * kQuartW; scale = 0.25f;
  }

  // PPA-rotate the served frame 90 deg CCW into the portrait buffer (scaled for
  // snapshot/stream). Crop input rows to inRows so the rotated+scaled output
  // width stays a multiple of 8 for the JPEG encoder. On any PPA failure fall
  // back to the next-simpler proven path so the camera never goes dark.
  const uint8_t* encSrc = src;
  uint32_t encW = kW, encH = kH;
  if (gPpa && gRotBuf) {
    ppa_srm_oper_config_t o = {};
    o.in.buffer = src;
    o.in.pic_w = kW;  o.in.pic_h = kH;
    o.in.block_w = kW;
    o.in.block_h = inRows;
    o.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    o.out.buffer = outBuf;
    o.out.buffer_size = outBytes;
    o.out.pic_w = outW;
    o.out.pic_h = outH;
    o.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    o.rotation_angle = PPA_SRM_ROTATION_ANGLE_90;  // CCW; flips scene upright for this mount
    o.scale_x = scale;
    o.scale_y = scale;
    o.mode = PPA_TRANS_MODE_BLOCKING;
    esp_err_t ep = ppa_do_scale_rotate_mirror(gPpa, &o);
    if (ep == ESP_OK) {
      encSrc = outBuf; encW = outW; encH = outH;
    } else if (scaleDiv != 1) {
      gScaleBroken = true;  // a scaled path failed -> serve full-res from now on
      telnet_log::logf("[cam] PPA scale/%d FAILED: 0x%x - falling back to full-res", scaleDiv, ep);
      return encodeLatest(quality, 1);
    } else {
      static bool sWarned = false;
      if (!sWarned) { sWarned = true; telnet_log::logf("[cam] PPA rotate FAILED: 0x%x - serving unrotated", ep); }
    }
  }

  const size_t encBytes = (size_t)encW * encH * 2;
  jpeg_encode_cfg_t cfg = {};
  cfg.width = encW;
  cfg.height = encH;
  cfg.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
  cfg.sub_sample = gSubSample;
  cfg.image_quality = quality;
  uint32_t sz = 0;
  esp_err_t e = jpeg_encoder_process(gEnc, &cfg, encSrc, encBytes, gJpgOut, gJpgOutSize, &sz);
  if (e == ESP_ERR_INVALID_ARG && gSubSample == JPEG_DOWN_SAMPLING_YUV420) {
    // Non-MCU-aligned width may violate the encoder's YUV420 constraint —
    // demote to YUV422 once and retry (slightly larger files, same look).
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    e = jpeg_encoder_process(gEnc, &cfg, encSrc, encBytes, gJpgOut, gJpgOutSize, &sz);
    if (e == ESP_OK) {
      gSubSample = JPEG_DOWN_SAMPLING_YUV422;
      telnet_log::logf("[cam] YUV420 rejected at %lux%lu - staying on YUV422",
                       (unsigned long)encW, (unsigned long)encH);
    }
  }
  if (e == ESP_ERR_INVALID_ARG && !gJpgIn) {
    // Encoder rejected the buffer itself — memcpy through a driver-allocated
    // input buffer (documented recipe; never triggered on the bench so far).
    jpeg_encode_memory_alloc_cfg_t mcfg = {};
    mcfg.buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER;
    gJpgIn = (uint8_t*)jpeg_alloc_encoder_mem(kFrameBytes, &mcfg, &gJpgInSize);
    telnet_log::logf("[cam] direct encode rejected (0x%x); fallback inbuf %s",
                     e, gJpgIn ? "allocated" : "ALLOC FAILED");
    if (gJpgIn) {
      memcpy(gJpgIn, encSrc, encBytes);
      e = jpeg_encoder_process(gEnc, &cfg, gJpgIn, encBytes, gJpgOut, gJpgOutSize, &sz);
    }
  } else if (e != ESP_OK && gJpgIn) {
    memcpy(gJpgIn, encSrc, encBytes);
    e = jpeg_encoder_process(gEnc, &cfg, gJpgIn, encBytes, gJpgOut, gJpgOutSize, &sz);
  }
  if (e != ESP_OK) {
    telnet_log::logf("[cam] jpeg encode FAILED: 0x%x (%lux%lu)", e,
                     (unsigned long)encW, (unsigned long)encH);
    return 0;
  }
  return sz;
}

// ---- HTTP plumbing ----
// Send with a mid-transmission heap guard. write() blocks on the lwIP send
// buffer (which bounds per-connection pbufs and paces us to the tunnel rate),
// so no artificial delay is needed — but between chunks we HARD-ABORT if
// internal heap hits the send-abort floor (a truncated JPEG is graceful; an
// OOM panic is not). Returns true only if the whole buffer went out.
bool sendAll(WiFiClient& c, const uint8_t* p, size_t n) {
  size_t off = 0;
  while (off < n && c.connected()) {
    if (internalFree() < kSendAbortHeap) {
      telnet_log::logf("[cam] send abort at %u/%u (internal=%lu)",
                       (unsigned)off, (unsigned)n, (unsigned long)internalFree());
      return false;
    }
    size_t chunk = std::min<size_t>(n - off, kSendChunk);
    size_t w = c.write(p + off, chunk);
    if (w == 0) break;
    off += w;
  }
  return off == n;
}

void send403(WiFiClient& c) {
  c.print("HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n"
          "Camera disabled (privacy switch). Publish 1 to slytherm/remote/<id>/cmd/camera to enable.\r\n");
}

void send503(WiFiClient& c, const char* why) {
  c.printf("HTTP/1.1 503 Service Unavailable\r\nContent-Type: text/plain\r\n"
           "Connection: close\r\nRetry-After: 1\r\n\r\n%s\r\n", why);
}

void sendIndex(WiFiClient& c) {
  char body[512];
  snprintf(body, sizeof(body),
           "<!doctype html><title>SlyTherm Remote camera</title>"
           "<h1>SlyTherm Remote camera (#150)</h1>"
           "<p>Camera is %s. Frames captured: %lu</p>"
           "<ul><li><a href=\"/snapshot.jpg\">/snapshot.jpg</a> - single frame</li>"
           "<li><a href=\"/stream\">/stream</a> - MJPEG stream (~10 fps)</li></ul>",
           gEnabled ? "ENABLED" : "DISABLED", (unsigned long)gFrames);
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %u\r\n"
           "Connection: close\r\n\r\n", (unsigned)strlen(body));
  c.print(body);
}

void serveSnapshot(WiFiClient& c) {
  // Round-4: half-res q60 (was full-res q75). ~60KB vs the 606KB peak-pressure
  // outlier — AE/AWB are already tuned, this just shrinks the served frame.
  const uint32_t sz = encodeLatest(60, /*scaleDiv=*/2);
  if (sz == 0) { send503(c, "no frame available"); return; }
  c.printf("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n"
           "Connection: close\r\n\r\n", (unsigned long)sz);
  sendAll(c, gJpgOut, sz);
  telnet_log::logf("[cam] snapshot served: %lu bytes", (unsigned long)sz);
}

void serveStream(WiFiClient& c) {
  c.print("HTTP/1.1 200 OK\r\n"
          "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
          "Connection: close\r\n\r\n");
  uint32_t sent = 0;
  uint32_t lastSeq = 0;
  const uint32_t startMs = millis();
  bool lowHeap = false;
  while (c.connected() && gEnabled) {
    const uint32_t iterMs = millis();
    // Round-4: bail cleanly if internal heap has dropped to the continue floor
    // mid-stream (rather than pushing another encode/send toward OOM). Lower
    // than the start gate so a lone stream keeps running; a contended one backs
    // off. sendAll() below also hard-aborts a frame if heap craters further.
    if (internalFree() < kStreamContinueHeap) { lowHeap = true; break; }
    // wait (bounded) for a frame newer than the last one we sent
    for (int i = 0; i < 50 && gSeq == lastSeq; i++) vTaskDelay(pdMS_TO_TICKS(10));
    if (gSeq == lastSeq) continue;  // still no new frame; re-check the socket
    lastSeq = gSeq;
    const uint32_t sz = encodeLatest(50, /*scaleDiv=*/4);  // quarter-res stream: small frames = shallow heap dip
    if (sz == 0) continue;
    c.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n",
             (unsigned long)sz);
    // Stop on a failed frame send; still-connected means it was a heap abort.
    if (!sendAll(c, gJpgOut, sz)) { if (c.connected()) lowHeap = true; break; }
    c.print("\r\n");
    sent++;
    // ~10 fps cap
    const uint32_t took = millis() - iterMs;
    if (took < kStreamMinIntervalMs) vTaskDelay(pdMS_TO_TICKS(kStreamMinIntervalMs - took));
  }
  const uint32_t secs = (millis() - startMs) / 1000;
  telnet_log::logf("[cam] stream ended: %lu frames in %lus (%s)", (unsigned long)sent,
                   (unsigned long)secs,
                   lowHeap ? "low-heap floor" : gEnabled ? "client closed" : "disabled");
}

void handleClient(WiFiClient& c) {
  c.setTimeout(2000);  // ms (core 3.x NetworkClient: _timeout/1000 -> timeval)
  String req = c.readStringUntil('\n');
  // drain remaining headers (bounded)
  for (int i = 0; i < 32; i++) {
    String h = c.readStringUntil('\n');
    if (h.length() <= 1) break;  // "\r" or empty = end of headers
  }
  const bool wantSnap = req.startsWith("GET /snapshot.jpg");
  const bool wantStream = req.startsWith("GET /stream");
  telnet_log::logf("[cam] client %s: %s", c.remoteIP().toString().c_str(),
                   wantSnap ? "snapshot" : wantStream ? "stream" : "index");
  if (wantSnap || wantStream) {
    if (!gEnabled) { send403(c); return; }
    // Round-4 hard concurrency bound: one serve at a time. httpTask is
    // single-threaded so this normally can't already be set, but if it is
    // (belt-and-braces), reject fast instead of letting the client hold RAM.
    if (gClientActive) { send503(c, "camera busy - one client at a time"); return; }
    // Round-4 heap-floor guard: the panic-stopper. Refuse to begin a serve
    // when internal RAM is below the start gate; the tunnel serve path would OOM.
    const uint32_t hf = internalFree();
    if (hf < kServeStartHeap) {
      telnet_log::logf("[cam] low-heap 503 (internal=%lu)", (unsigned long)hf);
      send503(c, "low memory - try again shortly");
      return;
    }
    gClientActive = true;
    if (wantSnap) serveSnapshot(c);
    else serveStream(c);
    gClientActive = false;
  } else {
    sendIndex(c);
  }
}

void httpTask(void*) {
  while (!wifi_prov::connected()) vTaskDelay(pdMS_TO_TICKS(500));
  // Round-4: tiny listen backlog (1) so concurrent connections can't pile up
  // in lwIP consuming internal RAM while a serve is in progress — extras get
  // refused at the TCP layer and the client retries.
  WiFiServer server(kHttpPort, kHttpBacklog);
  server.begin();
  server.setNoDelay(true);
  telnet_log::logf("[cam] http server up on :%u (backlog %u)", kHttpPort, kHttpBacklog);
  for (;;) {
    WiFiClient c = server.accept();
    if (!c) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    handleClient(c);
    gClientActive = false;  // belt-and-braces if a serve path threw early
    c.stop();
    telnet_log::logf("[cam] client done");
  }
}

}  // namespace

void begin() {
  if (gBegun) return;
  gBegun = true;
  gWireMux = xSemaphoreCreateMutex();  // before any task could contend
  gInitOk = initHw();
  if (!gInitOk) {
    telnet_log::logf("[cam] init FAILED - camera disabled for this boot");
    return;
  }
  // Capture + HTTP + AE live on core 1 (the UI task owns core 0).
  xTaskCreatePinnedToCore(captureTask, "cam_cap", 4096, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(httpTask, "cam_http", 8192, nullptr, 1, nullptr, 1);
  xTaskCreatePinnedToCore(aeTask, "cam_ae", 4096, nullptr, 1, nullptr, 1);
}

void wireLock() {
  if (gWireMux) xSemaphoreTake(gWireMux, portMAX_DELAY);
}
void wireUnlock() {
  if (gWireMux) xSemaphoreGive(gWireMux);
}

void setEnabled(bool on) {
  if (gEnabled == on) return;
  gEnabled = on;
  telnet_log::logf("[cam] %s via privacy switch", on ? "ENABLED" : "DISABLED");
}

bool enabled() { return gEnabled && gInitOk; }
bool clientActive() { return gClientActive; }
uint32_t frames() { return gFrames; }

}  // namespace remote_camera
