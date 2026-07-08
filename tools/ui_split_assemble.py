#!/usr/bin/env python3
# One-shot assembler for the #114 slytherm_ui.cpp split. Slices exact line
# ranges out of the pre-split monolith so the moved code is byte-identical,
# stitching in only the new includes/namespace seams. Deleted after use
# (kept in-tree during review so the split is auditable/re-runnable).
import sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "slytherm_ui.cpp")
with open(SRC, encoding="utf-8") as f:
    orig = f.readlines()  # orig[0] is line 1

def rng(a, b, skip=()):
    out = []
    for n in range(a, b + 1):
        if n in skip:
            continue
        out.append(orig[n - 1])
    return "".join(out)

def write(path, parts):
    p = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as f:
        f.write("".join(parts))
    print(f"wrote {path}: {sum(s.count(chr(10)) for s in parts)} lines")

# ---------------- ui_port_s3.cpp ----------------
write("src/ui/ui_port_s3.cpp", [
"""// ui_port_s3.cpp — Controller board port (#114): Waveshare ESP32-S3 4.3B.
// LovyanGFX RGB panel + CH422G IO-expander (backlight/reset) + GT911 raw I2C
// @0x5D — the validated stage-1 stack, moved verbatim from the pre-split
// slytherm_ui.cpp. See ui_port.h for the contract.

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include <lvgl.h>

#include "ui_port.h"

extern "C" void uiNoteTouch();   // #90 sleep-state touch note (press edge)

""",
rng(38, 57),   # LGFX class
"""
namespace slytherm_ui {
namespace {

""",
rng(100, 118),  # CH422G + GT911
"""
""",
rng(120, 129),  # LVGL glue: gfx, buffers, flushCb, touchCb
"""
}  // namespace

bool portInit(){
  Wire.begin(kSda,kScl); Wire.setClock(400000);
  ch422M(0x01); ch422O(kBitTpRst|kBitLcdRst); delay(20);
  gfx.init(); gfx.setColorDepth(16); gfx.fillScreen(0);
  ch422O(kBitTpRst|kBitLcdRst|kBitBl);
  gtReset(); Wire.beginTransmission(kGt); gTouchOk=(Wire.endTransmission()==0);
  Serial.printf("[ui] GT911 %s\\n", gTouchOk?"present":"NO ACK");
  lv_init();
  lv_disp_draw_buf_init(&drawBuf,buf1,nullptr,800*40);
  lv_disp_drv_init(&dispDrv); dispDrv.hor_res=800; dispDrv.ver_res=480; dispDrv.flush_cb=flushCb; dispDrv.draw_buf=&drawBuf; lv_disp_drv_register(&dispDrv);
  lv_indev_drv_init(&indDrv); indDrv.type=LV_INDEV_TYPE_POINTER; indDrv.read_cb=touchCb; lv_indev_drv_register(&indDrv);
  return true;
}

// #86: the panel backlight is CH422G bit kBitBl — ON/OFF only (no PWM pin on
// the 4.3B); portBacklight(false) leaves the GT911 alive so touch still wakes.
void portBacklight(bool on){ if(on) ch422O(gCh|kBitBl); else ch422O(gCh&~kBitBl); }

bool portTouchOk(){ return gTouchOk; }

}  // namespace slytherm_ui
""",
])

