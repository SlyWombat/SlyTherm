// ui_shared.h — internal shared state + helpers for the split wall UI
// (issue #114). This header is PRIVATE to src/ui/ + slytherm_ui.cpp; the
// public API stays slytherm_ui.h. It declares the palette, the model/mutex
// access, the cross-module widget handles and mode flags, the small styling
// helpers, and the extern "C" hooks each entrypoint provides.
//
// Module map (who defines what):
//   ui_shared.cpp   — the shared globals below + the small helpers
//   ui_main.cpp     — top bar, nav, the six tabs, renderMain, sysGraphSample
//   ui_overlays.cpp — keypad, hold/vacation sheets, WiFi setup, Home-system
//                     (broker) screen, RS-485 sniff screen, screenshot server
//   ui_modes.cpp    — ambient, Welcome, boot splash, safe UI
//   ui_port_*.cpp   — board display/touch/backlight (see ui_port.h)
//   slytherm_ui.cpp — begin()/service() orchestration only
#pragma once

#include <lvgl.h>

#include "slytherm_ui.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

using namespace dettson;
using namespace dettson::ui;

// ---- design-system palette (docs/09) ----
#define COL_BG       0x0B0F14
#define COL_CARD     0x151C25
#define COL_RAISED   0x1F2A36
#define COL_INK      0xEAF0F6
#define COL_MUTED    0xAEB9C4
#define COL_TEXT3    0x7A8895
#define COL_EMBER    0xFF7A18
#define COL_EMBER_HI 0xFFB020   // accent.heat.hi (gradient end)
#define COL_CRYO     0x38BDF8
#define COL_CRYO_HI  0x7DD3FC   // accent.cool.hi (gradient end)
#define COL_OK       0x37D39A
#define COL_WARN     0xF2C14E
#define COL_CRIT     0xFF5D5D
#define COL_BORDER   0x2B3947   // hairline dividers / demoted card borders (docs/09)
#define COL_DIM      0x3E4650   // ambient neutral
#define COL_DIM_EMB  0x8A4A18
#define COL_DIM_CRY  0x26647F

// ---- fonts + logo (generated .c files in src/) ----
LV_FONT_DECLARE(font_now80);   // 128px Montserrat-Thin subset (0-9 . ° -) for the hero
LV_FONT_DECLARE(font_set48);   // 48px Montserrat-Bold subset (0-9 . ° -) for setpoints
extern "C" const lv_img_dsc_t slymark_img;  // assets/slytherm-mark.svg -> slymark_img.c

// ---- hooks each entrypoint provides (main_thermostat.cpp / main_remote.cpp) ----
extern "C" void uiToggleClock24();           // flip+persist 12/24h (#69)
extern "C" bool uiClock24();
extern "C" void uiToggleSensor(const char* name);  // flip room participation (#68)
// #128 Fan settings — mode + circulate minutes/speed. Controller applies
// locally (globals + NVS + MQTT state); Remote forwards to the Controller via
// the fan cmd MQTT topics. FanMode is a uint8 (0=auto,1=on,2=circulate) so the
// shared UI needn't depend on lib/HaMqtt. Speed pct is one of 25/50/75.
extern "C" uint8_t  uiFanMode();
extern "C" void     uiSetFanMode(uint8_t mode);
extern "C" uint32_t uiFanCircMin();                // circulate minutes-per-hour
extern "C" uint8_t  uiFanCircPct();                // circulate speed pct (25/50/75)
extern "C" void     uiSetFanCirculate(uint32_t minPerHour, uint8_t pct);
extern "C" void uiSniffStart();              // RS-485 LISTEN capture (#71)
extern "C" void uiSniffStop();
extern "C" bool uiSniffActive();
extern "C" uint32_t uiSniffFrames();
extern "C" int uiSniffLines(char out[10][56]);     // newest-first; returns count
extern "C" void uiClearReducedMode();              // clear the #80 safe-UI latch + reboot
extern "C" void uiNoteTouch();                     // #90 sleep-state touch note (press edge)

namespace slytherm_ui {

// ---- timing constants shared by service() + the mode screens ----
constexpr uint32_t kIdleMs = 5u * 60u * 1000u;  // 5 min -> ambient (docs/09 §8)
// #86a: night-only deep screensaver — see ui_modes.cpp for the full story.
constexpr uint32_t kNightBlankIdleMs = 15u * 60u * 1000u;  // 15 min idle -> night blank
constexpr uint32_t kAmbShiftMs = 15u * 60u * 1000u;        // ambient burn-in drift cadence (#70)

// ---- shared model + mutex ----
extern UiModel*          gM;
extern SemaphoreHandle_t gMux;
inline void L(){ if(gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
inline void U(){ if(gMux) xSemaphoreGive(gMux); }

// ---- cross-module screens, widgets and mode flags (defined in ui_shared.cpp) ----
extern bool gReduced;              // #80 reduced safe-UI latch
extern lv_obj_t *scrMain, *scrAmb, *scrBoot, *scrWelcome, *gTabview;
extern lv_obj_t *wifiOv;           // WiFi-setup overlay (service hides it on connect)
extern bool gAmbient;
extern bool gBlanked;              // #86 deep-screensaver latch
extern bool gWelcomeActive;        // #82 first-run onboarding gate
extern bool gBootActive, gBootExiting;   // #92 warm-up splash gate
extern uint32_t gBootStartMs;
extern uint32_t gAmbShiftMs, gGraphLastMs;
extern uint8_t  gAmbShiftIdx;
extern bool gWifiOpen, gSrvOpen, gSniffOpen;

// ---- small helpers (ui_shared.cpp) ----
uint32_t nowS();
const char* modeName(UserMode m);
const char* actName(HvacAction a);
uint32_t actCol(HvacAction a);
void setTxt(lv_obj_t* o, const char* t);
const char* friendlyAlarm(const char* raw);   // #66 homeowner-friendly alarm text
lv_obj_t* card(lv_obj_t* p);
lv_obj_t* header(lv_obj_t* tab, const char* t);
lv_obj_t* mkBtn(lv_obj_t* p, const char* t, lv_event_cb_t cb, lv_align_t al, int x, int y, uint32_t bg, int w = 140);
void eyebrow(lv_obj_t* l);         // tracked-out caption style (docs/09)
bool uiLocked();                   // locked = read-only (screen lock, #45)

// ---- cross-module functions ----
// builders (called from buildUi/begin)
void buildUi();                    // ui_main.cpp — full tabbed UI + overlays + modes
void buildSafeUi();                // ui_modes.cpp — #80 reduced safe screen
void buildKeypad(lv_obj_t* scr);   // ui_overlays.cpp
void buildWifi(lv_obj_t* scr);
void buildServer(lv_obj_t* scr);
void buildHoldSheet(lv_obj_t* scr);
void buildVacationSheet(lv_obj_t* scr);
void buildFanSheet(lv_obj_t* scr);   // #128 Fan settings sheet (ui_overlays.cpp)
// Settings information-architecture reorg (#128/settings): the flat Settings
// button list becomes category cards that drill into these grouping sub-sheets.
// Each is a centered card overlay (same grammar as the Fan/Vacation sheets),
// built once here and opened from the Settings tab's category cards.
void buildNetSheet(lv_obj_t* scr);       // Networking: WiFi setup + Home system + read-only facts
void buildDisplaySheet(lv_obj_t* scr);   // Display: 12/24h clock (room for brightness/theme)
void buildSecuritySheet(lv_obj_t* scr);  // Security: Set PIN / Lock / Unlock
void buildSystemSheet(lv_obj_t* scr);    // System: firmware id + OTA stub + restore-full-screen
void buildSniff();
void buildAmbient();               // ui_modes.cpp
void buildWelcome();
void buildBoot();
// renderers (called from service())
void renderMain(const DisplayState& s);      // ui_main.cpp
void fillPresenceLine(const DisplayState& s, char* b, size_t n);  // #88 presence line (Home + ambient)
void sysGraphSample(const DisplayState& s);
void renderWifi();                           // ui_overlays.cpp
void renderServer();
void renderSniff();
void screenshotPoll();
void renderAmbient(const DisplayState& s);   // ui_modes.cpp
void renderBoot(const DisplayState& s);
void renderSafe(const DisplayState& s);
void ambientShift(int step);
void bootExit();
// cross-module event handlers / openers
enum class KpMode{Set,Unlock};     // PIN keypad mode (ui_overlays.cpp owns the keypad)
void kpadOpen(KpMode m, const char* t);
void promptUnlock();               // ui_overlays.cpp (PIN keypad)
void holdEvt(lv_event_t*);         // ui_overlays.cpp — open hold chooser (#81)
void vacOpen(lv_event_t*);         // ui_overlays.cpp — open vacation sheet (#78)
void openFan(lv_event_t*);         // ui_overlays.cpp — open Fan settings sheet (#128)
void openNet(lv_event_t*);         // ui_overlays.cpp — Networking sub-sheet (Settings reorg)
void openDisplay(lv_event_t*);     // ui_overlays.cpp — Display sub-sheet
void openSecurity(lv_event_t*);    // ui_overlays.cpp — Security sub-sheet
void openSystem(lv_event_t*);      // ui_overlays.cpp — System sub-sheet
void openMode(lv_event_t*);        // ui_main.cpp — System Mode sub-sheet (Off/Heat/Cool/Auto moved off Home)
lv_obj_t* sheetShell(lv_obj_t*scr,int w,int h,const char*title,const char*sub);  // ui_overlays.cpp — shared category-sheet shell
void openSniff(lv_event_t*);       // ui_overlays.cpp — RS-485 LISTEN screen (#71)
void openWifi(lv_event_t*);        // ui_overlays.cpp
void openServer(lv_event_t*);      // ui_overlays.cpp

}  // namespace slytherm_ui