# ---------------- ui_main.cpp ----------------
write("src/ui/ui_main.cpp", [
"""// ui_main.cpp — the tabbed main screen (#114): top bar, pull-down nav, the
// six tabs (Home/Presets/Sensors/System/Settings/Diag) with their event
// handlers, renderMain, and the 12 h trend graph. Moved verbatim from the
// pre-split slytherm_ui.cpp.

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "ui_shared.h"
#include "mqtt_cfg.h"

// #113: injected by tools/version_flag.py; fallback keeps ad-hoc builds compiling.
#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

namespace slytherm_ui {

""",
rng(140, 166),  # main-screen widget handles + sensor rows + graph + nav globals
"""lv_obj_t *modeBtns[4];
lv_obj_t *gBtnListen=nullptr;   // Diag LISTEN button — render-gated on DisplayState.hasBus (#101)
""",
rng(215, 230, skip={217, 218}),  # home event handlers (promptUnlock/uiLocked now in ui_shared)
"""
""",
rng(234, 239),  # spBtn
"""
""",
rng(244, 290),   # buildHome
"""
""",
rng(353, 362),   # presetLabel
"""
""",
rng(364, 391),   # buildPresets
"""
""",
rng(393, 423),   # sensors tab
rng(424, 440),   # buildSystem
rng(441, 443),   # buildDiag
"""
""",
rng(495, 520, skip={499, 500}),  # settings events + buildSettings (openWifi/openServer decls in ui_shared.h)
"""
""",
rng(841, 874),   # pull-down nav + top bar
"""
""",
rng(924, 942),   # buildUi
"""
""",
rng(944, 960),   # layoutCard
"""
""",
rng(962, 994),   # fillPresenceLine
"""
""",
rng(996, 1111),  # renderMain
"""
""",
rng(1129, 1146),  # sysGraphSample
"""
}  // namespace slytherm_ui
""",
])

# ---------------- ui_overlays.cpp ----------------
write("src/ui/ui_overlays.cpp", [
"""// ui_overlays.cpp — overlays + auxiliary screens (#114): PIN keypad, hold
// chooser (#81), vacation sheet (#78), WiFi setup flow, Home-system (broker)
// screen, RS-485 LISTEN capture screen (#71), and the screenshot server.
// Moved verbatim from the pre-split slytherm_ui.cpp.

#include <Arduino.h>
#include <WiFi.h>
#include <cstring>

#include "ui_shared.h"
#include "wifi_prov.h"
#include "mqtt_cfg.h"
#include "telnet_log.h"

namespace slytherm_ui {

""",
rng(182, 182),
"""lv_obj_t *taPass=nullptr, *taSsid=nullptr, *kbd=nullptr, *wfLine=nullptr, *wfSub=nullptr;
""",
rng(184, 198, skip={186, 191, 194}),  # gWifiOpen/gSrvOpen/gSniffOpen now in ui_shared
"""
""",
rng(292, 351),   # vacation sheet
"""
""",
rng(445, 470),   # PIN keypad
"""
""",
rng(472, 493),   # hold-duration chooser
"""
""",
rng(603, 734, skip={606, 607, 608, 609, 610}),  # WiFi setup flow (mkBtn def now in ui_shared)
"""
""",
rng(736, 772),   # Home-system (broker) screen
"""
""",
rng(774, 806),   # RS-485 LISTEN screen
"""
""",
rng(808, 839),   # screenshot server
"""
}  // namespace slytherm_ui
""",
])

# ---------------- ui_modes.cpp ----------------
write("src/ui/ui_modes.cpp", [
"""// ui_modes.cpp — full-screen modes (#114): ambient idle screen (+burn-in
// drift and touch wake), first-run Welcome (#82), animated boot splash (#92),
// and the reduced safe UI (#80). Moved verbatim from the pre-split
// slytherm_ui.cpp; the backlight bit-bang became portBacklight() (ui_port.h).

#include <Arduino.h>
#include <cstring>

#include "ui_shared.h"
#include "ui_port.h"

namespace slytherm_ui {

// Reduced safe-UI (issue #80) widgets
lv_obj_t *scrSafe=nullptr,*wSafeTemp=nullptr,*wSafeMode=nullptr,*wSafeAlarm=nullptr;
// #92 boot splash widgets (scrBoot itself is shared — begin() loads it)
lv_obj_t *gBootMark=nullptr,*wBootStat=nullptr,*bcWifi=nullptr,*bcMqtt=nullptr,*bcOat=nullptr,*bcRoom=nullptr;
""",
rng(180, 181),   # ambient widgets
"""
""",
rng(522, 584, skip={523, 531, 532}),  # ambient (renderMain decl + kAmbShiftMs now in ui_shared.h)
"""
""",
rng(586, 601),   # Welcome
"""
""",
rng(876, 922),   # boot splash
"""
""",
rng(1113, 1127),  # renderAmbient
"""
""",
rng(1148, 1164),  # safe UI
"""
}  // namespace slytherm_ui
""",
])

print("done")
