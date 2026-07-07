// slytherm_ui.cpp — LVGL wall-UI binding. See slytherm_ui.h. Renders the shared
// UiModel and routes touch into it, all under the caller's mutex. Validated
// display/touch stack (LovyanGFX RGB, GT911 raw @0x5D, pclk_idle_high=1);
// palette per docs/09; ambient idle screen per docs/09 §8.

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <Wire.h>
#include <WiFi.h>
#include <lvgl.h>

#include "slytherm_ui.h"
#include "SleepState.h"   // #90: shared night-window constants (kSleepStartHour/EndHour)
#include "wifi_prov.h"
#include "mqtt_cfg.h"
#include "telnet_log.h"

using namespace dettson;
using namespace dettson::ui;

extern "C" const lv_img_dsc_t slymark_img;  // generated from assets/slytherm-mark.svg (slymark_img.c)
extern "C" void uiToggleClock24();           // main_thermostat.cpp: flip+persist 12/24h (#69)
extern "C" bool uiClock24();
extern "C" void uiToggleSensor(const char* name);  // main_thermostat.cpp: flip room participation (#68)
extern "C" void uiSniffStart();              // main_thermostat.cpp: RS-485 LISTEN capture (#71)
extern "C" void uiSniffStop();
extern "C" bool uiSniffActive();
extern "C" uint32_t uiSniffFrames();
extern "C" int uiSniffLines(char out[10][56]);     // newest-first; returns count
extern "C" void uiClearReducedMode();              // main_thermostat.cpp: clear the #80 safe-UI latch + reboot
extern "C" void uiNoteTouch();                     // main_thermostat.cpp: #90 sleep-state touch note (press edge)
LV_FONT_DECLARE(font_now80);                 // 128px Montserrat-Thin subset (0-9 . ° -) for the hero (font_now80.c)
LV_FONT_DECLARE(font_set48);                 // 48px Montserrat-Bold subset (0-9 . ° -) for setpoint values (font_set48.c)

// ---- LovyanGFX RGB panel (validated) ---------------------------------------
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB   _bus;
  lgfx::Panel_RGB _panel;
 public:
  LGFX(){
    { auto c=_panel.config(); c.memory_width=800; c.memory_height=480;
      c.panel_width=800; c.panel_height=480; c.offset_x=0; c.offset_y=0; _panel.config(c); }
    { auto c=_panel.config_detail(); c.use_psram=1; _panel.config_detail(c); }
    { auto c=_bus.config(); c.panel=&_panel;
      c.pin_d0=14;c.pin_d1=38;c.pin_d2=18;c.pin_d3=17;c.pin_d4=10;
      c.pin_d5=39;c.pin_d6=0;c.pin_d7=45;c.pin_d8=48;c.pin_d9=47;c.pin_d10=21;
      c.pin_d11=1;c.pin_d12=2;c.pin_d13=42;c.pin_d14=41;c.pin_d15=40;
      c.pin_henable=5;c.pin_vsync=3;c.pin_hsync=46;c.pin_pclk=7;c.freq_write=14000000;
      c.hsync_polarity=0;c.hsync_front_porch=20;c.hsync_pulse_width=10;c.hsync_back_porch=10;
      c.vsync_polarity=0;c.vsync_front_porch=10;c.vsync_pulse_width=10;c.vsync_back_porch=10;
      c.pclk_active_neg=0;c.de_idle_high=0;c.pclk_idle_high=1; _bus.config(c); }
    _panel.setBus(&_bus); setPanel(&_panel);
  }
};

namespace slytherm_ui {
namespace {

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

constexpr uint32_t kIdleMs = 5u * 60u * 1000u;  // 5 min -> ambient (docs/09 §8)
// #86: the panel backlight is CH422G bit kBitBl — ON/OFF only (no PWM pin on the
// 4.3B), so we can't analog-dim. Instead: darker ambient theme at 5 min, and a
// NIGHT-ONLY deep screensaver that fully BLANKS the backlight overnight.
// #86a: only blank between 00:00-06:00 local after this idle; outside that window
// the ambient screen stays fully lit forever (never blank during the day).
constexpr uint32_t kNightBlankIdleMs = 15u * 60u * 1000u;  // 15 min idle -> night blank

// Reduced safe-UI (issue #80): built instead of the full UI after a reset-loop
// latch, so a crash in an optional widget can't boot-loop the panel.
bool gReduced=false;
lv_obj_t *scrSafe=nullptr,*wSafeTemp=nullptr,*wSafeMode=nullptr,*wSafeAlarm=nullptr;

// ---- shared model + mutex ----
UiModel*          gM   = nullptr;
SemaphoreHandle_t gMux = nullptr;
inline void L(){ if(gMux) xSemaphoreTake(gMux, portMAX_DELAY); }
inline void U(){ if(gMux) xSemaphoreGive(gMux); }

// ---- CH422G + GT911 (validated) ----
constexpr uint8_t kCh422Mode=0x24, kCh422Out=0x38, kBitTpRst=1<<1, kBitBl=1<<2, kBitLcdRst=1<<3;
uint8_t gCh=0;
void ch422M(uint8_t v){ Wire.beginTransmission(kCh422Mode); Wire.write(v); Wire.endTransmission(); }
void ch422O(uint8_t v){ gCh=v; Wire.beginTransmission(kCh422Out); Wire.write(v); Wire.endTransmission(); }
constexpr int kSda=8,kScl=9,kInt=4; constexpr uint8_t kGt=0x5D; bool gTouchOk=false;
void gtReset(){ pinMode(kInt,OUTPUT); digitalWrite(kInt,LOW);
  ch422O(gCh&~kBitTpRst); delay(10); digitalWrite(kInt,LOW); delayMicroseconds(120);
  ch422O(gCh|kBitTpRst); delay(5); digitalWrite(kInt,LOW); delay(50);
  pinMode(kInt,INPUT); delay(50); }
int gtReg(uint16_t r,uint8_t*b,uint8_t n){ Wire.beginTransmission(kGt); Wire.write(r>>8); Wire.write(r&0xFF);
  if(Wire.endTransmission(false)!=0) return 0; uint8_t g=Wire.requestFrom(kGt,n),i=0;
  while(i<g&&Wire.available()) b[i++]=Wire.read(); return i; }
bool gtRead(uint16_t&x,uint16_t&y){ static uint16_t lx=0,ly=0; static bool dn=false; static uint32_t readyMs=0; uint8_t s=0;
  if(gtReg(0x814E,&s,1)==1 && (s&0x80)){ readyMs=millis();
    if((s&0x0F)>0){ uint8_t d[6]={0}; if(gtReg(0x8150,d,6)>=4){ lx=d[0]|(d[1]<<8); ly=d[2]|(d[3]<<8); dn=true; } } else dn=false;
    Wire.beginTransmission(kGt); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission(); }
  else if(dn && millis()-readyMs>150) dn=false;  // no fresh GT911 sample -> released (frees LVGL idle timer)
  x=lx; y=ly; return dn; }

// ---- LVGL glue ----
LGFX gfx;
lv_disp_draw_buf_t drawBuf; lv_color_t buf1[800*40];
lv_disp_drv_t dispDrv; lv_indev_drv_t indDrv;
void flushCb(lv_disp_drv_t*d,const lv_area_t*a,lv_color_t*px){
  gfx.pushImage(a->x1,a->y1,a->x2-a->x1+1,a->y2-a->y1+1,(lgfx::rgb565_t*)px); lv_disp_flush_ready(d); }
void touchCb(lv_indev_drv_t*,lv_indev_data_t*dt){ uint16_t x,y; static bool wasDown=false;
  if(gTouchOk&&gtRead(x,y)){ dt->state=LV_INDEV_STATE_PR; dt->point.x=x; dt->point.y=y;
    if(!wasDown){ wasDown=true; uiNoteTouch(); } }   // #90: press edge -> sleep-state wake
  else { dt->state=LV_INDEV_STATE_REL; wasDown=false; } }

// ---- screens ----
lv_obj_t *scrMain=nullptr, *scrAmb=nullptr, *gTabview=nullptr;
bool gAmbient=false;
bool gBlanked=false;  // #86: deep-screensaver latch — backlight bit cleared, restore on wake
bool gWelcomeActive=false;  // #82: first-run onboarding gate (no saved WiFi)
bool gBootActive=false,gBootExiting=false; uint32_t gBootStartMs=0;  // #92: warm-up splash gate (hold UI until key sensors live, <=60s)
lv_obj_t *scrBoot=nullptr,*gBootMark=nullptr,*wBootStat=nullptr,*bcWifi=nullptr,*bcMqtt=nullptr,*bcOat=nullptr,*bcRoom=nullptr;
lv_obj_t *scrWelcome=nullptr;  // #82: standalone Welcome screen (like scrAmb)
uint32_t gAmbShiftMs=0; uint8_t gAmbShiftIdx=0;  // ambient burn-in pixel-shift ring (#70)
// main-screen widgets
lv_obj_t *wTemp,*wDeg=nullptr,*wAction,*wHeatSp,*wCoolSp,*wWifi,*wMqtt,*wBus,*wOat,*wClock,*wSysBody,*wDiagBody,*wLockState;
lv_obj_t *gHomeTab=nullptr;  // Home tab page — parent of the hero, for the mode-tinted bg gradient (#fix5)
// Sensors screen: interactive per-room rows (#68)
struct SensorRowUi{ lv_obj_t*row,*name,*temp,*pres,*btn,*btnlbl; }; SensorRowUi gSensorRows[7]={};
// #89 column geometry (x within the 760-wide row): Name | Temperature | Presence | toggle
#define SR_NAME_X   12
#define SR_NAME_W   244
#define SR_TEMP_X   262
#define SR_TEMP_W   90
#define SR_PRES_X   372
#define SR_PRES_W   260
char gRowName[7][16]={};
lv_obj_t *wFollow,*gHeatCard,*gCoolCard,*wOffMsg,*wOnline,*gPresetBtns[kMaxPresets]={};  // UI v2 Home/Presets
lv_obj_t *gPresetName[kMaxPresets]={},*gPresetVal[kMaxPresets]={};  // #74: live-roster card labels
lv_obj_t *gHoldBtn=nullptr,*gHoldLbl=nullptr;   // Home hold pill (#81): shows active hold, opens the chooser
lv_obj_t *gVacHome=nullptr;                      // Home vacation banner (#78): "Vacation until <date>"
lv_obj_t *gHoldSheet=nullptr;                    // hold-duration chooser overlay (#81)
lv_obj_t *gClkLbl=nullptr,*wSetWifi=nullptr,*wSetHome=nullptr;   // #77: Settings clock toggle + WiFi/Home-system status words
// System 12 h trend graph (#76): ~5-min RAM ring, 144 pts (temps x10 as lv_coord_t).
constexpr int kGraphPts=144;
lv_obj_t *gSysChart=nullptr,*wSysGraphLbl=nullptr;
lv_chart_series_t *gSerActual=nullptr,*gSerHeat=nullptr,*gSerCool=nullptr;
lv_coord_t gRingA[kGraphPts],gRingH[kGraphPts],gRingC[kGraphPts];  // actual / heat-set / cool-set
uint32_t gGraphLastMs=0;
lv_obj_t *gNavMenu=nullptr,*wCaret=nullptr;  // pull-down navigation
// #66: map an internal alarm string to homeowner-friendly text (wall UI + shared idea for HA).
const char* friendlyAlarm(const char* raw){
  if(!raw||!raw[0]) return "Attention needed";
  if(strstr(raw,"OAT")||strstr(raw,"utdoor")) return "Outdoor temp unavailable - heat pump paused, using gas";
  if(strstr(raw,"ompressor")&&(strstr(raw,"lock")||strstr(raw,"ock"))) return "Heat pump locked out - waiting for outdoor temp";
  if(strstr(raw,"min-off")||strstr(raw,"min off")) return "Compressor resting before it can restart";
  if(strstr(raw,"tale")) return "A room sensor stopped reporting - check it";
  if(strstr(raw,"egrad")) return "Using the local sensor only - remotes offline";
  if(strstr(raw,"oot")||strstr(raw,"race")) return "Warming up - waiting for a temperature";
  if(strstr(raw,"o source")||strstr(raw,"ll bad")||strstr(raw,"o demand")) return "No room temperature - heating/cooling paused";
  return raw;
}
lv_obj_t *modeBtns[4];
// ambient widgets
lv_obj_t *aName,*aTemp,*aTarget,*aAlarm,*aNow=nullptr;
// WiFi setup — state machine (one screen per state; flow per docs/09 + mockup)
lv_obj_t *wifiOv=nullptr, *taPass=nullptr, *taSsid=nullptr, *kbd=nullptr, *wfLine=nullptr, *wfSub=nullptr;
enum class WifiState { Status, Scanning, List, Password, Other, Connecting, ResultOk, ResultFail };
WifiState gWs=WifiState::Status;
bool gWifiOpen=false;
char gSelSsid[33]={};
char gNetSsids[wifi_prov::kMaxNets][33]={};
// Home-system (MQTT broker) manual entry — Installer/Advanced (mqtt_cfg)
lv_obj_t *srvOv=nullptr,*taHost=nullptr,*taPort=nullptr,*taUser=nullptr,*taSrvPass=nullptr,*kbdS=nullptr,*lblSrvStat=nullptr;
bool gSrvOpen=false;
// RS-485 LISTEN capture screen (#71) — dedicated top-level screen (like scrAmb)
lv_obj_t *scrSniff=nullptr,*wSniffStat=nullptr,*wSniffCount=nullptr,*wSniffList=nullptr,*wSniffFoot=nullptr;
bool gSniffOpen=false;
// keypad
lv_obj_t *kpad=nullptr,*kpadTitle=nullptr,*kpadDots=nullptr;
enum class KpMode{Set,Unlock}; KpMode kpMode=KpMode::Set; uint8_t kpBuf[4]; int kpN=0;
constexpr uint32_t kPinSalt=0x5A17C0DE;
uint32_t nowS(){ return millis()/1000; }

const char* modeName(UserMode m){ switch(m){case UserMode::kHeat:return "HEAT";case UserMode::kCool:return "COOL";
  case UserMode::kAuto:return "AUTO";case UserMode::kEmergencyHeat:return "EM HEAT";default:return "OFF";} }
const char* actName(HvacAction a){ switch(a){case HvacAction::kHeating:return "Heating";case HvacAction::kCooling:return "Cooling";
  case HvacAction::kFanOnly:return "Fan";case HvacAction::kDefrosting:return "Defrost";default:return "Idle";} }
uint32_t actCol(HvacAction a){ switch(a){case HvacAction::kHeating:case HvacAction::kDefrosting:return COL_EMBER;
  case HvacAction::kCooling:return COL_CRYO;default:return COL_MUTED;} }

void setTxt(lv_obj_t*o,const char*t){ const char*c=lv_label_get_text(o); if(c&&strcmp(c,t)==0)return; lv_label_set_text(o,t); }
lv_obj_t* mkBtn(lv_obj_t*,const char*,lv_event_cb_t,lv_align_t,int,int,uint32_t,int);  // defined below; default width lives on the definition
void openSniff(lv_event_t*);  // RS-485 LISTEN screen (#71), defined below; used by buildDiag
void holdEvt(lv_event_t*);    // open the hold-duration chooser (#81), defined below; used by buildHome
// tracked-out caption ("eyebrow"): small Montserrat + wide letter-spacing (docs/09)
void eyebrow(lv_obj_t*l){ lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0); lv_obj_set_style_text_letter_space(l,2,0); }

// ---- event handlers (touch runs on the UI task; take the mutex around model) ----
int gHoldReps=0;
void promptUnlock();                         // defined with the keypad below
inline bool uiLocked(){ L(); bool lk=gM->lockState()!=LockState::kUnlocked; U(); return lk; }  // locked = read-only
void spEvt(lv_event_t*e){ const lv_event_code_t c=lv_event_get_code(e);
  if(uiLocked()){ if(c==LV_EVENT_PRESSED) promptUnlock(); return; }   // locked: pressing a value prompts to unlock
  const int d=(int)(intptr_t)lv_event_get_user_data(e);
  const SetpointSide side=(d==1||d==-1)?SetpointSide::kHeat:SetpointSide::kCool; const float base=(d>0)?+0.5f:-0.5f;
  if(c==LV_EVENT_PRESSED){ gHoldReps=0; return; }
  float step=base; if(c==LV_EVENT_LONG_PRESSED_REPEAT && ++gHoldReps>12) step=base*2.0f;
  L(); gM->adjustSetpoint(side,step); U(); }
void modeEvt(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; } UserMode m=(UserMode)(intptr_t)lv_event_get_user_data(e); L(); gM->setMode(m); U(); }
int gPresetSel=-1; uint32_t gPresetSelMs=0;   // #75: optimistic active-card highlight until the setpoints round-trip
void presetEvt(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; } int idx=(int)(intptr_t)lv_event_get_user_data(e);  // #74: card == roster index
  gPresetSel=idx; gPresetSelMs=millis();     // move the highlight NOW, before the control task echoes the setpoints back
  L(); gM->setPreset((Preset)idx); U(); }

lv_obj_t* card(lv_obj_t*p){ lv_obj_t*c=lv_obj_create(p); lv_obj_set_style_bg_color(c,lv_color_hex(COL_CARD),0);
  lv_obj_set_style_border_width(c,0,0); lv_obj_set_style_radius(c,14,0); lv_obj_clear_flag(c,LV_OBJ_FLAG_SCROLLABLE); return c; }
lv_obj_t* spBtn(lv_obj_t*p,const char*t,int code,lv_align_t al,int xo=0,int yo=0){ lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,64,64);
  lv_obj_align(b,al,xo,yo); lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),0); void*u=(void*)(intptr_t)code;
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_PRESSED,u); lv_obj_add_event_cb(b,spEvt,LV_EVENT_SHORT_CLICKED,u);
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_LONG_PRESSED_REPEAT,u);
  const char*sym=(t[0]=='-'&&!t[1])?LV_SYMBOL_MINUS:((t[0]=='+'&&!t[1])?LV_SYMBOL_PLUS:t);  // real +/- glyphs, not off-center ASCII
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,sym); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l); return b; }
lv_obj_t* header(lv_obj_t*tab,const char*t){ lv_obj_t*h=lv_label_create(tab); lv_label_set_text(h,t);
  lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(h,lv_color_hex(COL_CRYO),0);
  lv_obj_align(h,LV_ALIGN_TOP_LEFT,4,0); return h; }

void buildHome(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); lv_obj_set_style_pad_all(tab,0,0);
  // hero: NOW + big current temp + action + presence (left); logo/status live in the top bar
  gHomeTab=tab;  // #fix5 handle for the mode-tinted bg gradient
  // hero aligned to the setpoint card: NOW sits just below the card top; the big temp is
  // top-justified with the card's value (mockup 04-home: hero + card center-aligned as a pair)
  lv_obj_t*nl=lv_label_create(tab); lv_label_set_text(nl,"NOW"); eyebrow(nl); lv_obj_set_style_text_color(nl,lv_color_hex(COL_TEXT3),0); lv_obj_align(nl,LV_ALIGN_TOP_LEFT,26,98);
  wTemp=lv_label_create(tab); lv_obj_set_style_text_font(wTemp,&font_now80,0); lv_obj_set_style_text_color(wTemp,lv_color_hex(COL_INK),0); lv_obj_align(wTemp,LV_ALIGN_TOP_LEFT,26,110);
  lv_obj_align_to(nl,wTemp,LV_ALIGN_OUT_TOP_LEFT,2,-10);  // NOW above the temp digits (negative y lifts it clear — the Thin font's digits sit at the line-box top, so a positive offset overwrote them)
  wDeg=lv_label_create(tab); lv_label_set_text(wDeg,"\xC2\xB0"); lv_obj_set_style_text_font(wDeg,&lv_font_montserrat_48,0); lv_obj_set_style_text_color(wDeg,lv_color_hex(COL_INK),0); lv_obj_align_to(wDeg,wTemp,LV_ALIGN_OUT_RIGHT_TOP,2,12);  // ° superscript (re-aligned each render — digits change width)
  wAction=lv_label_create(tab); lv_obj_set_style_text_font(wAction,&lv_font_montserrat_20,0); lv_obj_set_style_bg_opa(wAction,LV_OPA_TRANSP,0);
  lv_obj_align(wAction,LV_ALIGN_TOP_LEFT,26,270);
  wFollow=lv_label_create(tab); lv_obj_set_style_text_color(wFollow,lv_color_hex(COL_MUTED),0); lv_obj_align(wFollow,LV_ALIGN_TOP_LEFT,26,302);
  // hold pill (#81): tap to choose 2h/4h/until-next/forever or resume the schedule
  // pinned under the "Reading..." line so it clears the centered mode bar below (#fix: was overprinting)
  gHoldBtn=lv_btn_create(tab); lv_obj_set_size(gHoldBtn,258,28); lv_obj_align_to(gHoldBtn,wFollow,LV_ALIGN_OUT_BOTTOM_LEFT,0,6);
  lv_obj_set_style_bg_color(gHoldBtn,lv_color_hex(COL_RAISED),0); lv_obj_set_style_shadow_width(gHoldBtn,0,0); lv_obj_set_style_radius(gHoldBtn,10,0);
  lv_obj_add_event_cb(gHoldBtn,holdEvt,LV_EVENT_CLICKED,nullptr);
  gHoldLbl=lv_label_create(gHoldBtn); lv_label_set_text(gHoldLbl,"Set a hold"); lv_obj_set_style_text_font(gHoldLbl,&lv_font_montserrat_16,0); lv_obj_center(gHoldLbl);
  // #78: vacation banner pill across the top of Home, shown while a vacation is set
  gVacHome=lv_label_create(tab); lv_obj_set_style_text_font(gVacHome,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(gVacHome,lv_color_hex(COL_CRYO),0); lv_obj_set_style_bg_color(gVacHome,lv_color_hex(COL_RAISED),0);
  lv_obj_set_style_bg_opa(gVacHome,LV_OPA_COVER,0); lv_obj_set_style_pad_hor(gVacHome,16,0); lv_obj_set_style_pad_ver(gVacHome,6,0); lv_obj_set_style_radius(gVacHome,10,0);
  lv_obj_align(gVacHome,LV_ALIGN_TOP_MID,0,8); lv_label_set_text(gVacHome,""); lv_obj_add_flag(gVacHome,LV_OBJ_FLAG_HIDDEN);
  // heat + cool cards (right), big target font, shown per mode
  gHeatCard=card(tab); lv_obj_set_size(gHeatCard,340,170); lv_obj_align(gHeatCard,LV_ALIGN_TOP_RIGHT,-16,84); lv_obj_set_style_pad_all(gHeatCard,0,0);
  lv_obj_set_style_border_color(gHeatCard,lv_color_hex(COL_EMBER),0); lv_obj_set_style_border_width(gHeatCard,1,0); lv_obj_set_style_border_opa(gHeatCard,LV_OPA_40,0);
  { lv_obj_t*l=lv_label_create(gHeatCard); lv_label_set_text(l,"HEAT"); eyebrow(l); lv_obj_set_style_text_color(l,lv_color_hex(COL_EMBER),0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,10); }
  wHeatSp=lv_label_create(gHeatCard); lv_obj_set_style_text_font(wHeatSp,&font_set48,0); lv_obj_align(wHeatSp,LV_ALIGN_TOP_MID,0,34);
  spBtn(gHeatCard,"-",-1,LV_ALIGN_BOTTOM_MID,-76,-8); spBtn(gHeatCard,"+",1,LV_ALIGN_BOTTOM_MID,76,-8);
  gCoolCard=card(tab); lv_obj_set_size(gCoolCard,340,170); lv_obj_align(gCoolCard,LV_ALIGN_TOP_RIGHT,-16,84); lv_obj_set_style_pad_all(gCoolCard,0,0);
  lv_obj_set_style_border_color(gCoolCard,lv_color_hex(COL_CRYO),0); lv_obj_set_style_border_width(gCoolCard,1,0); lv_obj_set_style_border_opa(gCoolCard,LV_OPA_40,0);
  { lv_obj_t*l=lv_label_create(gCoolCard); lv_label_set_text(l,"COOL"); eyebrow(l); lv_obj_set_style_text_color(l,lv_color_hex(COL_CRYO),0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,10); }
  wCoolSp=lv_label_create(gCoolCard); lv_obj_set_style_text_font(wCoolSp,&font_set48,0); lv_obj_align(wCoolSp,LV_ALIGN_TOP_MID,0,34);
  spBtn(gCoolCard,"-",-2,LV_ALIGN_BOTTOM_MID,-76,-8); spBtn(gCoolCard,"+",2,LV_ALIGN_BOTTOM_MID,76,-8);
  wOffMsg=lv_label_create(tab); lv_label_set_text(wOffMsg,"System off\npick a mode to set a temperature");
  lv_obj_set_style_text_color(wOffMsg,lv_color_hex(COL_TEXT3),0); lv_obj_set_style_text_align(wOffMsg,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(wOffMsg,LV_ALIGN_TOP_RIGHT,-64,96);
  // mode selector across the reclaimed bottom
  const char*mn[4]={"OFF","HEAT","COOL","AUTO"}; UserMode mv[4]={UserMode::kOff,UserMode::kHeat,UserMode::kCool,UserMode::kAuto};
  lv_obj_t*mrow=lv_obj_create(tab); lv_obj_set_size(mrow,470,56); lv_obj_align(mrow,LV_ALIGN_BOTTOM_MID,0,-12);
  lv_obj_set_style_bg_color(mrow,lv_color_hex(COL_CARD),0); lv_obj_set_style_bg_opa(mrow,LV_OPA_COVER,0); lv_obj_set_style_border_width(mrow,0,0);
  lv_obj_set_style_radius(mrow,12,0); lv_obj_set_style_pad_all(mrow,4,0);  // #fix1: segmented control track, not floating pills
  lv_obj_set_flex_flow(mrow,LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(mrow,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(mrow,LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<4;i++){ lv_obj_t*b=lv_btn_create(mrow); lv_obj_set_size(b,108,48);
    lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0); lv_obj_set_style_shadow_width(b,0,0); lv_obj_set_style_radius(b,9,0);  // flat segment; renderMain fills the active one
    lv_obj_add_event_cb(b,modeEvt,LV_EVENT_CLICKED,(void*)(intptr_t)mv[i]);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,mn[i]); lv_obj_set_style_text_color(l,lv_color_hex(COL_MUTED),0); lv_obj_center(l); modeBtns[i]=b; } }

// ---- On-device Vacation sheet (issue #78) ----------------------------------
// Steppers for Start (days out), Length (nights), and eco heat/cool setpoints,
// with Start / Cancel. Sends a kSetVacation / kClearVacation intent; the control
// task anchors the window to the on-device clock and applies eco in-window.
lv_obj_t *gVacSheet=nullptr,*gVacStartLbl=nullptr,*gVacNightsLbl=nullptr,*gVacHeatLbl=nullptr,*gVacCoolLbl=nullptr;
int gVacStartDays=0, gVacNights=7; float gVacEcoHeat=16.0f, gVacEcoCool=28.0f;
void vacRefresh(){ if(!gVacSheet) return; char b[24];
  if(gVacStartDays==0) strcpy(b,"Today"); else snprintf(b,sizeof(b),"In %d day%s",gVacStartDays,gVacStartDays==1?"":"s"); setTxt(gVacStartLbl,b);
  snprintf(b,sizeof(b),"%d night%s",gVacNights,gVacNights==1?"":"s"); setTxt(gVacNightsLbl,b);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)gVacEcoHeat); setTxt(gVacHeatLbl,b);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)gVacEcoCool); setTxt(gVacCoolLbl,b); }
void vacStep(lv_event_t*e){ int c=(int)(intptr_t)lv_event_get_user_data(e); int field=c>>1; bool up=c&1;
  switch(field){
    case 0: gVacStartDays+=up?1:-1; if(gVacStartDays<0)gVacStartDays=0; if(gVacStartDays>30)gVacStartDays=30; break;
    case 1: gVacNights+=up?1:-1; if(gVacNights<1)gVacNights=1; if(gVacNights>60)gVacNights=60; break;
    case 2: gVacEcoHeat+=up?0.5f:-0.5f; if(gVacEcoHeat<7.0f)gVacEcoHeat=7.0f; if(gVacEcoHeat>gVacEcoCool-1.0f)gVacEcoHeat=gVacEcoCool-1.0f; break;
    case 3: gVacEcoCool+=up?0.5f:-0.5f; if(gVacEcoCool>35.0f)gVacEcoCool=35.0f; if(gVacEcoCool<gVacEcoHeat+1.0f)gVacEcoCool=gVacEcoHeat+1.0f; break;
  } vacRefresh(); }
void vacStart(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; }
  L(); gM->requestVacation((uint16_t)gVacStartDays,(uint16_t)gVacNights,gVacEcoHeat,gVacEcoCool); U();
  if(gVacSheet) lv_obj_add_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); }
void vacCancelHold(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; }
  L(); gM->cancelVacation(); U(); if(gVacSheet) lv_obj_add_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); }
void vacClose(lv_event_t*){ if(gVacSheet) lv_obj_add_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); }
void vacOpen(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; }
  if(!gVacSheet) return; vacRefresh(); lv_obj_clear_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gVacSheet); }
static lv_obj_t* vacRow(lv_obj_t*par,const char*name,int y,int field){
  lv_obj_t*nl=lv_label_create(par); lv_label_set_text(nl,name); lv_obj_set_style_text_color(nl,lv_color_hex(COL_MUTED),0);
  lv_obj_set_style_text_font(nl,&lv_font_montserrat_20,0); lv_obj_align(nl,LV_ALIGN_TOP_LEFT,18,y+12);
  lv_obj_t*minus=lv_btn_create(par); lv_obj_set_size(minus,46,44); lv_obj_align(minus,LV_ALIGN_TOP_RIGHT,-150,y);
  lv_obj_set_style_bg_color(minus,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(minus,vacStep,LV_EVENT_CLICKED,(void*)(intptr_t)(field*2+0));
  { lv_obj_t*l=lv_label_create(minus); lv_label_set_text(l,"-"); lv_obj_center(l); }
  lv_obj_t*val=lv_label_create(par); lv_obj_set_style_text_font(val,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(val,lv_color_hex(COL_INK),0);
  lv_obj_set_width(val,92); lv_obj_set_style_text_align(val,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(val,LV_ALIGN_TOP_RIGHT,-52,y+12);
  lv_obj_t*plus=lv_btn_create(par); lv_obj_set_size(plus,46,44); lv_obj_align(plus,LV_ALIGN_TOP_RIGHT,-4,y);
  lv_obj_set_style_bg_color(plus,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(plus,vacStep,LV_EVENT_CLICKED,(void*)(intptr_t)(field*2+1));
  { lv_obj_t*l=lv_label_create(plus); lv_label_set_text(l,"+"); lv_obj_center(l); }
  return val; }
void buildVacationSheet(lv_obj_t*scr){ gVacSheet=lv_obj_create(scr); lv_obj_set_size(gVacSheet,470,442); lv_obj_center(gVacSheet);
  lv_obj_set_style_bg_color(gVacSheet,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(gVacSheet,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(gVacSheet,2,0); lv_obj_set_style_radius(gVacSheet,14,0); lv_obj_set_style_pad_all(gVacSheet,0,0); lv_obj_clear_flag(gVacSheet,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(gVacSheet); lv_label_set_text(t,"Vacation"); lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(t,lv_color_hex(COL_INK),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,18,14);
  lv_obj_t*sub=lv_label_create(gVacSheet); lv_label_set_text(sub,"Hold eco setpoints while you are away"); lv_obj_set_style_text_font(sub,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(sub,lv_color_hex(COL_TEXT3),0); lv_obj_align(sub,LV_ALIGN_TOP_LEFT,18,50);
  gVacStartLbl =vacRow(gVacSheet,"Starts",   80,0);
  gVacNightsLbl=vacRow(gVacSheet,"Length",  142,1);
  gVacHeatLbl  =vacRow(gVacSheet,"Eco heat",204,2);
  gVacCoolLbl  =vacRow(gVacSheet,"Eco cool",266,3);
  lv_obj_t*sb=lv_btn_create(gVacSheet); lv_obj_set_size(sb,214,52); lv_obj_align(sb,LV_ALIGN_BOTTOM_LEFT,18,-16);
  lv_obj_set_style_bg_color(sb,lv_color_hex(COL_OK),0); lv_obj_add_event_cb(sb,vacStart,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(sb); lv_label_set_text(l,"Start vacation"); lv_obj_center(l); }
  lv_obj_t*cb=lv_btn_create(gVacSheet); lv_obj_set_size(cb,214,52); lv_obj_align(cb,LV_ALIGN_BOTTOM_RIGHT,-18,-16);
  lv_obj_set_style_bg_color(cb,lv_color_hex(COL_RAISED),0); lv_obj_set_style_border_color(cb,lv_color_hex(COL_BORDER),0); lv_obj_set_style_border_width(cb,1,0);
  lv_obj_add_event_cb(cb,vacCancelHold,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(cb); lv_label_set_text(l,"Cancel vacation"); lv_obj_center(l); }
  lv_obj_t*x=lv_btn_create(gVacSheet); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-6,8);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,vacClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  vacRefresh(); lv_obj_add_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); }

// #74: build up to kMaxPresets cards once (3-wide grid); renderMain fills the
// name/values from the LIVE roster (DisplayState.presets) and shows/hides by
// presetCount. Card index == roster index == the value passed to presetEvt.
// TODO(#74 deferred): on-device EDIT of a preset (long-press -> stepper ->
// stage retained slytherm/config/presets via mqttTask). Displaying the live
// roster is done; editing is PARTIAL.
void buildPresets(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Presets");
  for(int i=0;i<(int)kMaxPresets;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,236,116);
    lv_obj_align(b,LV_ALIGN_TOP_LEFT,6+(i%3)*256,52+(i/3)*128);
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(b,lv_color_hex(COL_BORDER),0); lv_obj_set_style_border_width(b,1,0);  // #fix2: dim hairline, not wireframe
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),LV_STATE_PRESSED);  // #75: immediate press feedback on touch-down
    lv_obj_set_style_border_color(b,lv_color_hex(COL_OK),LV_STATE_PRESSED); lv_obj_set_style_border_width(b,2,LV_STATE_PRESSED);
    lv_obj_add_event_cb(b,presetEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i); gPresetBtns[i]=b;
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,""); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,8); gPresetName[i]=l;
    lv_obj_t*s=lv_label_create(b); lv_label_set_text(s,""); lv_obj_set_style_text_color(s,lv_color_hex(COL_MUTED),0); lv_obj_align(s,LV_ALIGN_CENTER,0,6); gPresetVal[i]=s;
    lv_obj_t*h=lv_label_create(b); lv_label_set_text(h,"tap to apply"); lv_obj_set_style_text_color(h,lv_color_hex(COL_TEXT3),0); lv_obj_align(h,LV_ALIGN_BOTTOM_MID,0,-6);
    lv_obj_add_flag(b,LV_OBJ_FLAG_HIDDEN); }
  // #78: Vacation entry — opens the on-device vacation sheet (dates + eco setpoints)
  lv_obj_t*vb=lv_btn_create(tab); lv_obj_set_size(vb,748,60); lv_obj_align(vb,LV_ALIGN_TOP_LEFT,6,314);
  lv_obj_set_style_bg_color(vb,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(vb,lv_color_hex(COL_CRYO),0); lv_obj_set_style_border_width(vb,1,0); lv_obj_set_style_radius(vb,8,0);
  lv_obj_add_event_cb(vb,vacOpen,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(vb); lv_label_set_text(l,LV_SYMBOL_GPS "  Vacation  -  hold eco setpoints while away");
    lv_obj_set_style_text_font(l,&lv_font_montserrat_20,0); lv_obj_align(l,LV_ALIGN_LEFT_MID,16,0); } }

void sensorToggleEvt(lv_event_t*e){ int i=(int)(intptr_t)lv_event_get_user_data(e); if(i<0||i>=7) return;
  if(uiLocked()){ promptUnlock(); return; } if(gRowName[i][0]) uiToggleSensor(gRowName[i]); }
// #89 tiny column-header caption at a fixed x within the sensors tab
static void srHeader(lv_obj_t*tab,const char*t,int x,int w,lv_text_align_t al){
  lv_obj_t*l=lv_label_create(tab); lv_label_set_text(l,t); lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(l,lv_color_hex(COL_TEXT3),0); lv_obj_set_width(l,w); lv_obj_set_style_text_align(l,al,0);
  lv_obj_align(l,LV_ALIGN_TOP_LEFT,4+x,40); }
void buildSensors(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Room sensors");
  // #89: column captions so the aligned Name / Temp / Presence columns read as a table
  srHeader(tab,"ROOM",SR_NAME_X,SR_NAME_W,LV_TEXT_ALIGN_LEFT);
  srHeader(tab,"TEMP",SR_TEMP_X,SR_TEMP_W,LV_TEXT_ALIGN_RIGHT);
  srHeader(tab,"STATUS",SR_PRES_X,SR_PRES_W,LV_TEXT_ALIGN_LEFT);
  for(int i=0;i<7;i++){ lv_obj_t*r=lv_obj_create(tab); lv_obj_set_size(r,760,44); lv_obj_align(r,LV_ALIGN_TOP_LEFT,4,64+i*50);
    lv_obj_set_style_bg_color(r,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_width(r,0,0); lv_obj_set_style_radius(r,8,0);
    lv_obj_set_style_pad_all(r,0,0); lv_obj_clear_flag(r,LV_OBJ_FLAG_SCROLLABLE);
    // Name — left, fixed column, ellipsize on overflow so it can't push the other columns (#89)
    lv_obj_t*nm=lv_label_create(r); lv_obj_set_style_text_color(nm,lv_color_hex(COL_INK),0);
    lv_obj_set_style_text_font(nm,&lv_font_montserrat_20,0); lv_obj_set_width(nm,SR_NAME_W);
    lv_label_set_long_mode(nm,LV_LABEL_LONG_DOT); lv_obj_align(nm,LV_ALIGN_LEFT_MID,SR_NAME_X,0);
    // Temperature — own column, right-aligned so the degree digits stack
    lv_obj_t*tp=lv_label_create(r); lv_obj_set_style_text_color(tp,lv_color_hex(COL_INK),0);
    lv_obj_set_style_text_font(tp,&lv_font_montserrat_20,0); lv_obj_set_width(tp,SR_TEMP_W);
    lv_obj_set_style_text_align(tp,LV_TEXT_ALIGN_RIGHT,0); lv_obj_align(tp,LV_ALIGN_LEFT_MID,SR_TEMP_X,0);
    // Presence / status — own column
    lv_obj_t*pr=lv_label_create(r); lv_obj_set_style_text_color(pr,lv_color_hex(COL_MUTED),0);
    lv_obj_set_style_text_font(pr,&lv_font_montserrat_16,0); lv_obj_set_width(pr,SR_PRES_W);
    lv_label_set_long_mode(pr,LV_LABEL_LONG_DOT); lv_obj_align(pr,LV_ALIGN_LEFT_MID,SR_PRES_X,0);
    lv_obj_t*b=lv_btn_create(r); lv_obj_set_size(b,92,36); lv_obj_align(b,LV_ALIGN_RIGHT_MID,-8,0);
    lv_obj_add_event_cb(b,sensorToggleEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i);
    lv_obj_t*bl=lv_label_create(b); lv_label_set_text(bl,"--"); lv_obj_center(bl);
    gSensorRows[i]={r,nm,tp,pr,b,bl}; gRowName[i][0]=0; lv_obj_add_flag(r,LV_OBJ_FLAG_HIDDEN); } }
void buildSystem(lv_obj_t*tab){ header(tab,"System");
  wSysBody=lv_label_create(tab); lv_obj_set_style_text_color(wSysBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wSysBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wSysBody,"");
  // 12 h trend graph (#76): actual (fused) vs heat/cool setpoints, right ~1/3.
  for(int i=0;i<kGraphPts;i++){ gRingA[i]=LV_CHART_POINT_NONE; gRingH[i]=LV_CHART_POINT_NONE; gRingC[i]=LV_CHART_POINT_NONE; }
  lv_obj_t*gt=lv_label_create(tab); lv_label_set_text(gt,"Last 12 h"); eyebrow(gt); lv_obj_set_style_text_color(gt,lv_color_hex(COL_TEXT3),0); lv_obj_align(gt,LV_ALIGN_TOP_RIGHT,-8,24);
  // #84: chart box top == wSysBody's first line ("Now running:", y=48); caption sits above it.
  gSysChart=lv_chart_create(tab); lv_obj_set_size(gSysChart,316,270); lv_obj_align(gSysChart,LV_ALIGN_TOP_RIGHT,-6,48);
  lv_obj_set_style_bg_color(gSysChart,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_width(gSysChart,0,0); lv_obj_set_style_radius(gSysChart,10,0);
  lv_obj_set_style_pad_all(gSysChart,6,0); lv_obj_set_style_line_color(gSysChart,lv_color_hex(COL_BORDER),LV_PART_MAIN);
  lv_obj_set_style_width(gSysChart,0,LV_PART_INDICATOR); lv_obj_set_style_height(gSysChart,0,LV_PART_INDICATOR);  // no point markers, just lines
  lv_chart_set_type(gSysChart,LV_CHART_TYPE_LINE); lv_chart_set_point_count(gSysChart,kGraphPts);
  lv_chart_set_div_line_count(gSysChart,4,0); lv_chart_set_range(gSysChart,LV_CHART_AXIS_PRIMARY_Y,150,300);  // 15.0..30.0 until data arrives
  gSerActual=lv_chart_add_series(gSysChart,lv_color_hex(COL_INK),LV_CHART_AXIS_PRIMARY_Y);
  gSerHeat=lv_chart_add_series(gSysChart,lv_color_hex(COL_EMBER),LV_CHART_AXIS_PRIMARY_Y);
  gSerCool=lv_chart_add_series(gSysChart,lv_color_hex(COL_CRYO),LV_CHART_AXIS_PRIMARY_Y);
  lv_chart_set_ext_y_array(gSysChart,gSerActual,gRingA); lv_chart_set_ext_y_array(gSysChart,gSerHeat,gRingH); lv_chart_set_ext_y_array(gSysChart,gSerCool,gRingC);
  wSysGraphLbl=lv_label_create(tab); lv_obj_set_style_text_font(wSysGraphLbl,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(wSysGraphLbl,lv_color_hex(COL_MUTED),0); lv_obj_align(wSysGraphLbl,LV_ALIGN_TOP_RIGHT,-8,322); lv_label_set_text(wSysGraphLbl,""); }
void buildDiag(lv_obj_t*tab){ header(tab,"Diagnostics");
  wDiagBody=lv_label_create(tab); lv_obj_set_style_text_color(wDiagBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wDiagBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wDiagBody,"");
  mkBtn(tab,LV_SYMBOL_EYE_OPEN "  LISTEN on RS-485",openSniff,LV_ALIGN_BOTTOM_LEFT,4,-8,COL_CRYO,300); }

// PIN keypad
void kpadRefresh(){ char d[16]=""; for(int i=0;i<4;i++) strcat(d,i<kpN?"* ":"_ "); lv_label_set_text(kpadDots,d); }
void kpadOpen(KpMode m,const char*t){ kpMode=m; kpN=0; lv_label_set_text(kpadTitle,t); kpadRefresh();
  lv_obj_clear_flag(kpad,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(kpad); }
void promptUnlock(){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
void kpEvt(lv_event_t*e){ const int v=(int)(intptr_t)lv_event_get_user_data(e);
  if(v==-1){ lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); return; } if(v==-2){ kpN=0; kpadRefresh(); return; }
  if(kpN<4){ kpBuf[kpN++]=(uint8_t)v; kpadRefresh(); }
  if(kpN==4){ if(kpMode==KpMode::Set){ L(); gM->setUserPin(kpBuf,kPinSalt); U(); lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); }  // saved -> close (control task persists to NVS)
    else { L(); gM->beginPinEntry(PinContext::kUnlock,nowS()); PinResult r=PinResult::kIdle;
      for(int i=0;i<4;i++) r=gM->enterPinDigit(kpBuf[i],nowS()); U();
      if(r==PinResult::kAccepted) lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); else lv_label_set_text(kpadTitle,"Wrong PIN"); }
    kpN=0; kpadRefresh(); } }
void buildKeypad(lv_obj_t*scr){ kpad=lv_obj_create(scr); lv_obj_set_size(kpad,360,420); lv_obj_center(kpad);
  lv_obj_set_style_bg_color(kpad,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(kpad,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(kpad,2,0); lv_obj_clear_flag(kpad,LV_OBJ_FLAG_SCROLLABLE);
  kpadTitle=lv_label_create(kpad); lv_obj_align(kpadTitle,LV_ALIGN_TOP_MID,0,4);
  kpadDots=lv_label_create(kpad); lv_obj_set_style_text_font(kpadDots,&lv_font_montserrat_28,0); lv_obj_align(kpadDots,LV_ALIGN_TOP_MID,0,36);
  lv_obj_t*grid=lv_obj_create(kpad); lv_obj_set_size(grid,340,300); lv_obj_align(grid,LV_ALIGN_BOTTOM_MID,0,-8);
  lv_obj_set_style_bg_opa(grid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(grid,0,0);
  lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP); lv_obj_set_flex_align(grid,LV_FLEX_ALIGN_SPACE_EVENLY,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(grid,LV_OBJ_FLAG_SCROLLABLE);
  const char*keys[12]={"1","2","3","4","5","6","7","8","9","Esc","0","Clr"}; const int vals[12]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  for(int i=0;i<12;i++){ lv_obj_t*b=lv_btn_create(grid); lv_obj_set_size(b,96,64); lv_obj_add_event_cb(b,kpEvt,LV_EVENT_CLICKED,(void*)(intptr_t)vals[i]);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,keys[i]); lv_obj_center(l); }
  lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); }

// ---- hold-duration chooser (#81) ----
void holdPick(lv_event_t*e){ int v=(int)(intptr_t)lv_event_get_user_data(e);
  L(); if(v<0) gM->resumeSchedule(); else gM->requestHold((HoldType)v); U();   // v<0: Resume schedule; else HoldType enum value
  if(gHoldSheet) lv_obj_add_flag(gHoldSheet,LV_OBJ_FLAG_HIDDEN); }
void holdClose(lv_event_t*){ if(gHoldSheet) lv_obj_add_flag(gHoldSheet,LV_OBJ_FLAG_HIDDEN); }
void holdEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; }
  if(!gHoldSheet) return; lv_obj_clear_flag(gHoldSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gHoldSheet); }
void buildHoldSheet(lv_obj_t*scr){ gHoldSheet=lv_obj_create(scr); lv_obj_set_size(gHoldSheet,420,382); lv_obj_center(gHoldSheet);
  lv_obj_set_style_bg_color(gHoldSheet,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(gHoldSheet,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(gHoldSheet,2,0); lv_obj_set_style_radius(gHoldSheet,14,0); lv_obj_set_style_pad_all(gHoldSheet,0,0); lv_obj_clear_flag(gHoldSheet,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(gHoldSheet); lv_label_set_text(t,"Hold this temperature"); lv_obj_set_style_text_font(t,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(t,lv_color_hex(COL_INK),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,16,14);
  struct HB{const char*t;int v;} hb[5]={{"2 hours",(int)HoldType::kTwoHours},{"4 hours",(int)HoldType::kFourHours},
    {"Until next schedule",(int)HoldType::kUntilNextPreset},{"Hold until you change it",(int)HoldType::kIndefinite},{"Resume schedule",-1}};
  for(int i=0;i<5;i++){ lv_obj_t*b=lv_btn_create(gHoldSheet); lv_obj_set_size(b,388,48); lv_obj_align(b,LV_ALIGN_TOP_MID,0,50+i*54);
    lv_obj_set_style_bg_color(b,lv_color_hex(hb[i].v==-1?COL_RAISED:COL_BG),0); lv_obj_set_style_border_color(b,lv_color_hex(hb[i].v==-1?COL_OK:COL_BORDER),0); lv_obj_set_style_border_width(b,1,0);
    lv_obj_add_event_cb(b,holdPick,LV_EVENT_CLICKED,(void*)(intptr_t)hb[i].v);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,hb[i].t); lv_obj_center(l); }
  lv_obj_t*x=lv_btn_create(gHoldSheet); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-6,8);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,holdClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  lv_obj_add_flag(gHoldSheet,LV_OBJ_FLAG_HIDDEN); }

void clkEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } uiToggleClock24(); }
void setPinEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } kpadOpen(KpMode::Set,"Set a 4-digit PIN"); }
void lockEvt(lv_event_t*){ L(); bool set=gM->userPinSet(); if(set) gM->lockNow(nowS()); U(); }
void unlockEvt(lv_event_t*){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
void openWifi(lv_event_t*);    // defined below (before buildUi)
void openServer(lv_event_t*);  // defined below (before buildUi)
void buildSettings(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Settings");
  wLockState=lv_label_create(tab); lv_obj_set_style_text_color(wLockState,lv_color_hex(COL_MUTED),0); lv_obj_align(wLockState,LV_ALIGN_TOP_LEFT,4,50);
  // Lock actions (12/24h moved to its own labelled row below, #77)
  struct B{const char*t; lv_event_cb_t cb;} bs[3]={{"Set PIN",setPinEvt},{"Lock",lockEvt},{"Unlock",unlockEvt}};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,150,54); lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*158,86);
    lv_obj_add_event_cb(b,bs[i].cb,LV_EVENT_CLICKED,nullptr); lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,bs[i].t); lv_obj_center(l); }
  // Clock: 12-hour / 24-hour on its own labelled row (#77)
  { lv_obj_t*cl=lv_label_create(tab); lv_label_set_text(cl,"Clock:"); lv_obj_set_style_text_color(cl,lv_color_hex(COL_MUTED),0); lv_obj_align(cl,LV_ALIGN_TOP_LEFT,8,168);
    lv_obj_t*cb=lv_btn_create(tab); lv_obj_set_size(cb,180,54); lv_obj_align(cb,LV_ALIGN_TOP_LEFT,110,152);
    lv_obj_set_style_bg_color(cb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(cb,clkEvt,LV_EVENT_CLICKED,nullptr);
    gClkLbl=lv_label_create(cb); lv_label_set_text(gClkLbl,"12-hour"); lv_obj_center(gClkLbl); }
  // WiFi + Home system: consistent green-when-working status word to the right (#77)
  lv_obj_t*wb=lv_btn_create(tab); lv_obj_set_size(wb,220,56); lv_obj_align(wb,LV_ALIGN_TOP_LEFT,4,228);
  lv_obj_set_style_bg_color(wb,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(wb,openWifi,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*wl=lv_label_create(wb); lv_label_set_text(wl,LV_SYMBOL_WIFI "  WiFi setup"); lv_obj_set_style_text_color(wl,lv_color_hex(0x06202B),0); lv_obj_center(wl);
  wSetWifi=lv_label_create(tab); lv_obj_set_style_text_font(wSetWifi,&lv_font_montserrat_20,0); lv_obj_align(wSetWifi,LV_ALIGN_TOP_LEFT,240,244); lv_label_set_text(wSetWifi,"");
  lv_obj_t*sb=lv_btn_create(tab); lv_obj_set_size(sb,220,56); lv_obj_align(sb,LV_ALIGN_TOP_LEFT,4,300);
  lv_obj_set_style_bg_color(sb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(sb,openServer,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*sl=lv_label_create(sb); lv_label_set_text(sl,LV_SYMBOL_HOME "  Home system"); lv_obj_center(sl);
  wSetHome=lv_label_create(tab); lv_obj_set_style_text_font(wSetHome,&lv_font_montserrat_20,0); lv_obj_align(wSetHome,LV_ALIGN_TOP_LEFT,240,316); lv_label_set_text(wSetHome,""); }

// ---- ambient (idle) screen ----
void renderMain(const DisplayState& s);  // fwd: wake repaints Home before the light shows
// Burn-in guard (#70/#86b/#86c): the WHOLE ambient hero does a smooth ping-pong
// "walk" across the panel — one step per drift, right to the right clamp, then
// reverse and walk back left, bouncing at the edges, with a gentle diagonal Y
// drift so it uses the vertical space too. NO teleporting between corners.
// The clamp is computed from the ACTUAL rendered label geometry every drift, so
// nothing ever clips regardless of room-name length or temperature width.
constexpr int      kAmbSteps   = 6;                 // steps across each direction
constexpr uint32_t kAmbShiftMs = 15u * 60u * 1000u; // drift cadence; a full
// left->right->left cycle takes 2*kAmbSteps*kAmbShiftMs (= 12 * 15 min = 3 h).
void ambientShift(int step){
  if(!aTemp) return;
  constexpr int kMargin=8;
  // Base top-left offsets of each label — MUST match buildAmbient() below.
  struct LblBase{ lv_obj_t* o; int bx; int by; };
  const LblBase L[4]={ {aNow,26,38}, {aTemp,18,72}, {aTarget,26,220}, {aName,26,254} };
  // #86c: resolve real widths/heights for the CURRENT text, then measure the
  // block's right/bottom extent (at zero shift) and its leftmost/topmost base.
  lv_obj_update_layout(scrAmb);
  int rightExtent=0, bottomExtent=0, minBaseX=INT32_MAX, minBaseY=INT32_MAX;
  for(const auto& e:L){ if(!e.o) continue;
    int w=lv_obj_get_width(e.o), h=lv_obj_get_height(e.o);
    if(e.bx<minBaseX) minBaseX=e.bx;
    if(e.by<minBaseY) minBaseY=e.by;
    if(e.bx+w>rightExtent) rightExtent=e.bx+w;
    if(e.by+h>bottomExtent) bottomExtent=e.by+h; }
  if(minBaseX==INT32_MAX){ minBaseX=18; minBaseY=38; }  // no labels -> defaults
  // Walk window: shift X in [Xmin,Xmax] keeps left>=margin and right<=800-margin;
  // shift Y in [Ymin,Ymax] keeps top>=margin and bottom<=480-margin. If the block
  // is wider/taller than the usable area, pin to the top-left margin (never clip
  // the left/top edge, never go negative past the margin).
  int Xmin=kMargin-minBaseX, Xmax=(800-kMargin)-rightExtent;
  int Ymin=kMargin-minBaseY, Ymax=(480-kMargin)-bottomExtent;
  if(Xmax<Xmin) Xmax=Xmin; if(Ymax<Ymin) Ymax=Ymin;
  // Triangle wave over the step counter -> smooth ping-pong 0..kAmbSteps..0.
  const int span=kAmbSteps>0?kAmbSteps:1, period=2*span;
  int m=((step%period)+period)%period;          // 0..period-1 (safe for any step)
  int pos=(m<=span)?m:(period-m);               // 0 at left, span at right, bounce
  int X=Xmin+(Xmax-Xmin)*pos/span;
  int Y=Ymin+(Ymax-Ymin)*pos/span;             // diagonal ping-pong uses vertical space
  if(aNow)   lv_obj_align(aNow,   LV_ALIGN_TOP_LEFT,26+X,38+Y);
  if(aTemp)  lv_obj_align(aTemp,  LV_ALIGN_TOP_LEFT,18+X,72+Y);
  if(aTarget)lv_obj_align(aTarget,LV_ALIGN_TOP_LEFT,26+X,220+Y);
  if(aName)  lv_obj_align(aName,  LV_ALIGN_TOP_LEFT,26+X,254+Y); }
void ambWake(lv_event_t*){ gAmbient=false; gAmbShiftIdx=0; ambientShift(0);
  // #86 polish: no blank-temperature flash on wake. Load Home, repaint it with
  // the LATEST model, force a synchronous flush so the temp is fully drawn, and
  // only THEN turn the backlight on. Runs even when not blanked (touch-wake from
  // lit ambient) so Home's temp is current the instant scrMain becomes visible.
  lv_scr_load(scrMain);
  { DisplayState s; L(); s=gM->state(); U(); renderMain(s); }
  lv_refr_now(NULL);
  if(gBlanked){ ch422O(gCh|kBitBl); gBlanked=false; } }
void buildAmbient(){ scrAmb=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrAmb,lv_color_hex(COL_BG),0); lv_obj_set_style_pad_all(scrAmb,0,0);
  lv_obj_add_event_cb(scrAmb,ambWake,LV_EVENT_CLICKED,nullptr); lv_obj_add_flag(scrAmb,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(scrAmb,LV_OBJ_FLAG_SCROLLABLE);
  // mirror the Home hero: NOW + big fused temp + action + what's being tracked
  aNow=lv_label_create(scrAmb); lv_label_set_text(aNow,"NOW"); eyebrow(aNow); lv_obj_set_style_text_color(aNow,lv_color_hex(COL_TEXT3),0); lv_obj_align(aNow,LV_ALIGN_TOP_LEFT,26,38);
  aTemp=lv_label_create(scrAmb); lv_obj_set_style_text_font(aTemp,&font_now80,0); lv_obj_set_style_text_color(aTemp,lv_color_hex(COL_INK),0); lv_obj_align(aTemp,LV_ALIGN_TOP_LEFT,18,72);
  aTarget=lv_label_create(scrAmb); lv_obj_set_style_text_font(aTarget,&lv_font_montserrat_20,0); lv_obj_align(aTarget,LV_ALIGN_TOP_LEFT,26,220);
  aName=lv_label_create(scrAmb); lv_obj_set_style_text_color(aName,lv_color_hex(COL_MUTED),0); lv_obj_set_style_text_font(aName,&lv_font_montserrat_20,0); lv_obj_align(aName,LV_ALIGN_TOP_LEFT,26,254);
  aAlarm=lv_label_create(scrAmb); lv_obj_set_style_text_color(aAlarm,lv_color_hex(COL_CRIT),0);   // alarm visible even in ambient (docs/04 §1c)
  lv_obj_set_style_text_font(aAlarm,&lv_font_montserrat_20,0); lv_obj_align(aAlarm,LV_ALIGN_BOTTOM_MID,0,-16); lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

// ---- first-run Welcome / onboarding (issue #82) ----
// Shown at boot when there is no saved Wi-Fi: big logo, friendly headline, and a
// primary "Let's Get Started" -> WiFi setup. service() moves to Home once the
// connection comes up. Homeowner wording (no MQTT/broker jargon).
void onGetStarted(lv_event_t*){ lv_scr_load(scrMain); openWifi(nullptr); }  // gWelcomeActive stays until connected
void buildWelcome(){ scrWelcome=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrWelcome,lv_color_hex(COL_BG),0);
  lv_obj_set_style_pad_all(scrWelcome,0,0); lv_obj_clear_flag(scrWelcome,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*mk=lv_img_create(scrWelcome); lv_img_set_src(mk,&slymark_img); lv_img_set_zoom(mk,700); lv_obj_align(mk,LV_ALIGN_TOP_MID,0,70);  // ~2.7x logo mark
  lv_obj_t*h=lv_label_create(scrWelcome); lv_label_set_text(h,"Welcome to SlyTherm"); lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0);  // full-alphabet face; font_set48 is a digits-only subset (tofu on letters)
  lv_obj_set_style_text_color(h,lv_color_hex(COL_INK),0); lv_obj_align(h,LV_ALIGN_CENTER,0,0);
  lv_obj_t*sub=lv_label_create(scrWelcome); lv_label_set_text(sub,"Let's get your thermostat online.");
  lv_obj_set_style_text_font(sub,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(sub,lv_color_hex(COL_MUTED),0); lv_obj_align(sub,LV_ALIGN_CENTER,0,44);
  lv_obj_t*b=lv_btn_create(scrWelcome); lv_obj_set_size(b,340,68); lv_obj_align(b,LV_ALIGN_BOTTOM_MID,0,-48);
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_CRYO),0); lv_obj_set_style_radius(b,12,0); lv_obj_add_event_cb(b,onGetStarted,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*bl=lv_label_create(b); lv_label_set_text(bl,"Let's Get Started"); lv_obj_set_style_text_font(bl,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(bl,lv_color_hex(0x06202B),0); lv_obj_center(bl); }

// ---- WiFi setup: state machine (Settings -> WiFi setup) --------------------
void wifiGoto(WifiState s);  // fwd

lv_obj_t* mkBtn(lv_obj_t*p,const char*t,lv_event_cb_t cb,lv_align_t al,int x,int y,uint32_t bg,int w=140){
  lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,w,46); lv_obj_align(b,al,x,y);
  lv_obj_set_style_bg_color(b,lv_color_hex(bg),0); lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,t);
  if(bg==COL_CRYO) lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); lv_obj_center(l); return b; }

void onWifiClose(lv_event_t*){ gWifiOpen=false; lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN); }
void onScan(lv_event_t*){ wifi_prov::requestScan(); wifiGoto(WifiState::Scanning); }
void onForget(lv_event_t*){ wifi_prov::forget(); wifiGoto(WifiState::Status); }
void onBackStatus(lv_event_t*){ wifiGoto(WifiState::Status); }
void onBackList(lv_event_t*){ wifiGoto(WifiState::List); }
void onNet(lv_event_t*e){ int i=(int)(intptr_t)lv_event_get_user_data(e); if(i<0||i>=wifi_prov::kMaxNets) return;
  strlcpy(gSelSsid,gNetSsids[i],sizeof(gSelSsid)); wifiGoto(WifiState::Password); }
void onOther(lv_event_t*){ gSelSsid[0]=0; wifiGoto(WifiState::Other); }
void doConnect(){ if(gWs==WifiState::Other){ const char*ss=lv_textarea_get_text(taSsid); if(!ss[0]) return;
    strlcpy(gSelSsid,ss,sizeof(gSelSsid)); wifi_prov::requestConnect(ss,lv_textarea_get_text(taPass)); }
  else wifi_prov::requestConnect(gSelSsid,lv_textarea_get_text(taPass));
  wifiGoto(WifiState::Connecting); }
void onConnect(lv_event_t*){ doConnect(); }
void onDone(lv_event_t*){ gWifiOpen=false; lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN);   // #74/#82: finish -> Home
  gWelcomeActive=false; lv_scr_load(scrMain); if(gTabview) lv_tabview_set_act(gTabview,0,LV_ANIM_OFF); }
void onTryAgain(lv_event_t*){ wifiGoto(gSelSsid[0]?WifiState::Password:WifiState::List); }
void onShowPw(lv_event_t*){ if(taPass) lv_textarea_set_password_mode(taPass,!lv_textarea_get_password_mode(taPass)); }
void onTaFocus(lv_event_t*e){ if(kbd) lv_keyboard_set_textarea(kbd,(lv_obj_t*)lv_event_get_target(e)); }
void onKb(lv_event_t*e){ lv_event_code_t c=lv_event_get_code(e); if(c==LV_EVENT_READY) doConnect(); else if(c==LV_EVENT_CANCEL) wifiGoto(WifiState::List); }

// Dark styling for a list row (belt-and-suspenders vs the theme flag).
void styleRow(lv_obj_t* b){ lv_obj_set_style_bg_color(b,lv_color_hex(COL_CARD),0);
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),LV_STATE_PRESSED);
  lv_obj_set_style_text_color(b,lv_color_hex(COL_INK),0); lv_obj_set_style_border_width(b,0,0);
  lv_obj_set_style_radius(b,8,0); }
void buildList(){ lv_obj_t*list=lv_list_create(wifiOv); lv_obj_set_size(list,780,352); lv_obj_align(list,LV_ALIGN_TOP_MID,0,46);
  lv_obj_set_style_bg_color(list,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(list,0,0); lv_obj_set_style_pad_row(list,6,0);
  wifi_prov::Net raw[wifi_prov::kMaxNets]; int n=wifi_prov::scanResults(raw,wifi_prov::kMaxNets);
  wifi_prov::Net uniq[wifi_prov::kMaxNets]; int m=0;                  // merge duplicate SSIDs, strongest signal
  for(int i=0;i<n;i++){ if(!raw[i].ssid[0]) continue; int f=-1;
    for(int j=0;j<m;j++) if(strcmp(uniq[j].ssid,raw[i].ssid)==0){ f=j; break; }
    if(f<0) uniq[m++]=raw[i]; else if(raw[i].rssi>uniq[f].rssi) uniq[f]=raw[i]; }
  for(int x=0;x<m-1;x++) for(int y=x+1;y<m;y++)                       // strongest signal first
    if(uniq[y].rssi>uniq[x].rssi){ wifi_prov::Net t=uniq[x]; uniq[x]=uniq[y]; uniq[y]=t; }
  for(int i=0;i<m;i++){ strlcpy(gNetSsids[i],uniq[i].ssid,sizeof(gNetSsids[i])); char it[48];
    snprintf(it,sizeof(it),"%s%s",uniq[i].ssid,uniq[i].locked?"":"   (open)");
    lv_obj_t*b=lv_list_add_btn(list,LV_SYMBOL_WIFI,it); styleRow(b); lv_obj_add_event_cb(b,onNet,LV_EVENT_CLICKED,(void*)(intptr_t)i); }
  if(m==0){ lv_obj_t*t=lv_list_add_text(list,"No networks found"); lv_obj_set_style_text_color(t,lv_color_hex(COL_MUTED),0); }
  lv_obj_t*ob=lv_list_add_btn(list,LV_SYMBOL_PLUS,"Other network"); styleRow(ob); lv_obj_add_event_cb(ob,onOther,LV_EVENT_CLICKED,nullptr);
  { char names[120]=""; for(int i=0;i<m && i<8;i++){ strncat(names,gNetSsids[i],sizeof(names)-strlen(names)-2); if(i<m-1&&i<7) strncat(names,", ",3); }
    telnet_log::logf("[ui] WiFi list: %d networks [%s]", m, names); }
  mkBtn(wifiOv,"Back",onBackStatus,LV_ALIGN_BOTTOM_LEFT,8,-8,COL_RAISED);
  mkBtn(wifiOv,"Rescan",onScan,LV_ALIGN_BOTTOM_RIGHT,-8,-8,COL_RAISED); }

void wifiUpdateStatus(){ if(gWs!=WifiState::Status||!wfLine) return; char ss[33],ip[20]; int8_t r; bool c;
  wifi_prov::status(ss,sizeof(ss),ip,sizeof(ip),&r,&c);
  if(c){ setTxt(wfLine,"Connected"); lv_obj_set_style_text_color(wfLine,lv_color_hex(COL_OK),0);
    char b[64]; snprintf(b,sizeof(b),"%s    %s    %d dBm",ss,ip,(int)r); setTxt(wfSub,b); }
  else { setTxt(wfLine,"Not connected"); lv_obj_set_style_text_color(wfLine,lv_color_hex(COL_INK),0); setTxt(wfSub,""); } }

void wifiGoto(WifiState s){ gWs=s; lv_obj_clean(wifiOv); taPass=taSsid=kbd=wfLine=wfSub=nullptr;
  { static const char* nm[]={"Status","Scanning","List","Password","Other","Connecting","Result-OK","Result-FAIL"};
    telnet_log::logf("[ui] WiFi -> %s%s%s", nm[(int)s], gSelSsid[0]?" ssid=":"", gSelSsid[0]?gSelSsid:""); }
  lv_obj_t*title=lv_label_create(wifiOv); lv_obj_set_style_text_font(title,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(title,lv_color_hex(COL_CRYO),0); lv_obj_align(title,LV_ALIGN_TOP_LEFT,10,10);
  lv_obj_t*x=lv_btn_create(wifiOv); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-8,6);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,onWifiClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  switch(s){
    case WifiState::Status: lv_label_set_text(title,"WiFi");
      wfLine=lv_label_create(wifiOv); lv_obj_set_style_text_font(wfLine,&lv_font_montserrat_28,0); lv_obj_align(wfLine,LV_ALIGN_CENTER,0,-30);
      wfSub=lv_label_create(wifiOv); lv_obj_set_style_text_color(wfSub,lv_color_hex(COL_TEXT3),0); lv_obj_align(wfSub,LV_ALIGN_CENTER,0,12);
      if(wifi_prov::connected()){ mkBtn(wifiOv,"Change network",onScan,LV_ALIGN_BOTTOM_RIGHT,-8,-8,COL_RAISED,190);
        mkBtn(wifiOv,"Forget",onForget,LV_ALIGN_BOTTOM_LEFT,8,-8,COL_RAISED); }
      else mkBtn(wifiOv,"Connect",onScan,LV_ALIGN_BOTTOM_MID,0,-8,COL_CRYO,190);
      wifiUpdateStatus(); break;
    case WifiState::Scanning: lv_label_set_text(title,"WiFi");
      { lv_obj_t*sp=lv_spinner_create(wifiOv,1000,60); lv_obj_set_size(sp,54,54); lv_obj_align(sp,LV_ALIGN_CENTER,0,-16);
        lv_obj_t*l=lv_label_create(wifiOv); lv_label_set_text(l,"Scanning..."); lv_obj_set_style_text_color(l,lv_color_hex(COL_MUTED),0); lv_obj_align(l,LV_ALIGN_CENTER,0,42); }
      break;
    case WifiState::List: lv_label_set_text(title,"Choose a network"); buildList(); break;
    case WifiState::Password: lv_label_set_text(title,gSelSsid);
      taPass=lv_textarea_create(wifiOv); lv_textarea_set_one_line(taPass,true); lv_textarea_set_password_mode(taPass,true);
      lv_textarea_set_placeholder_text(taPass,"password"); lv_obj_set_size(taPass,460,48); lv_obj_align(taPass,LV_ALIGN_TOP_LEFT,10,52);
      mkBtn(wifiOv,"show",onShowPw,LV_ALIGN_TOP_LEFT,482,52,COL_RAISED,90);
      mkBtn(wifiOv,"Connect",onConnect,LV_ALIGN_TOP_RIGHT,-8,52,COL_CRYO,150);
      kbd=lv_keyboard_create(wifiOv); lv_keyboard_set_textarea(kbd,taPass); lv_obj_add_event_cb(kbd,onKb,LV_EVENT_ALL,nullptr);
      break;
    case WifiState::Other: lv_label_set_text(title,"Other network");
      taSsid=lv_textarea_create(wifiOv); lv_textarea_set_one_line(taSsid,true); lv_textarea_set_placeholder_text(taSsid,"network name (SSID)");
      lv_obj_set_size(taSsid,600,48); lv_obj_align(taSsid,LV_ALIGN_TOP_LEFT,10,52); lv_obj_add_event_cb(taSsid,onTaFocus,LV_EVENT_FOCUSED,nullptr);
      taPass=lv_textarea_create(wifiOv); lv_textarea_set_one_line(taPass,true); lv_textarea_set_password_mode(taPass,true); lv_textarea_set_placeholder_text(taPass,"password");
      lv_obj_set_size(taPass,460,48); lv_obj_align(taPass,LV_ALIGN_TOP_LEFT,10,108); lv_obj_add_event_cb(taPass,onTaFocus,LV_EVENT_FOCUSED,nullptr);
      mkBtn(wifiOv,"Connect",onConnect,LV_ALIGN_TOP_RIGHT,-8,108,COL_CRYO,150);
      kbd=lv_keyboard_create(wifiOv); lv_keyboard_set_textarea(kbd,taSsid); lv_obj_add_event_cb(kbd,onKb,LV_EVENT_ALL,nullptr);
      break;
    case WifiState::Connecting: lv_label_set_text(title,"WiFi");
      { lv_obj_t*sp=lv_spinner_create(wifiOv,1000,60); lv_obj_set_size(sp,54,54); lv_obj_align(sp,LV_ALIGN_CENTER,0,-16);
        lv_obj_t*l=lv_label_create(wifiOv); char b[64]; snprintf(b,sizeof(b),"Connecting to %s...",gSelSsid);
        lv_label_set_text(l,b); lv_obj_set_style_text_color(l,lv_color_hex(COL_MUTED),0); lv_obj_align(l,LV_ALIGN_CENTER,0,42); }
      break;
    case WifiState::ResultOk: lv_label_set_text(title,"WiFi");
      { lv_obj_t*l=lv_label_create(wifiOv); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(l,lv_color_hex(COL_OK),0);
        char b[48]; snprintf(b,sizeof(b),"Connected to %s",gSelSsid); lv_label_set_text(l,b); lv_obj_align(l,LV_ALIGN_CENTER,0,-20);
        char ss[33],ip[20]; int8_t r; bool c; wifi_prov::status(ss,sizeof(ss),ip,sizeof(ip),&r,&c);
        lv_obj_t*s2=lv_label_create(wifiOv); lv_obj_set_style_text_color(s2,lv_color_hex(COL_TEXT3),0); lv_label_set_text(s2,ip); lv_obj_align(s2,LV_ALIGN_CENTER,0,20); }
      mkBtn(wifiOv,"Done",onDone,LV_ALIGN_BOTTOM_MID,0,-8,COL_CRYO,150); break;
    case WifiState::ResultFail: lv_label_set_text(title,"WiFi");
      { lv_obj_t*l=lv_label_create(wifiOv); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(l,lv_color_hex(COL_CRIT),0);
        lv_label_set_text(l,"Couldn't connect"); lv_obj_align(l,LV_ALIGN_CENTER,0,-20);
        lv_obj_t*s2=lv_label_create(wifiOv); lv_obj_set_style_text_color(s2,lv_color_hex(COL_TEXT3),0);
        char b[64]; snprintf(b,sizeof(b),"Check the password for %s",gSelSsid); lv_label_set_text(s2,b); lv_obj_align(s2,LV_ALIGN_CENTER,0,20); }
      mkBtn(wifiOv,"Back",onBackList,LV_ALIGN_BOTTOM_LEFT,8,-8,COL_RAISED);
      mkBtn(wifiOv,"Try again",onTryAgain,LV_ALIGN_BOTTOM_RIGHT,-8,-8,COL_CRYO); break;
  }
}

void buildWifi(lv_obj_t*scr){ wifiOv=lv_obj_create(scr); lv_obj_set_size(wifiOv,800,480); lv_obj_set_pos(wifiOv,0,0);
  lv_obj_set_style_bg_color(wifiOv,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(wifiOv,0,0);
  lv_obj_set_style_pad_all(wifiOv,0,0); lv_obj_clear_flag(wifiOv,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN); }

void openWifi(lv_event_t*){ gWifiOpen=true; lv_obj_clear_flag(wifiOv,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(wifiOv); wifiGoto(WifiState::Status); }

void renderWifi(){ if(!gWifiOpen) return;
  switch(gWs){
    case WifiState::Scanning:   if(wifi_prov::scanState()==wifi_prov::ScanState::kDone) wifiGoto(WifiState::List); break;
    case WifiState::Connecting: { wifi_prov::ConnState cs=wifi_prov::connState();
      if(cs==wifi_prov::ConnState::kConnected) wifiGoto(WifiState::ResultOk);
      else if(cs==wifi_prov::ConnState::kFailed) wifiGoto(WifiState::ResultFail); break; }
    case WifiState::Status:     wifiUpdateStatus(); break;
    default: break;
  }
}

// ---- Home system (broker) — Installer/Advanced manual entry (mqtt_cfg) ----
void onSrvClose(lv_event_t*){ gSrvOpen=false; lv_obj_add_flag(srvOv,LV_OBJ_FLAG_HIDDEN); }
void onSrvFocus(lv_event_t*e){ lv_obj_t*t=(lv_obj_t*)lv_event_get_target(e); if(!kbdS) return;
  lv_keyboard_set_textarea(kbdS,t); lv_keyboard_set_mode(kbdS, t==taPort?LV_KEYBOARD_MODE_NUMBER:LV_KEYBOARD_MODE_TEXT_LOWER); }
void onSrvSave(lv_event_t*){ uint16_t pt=(uint16_t)atoi(lv_textarea_get_text(taPort));
  mqtt_cfg::save(lv_textarea_get_text(taHost), pt?pt:1883, lv_textarea_get_text(taUser), lv_textarea_get_text(taSrvPass)); }
void onSrvKb(lv_event_t*){ onSrvSave(nullptr); }
void buildServer(lv_obj_t*scr){ srvOv=lv_obj_create(scr); lv_obj_set_size(srvOv,800,480); lv_obj_set_pos(srvOv,0,0);
  lv_obj_set_style_bg_color(srvOv,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(srvOv,0,0);
  lv_obj_set_style_pad_all(srvOv,0,0); lv_obj_clear_flag(srvOv,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(srvOv,LV_OBJ_FLAG_HIDDEN);
  lv_obj_t*title=lv_label_create(srvOv); lv_label_set_text(title,"Home system (advanced)");
  lv_obj_set_style_text_font(title,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(title,lv_color_hex(COL_CRYO),0); lv_obj_align(title,LV_ALIGN_TOP_LEFT,10,8);
  lv_obj_t*x=lv_btn_create(srvOv); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-8,6);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,onSrvClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  lblSrvStat=lv_label_create(srvOv); lv_obj_set_style_text_color(lblSrvStat,lv_color_hex(COL_MUTED),0);
  lv_obj_set_style_text_font(lblSrvStat,&lv_font_montserrat_20,0); lv_obj_align(lblSrvStat,LV_ALIGN_TOP_LEFT,10,42);
  taHost=lv_textarea_create(srvOv); lv_textarea_set_one_line(taHost,true); lv_textarea_set_placeholder_text(taHost,"broker host / IP");
  lv_obj_set_size(taHost,500,46); lv_obj_align(taHost,LV_ALIGN_TOP_LEFT,10,70); lv_obj_add_event_cb(taHost,onSrvFocus,LV_EVENT_FOCUSED,nullptr);
  taPort=lv_textarea_create(srvOv); lv_textarea_set_one_line(taPort,true); lv_textarea_set_placeholder_text(taPort,"1883");
  lv_obj_set_size(taPort,110,46); lv_obj_align(taPort,LV_ALIGN_TOP_LEFT,520,70); lv_obj_add_event_cb(taPort,onSrvFocus,LV_EVENT_FOCUSED,nullptr);
  mkBtn(srvOv,"Save",onSrvSave,LV_ALIGN_TOP_RIGHT,-8,70,COL_CRYO,140);
  taUser=lv_textarea_create(srvOv); lv_textarea_set_one_line(taUser,true); lv_textarea_set_placeholder_text(taUser,"username (optional)");
  lv_obj_set_size(taUser,310,46); lv_obj_align(taUser,LV_ALIGN_TOP_LEFT,10,124); lv_obj_add_event_cb(taUser,onSrvFocus,LV_EVENT_FOCUSED,nullptr);
  taSrvPass=lv_textarea_create(srvOv); lv_textarea_set_one_line(taSrvPass,true); lv_textarea_set_password_mode(taSrvPass,true); lv_textarea_set_placeholder_text(taSrvPass,"password (optional)");
  lv_obj_set_size(taSrvPass,310,46); lv_obj_align(taSrvPass,LV_ALIGN_TOP_LEFT,330,124); lv_obj_add_event_cb(taSrvPass,onSrvFocus,LV_EVENT_FOCUSED,nullptr);
  kbdS=lv_keyboard_create(srvOv); lv_keyboard_set_textarea(kbdS,taHost); lv_obj_add_event_cb(kbdS,onSrvKb,LV_EVENT_READY,nullptr); }
void openServer(lv_event_t*){ gSrvOpen=true; char h[64],u[48],p[64]; uint16_t pt=1883;
  mqtt_cfg::current(h,sizeof(h),&pt,u,sizeof(u),p,sizeof(p)); lv_textarea_set_text(taHost,h);
  char ps[8]; snprintf(ps,sizeof(ps),"%u",pt); lv_textarea_set_text(taPort,ps);
  lv_textarea_set_text(taUser,u); lv_textarea_set_text(taSrvPass,p);
  lv_obj_clear_flag(srvOv,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(srvOv);
  telnet_log::logf("[ui] Home system screen (broker %s:%u connected=%d)", h, pt, mqtt_cfg::connected()); }
void renderServer(){ if(!gSrvOpen||!lblSrvStat) return; char h[64]; uint16_t pt=1883; mqtt_cfg::current(h,sizeof(h),&pt,nullptr,0,nullptr,0);
  char b[96]; const bool c=mqtt_cfg::connected();
  if(c) snprintf(b,sizeof(b),"Connected  %s:%u",h,pt); else snprintf(b,sizeof(b),"Not connected%s%s",h[0]?"  ":"",h[0]?h:"");
  setTxt(lblSrvStat,b); lv_obj_set_style_text_color(lblSrvStat,lv_color_hex(c?COL_OK:COL_MUTED),0); }

// ---- RS-485 LISTEN capture screen (#71) ----
// Dedicated screen: Start/Stop drive the ct485Task capture, live frame count +
// a scrolling last-10 list refresh from renderSniff() each 250ms tick. The real
// artifact is the telnet stream (:23); capture it with tools/ct485cap.py.
void onSniffBack(lv_event_t*){ gSniffOpen=false; lv_scr_load(scrMain); }
void onSniffStart(lv_event_t*){ uiSniffStart(); }
void onSniffStop(lv_event_t*){ uiSniffStop(); }
void buildSniff(){ scrSniff=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrSniff,lv_color_hex(COL_BG),0);
  lv_obj_set_style_pad_all(scrSniff,0,0); lv_obj_set_style_text_font(scrSniff,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(scrSniff,lv_color_hex(COL_INK),0); lv_obj_clear_flag(scrSniff,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*title=lv_label_create(scrSniff); lv_label_set_text(title,"RS-485 Listen");
  lv_obj_set_style_text_font(title,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(title,lv_color_hex(COL_CRYO),0); lv_obj_align(title,LV_ALIGN_TOP_LEFT,10,8);
  mkBtn(scrSniff,LV_SYMBOL_LEFT "  Back",onSniffBack,LV_ALIGN_TOP_RIGHT,-8,6,COL_RAISED,150);
  mkBtn(scrSniff,"Start",onSniffStart,LV_ALIGN_TOP_LEFT,10,52,COL_CRYO,150);
  mkBtn(scrSniff,"Stop",onSniffStop,LV_ALIGN_TOP_LEFT,170,52,COL_RAISED,150);
  wSniffStat=lv_label_create(scrSniff); lv_obj_set_style_text_color(wSniffStat,lv_color_hex(COL_MUTED),0); lv_obj_align(wSniffStat,LV_ALIGN_TOP_LEFT,340,64);
  wSniffCount=lv_label_create(scrSniff); lv_obj_set_style_text_color(wSniffCount,lv_color_hex(COL_INK),0); lv_obj_align(wSniffCount,LV_ALIGN_TOP_RIGHT,-10,64);
  wSniffList=lv_label_create(scrSniff); lv_obj_set_style_text_font(wSniffList,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(wSniffList,lv_color_hex(COL_MUTED),0);
  lv_obj_align(wSniffList,LV_ALIGN_TOP_LEFT,10,108); lv_label_set_text(wSniffList,"(no frames yet)");
  wSniffFoot=lv_label_create(scrSniff); lv_obj_set_style_text_font(wSniffFoot,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(wSniffFoot,lv_color_hex(COL_TEXT3),0); lv_obj_align(wSniffFoot,LV_ALIGN_BOTTOM_LEFT,10,-8); }
void openSniff(lv_event_t*){ gSniffOpen=true; char foot[80]; String ip=WiFi.localIP().toString();
  snprintf(foot,sizeof(foot),"streaming to %s:23 - capture with tools/ct485cap.py",ip.c_str());
  if(wSniffFoot) setTxt(wSniffFoot,foot);
  lv_scr_load(scrSniff); telnet_log::logf("[ui] RS-485 Listen screen (capturing=%d frames=%lu)",(int)uiSniffActive(),(unsigned long)uiSniffFrames()); }
void renderSniff(){ if(!wSniffCount) return;
  const bool on=uiSniffActive();
  setTxt(wSniffStat,on?"capturing...":"stopped"); lv_obj_set_style_text_color(wSniffStat,lv_color_hex(on?COL_OK:COL_MUTED),0);
  char b[32]; snprintf(b,sizeof(b),"Frames: %lu",(unsigned long)uiSniffFrames()); setTxt(wSniffCount,b);
  char lines[10][56]; int n=uiSniffLines(lines);
  if(n<=0){ setTxt(wSniffList,"(no frames yet)"); return; }
  char body[10*57+1]; body[0]=0;
  for(int i=0;i<n;i++){ strncat(body,lines[i],sizeof(body)-strlen(body)-1); if(i<n-1) strncat(body,"\n",sizeof(body)-strlen(body)-1); }
  setTxt(wSniffList,body); }

// Screenshot server: on connect to :8081, snapshot the active screen (into
// PSRAM) and stream it as raw RGB565 with a "SLYSHOT <w> <h>\n" header. Runs on
// the UI task so lv_snapshot is safe. Decode with tools/slyshot.py.
void screenshotPoll(){
  static WiFiServer* srv=nullptr; static uint8_t* sbuf=nullptr;
  if(WiFi.status()!=WL_CONNECTED) return;
  if(!srv){ srv=new WiFiServer(8081); srv->begin(); srv->setNoDelay(true); }
  if(!sbuf) sbuf=(uint8_t*)ps_malloc((size_t)800*480*2+64);
  if(!sbuf) return;
  WiFiClient c=srv->available(); if(!c) return;
  // optional: client may send a screen index (one line) to switch to first
  String cmd=""; uint32_t t0=millis();
  while(millis()-t0<250){ if(c.available()){ char ch=c.read(); if(ch=='\n'||ch=='\r') break; cmd+=ch; if(cmd.length()>3) break; } }
  if(cmd.length()>0 && gTabview){ int idx=cmd.toInt(); if(idx>=0&&idx<6){ lv_tabview_set_act(gTabview,(uint32_t)idx,LV_ANIM_OFF); lv_refr_now(NULL); } }
  lv_img_dsc_t dsc; memset(&dsc,0,sizeof(dsc));
  if(lv_snapshot_take_to_buf(lv_scr_act(),LV_IMG_CF_TRUE_COLOR,&dsc,sbuf,(uint32_t)800*480*2+64)==LV_RES_OK){
    char hdr[48]; int hn=snprintf(hdr,sizeof(hdr),"SLYSHOT %u %u\n",(unsigned)dsc.header.w,(unsigned)dsc.header.h);
    c.write((const uint8_t*)hdr,hn);
    // Hard deadline: a slow/dead client (e.g. a slyshot that timed out mid-transfer over
    // laggy WiFi) must NOT block the UI task here, or the whole UI + server freezes and
    // won't accept the next capture until reboot. Abort the send after 3s and drop it.
    // Robust send: overall 15s cap + a 2.5s no-progress stall detector. A slow-but-alive
    // client (laggy WiFi) still completes the 768KB image; a truly dead one aborts in
    // ~2.5s so it can't freeze the UI task/server (the ~1-capture-per-boot stall).
    uint32_t total=(uint32_t)dsc.header.w*dsc.header.h*2, sent=0, deadline=millis()+15000, lastProg=millis();
    while(sent<total && c.connected() && (int32_t)(millis()-deadline)<0){ uint32_t ch=total-sent; if(ch>1460) ch=1460;
      int w=c.write(sbuf+sent,ch);
      if(w<=0){ if(millis()-lastProg>2500) break; delay(2); continue; }
      sent+=(uint32_t)w; lastProg=millis(); }
  } else c.print("SLYSHOT ERR\n");
  c.stop();
}

// ---- pull-down navigation (logo -> drop-down menu; swipe still works) ----
void onNavToggle(lv_event_t*){ if(!gNavMenu) return; bool hidden=lv_obj_has_flag(gNavMenu,LV_OBJ_FLAG_HIDDEN);
  if(hidden){ lv_obj_clear_flag(gNavMenu,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gNavMenu); if(wCaret) lv_label_set_text(wCaret,LV_SYMBOL_UP); }
  else { lv_obj_add_flag(gNavMenu,LV_OBJ_FLAG_HIDDEN); if(wCaret) lv_label_set_text(wCaret,LV_SYMBOL_DOWN); } }
void onNavPick(lv_event_t*e){ int i=(int)(intptr_t)lv_event_get_user_data(e);
  if(gTabview) lv_tabview_set_act(gTabview,(uint32_t)i,LV_ANIM_OFF);
  lv_obj_add_flag(gNavMenu,LV_OBJ_FLAG_HIDDEN); if(wCaret) lv_label_set_text(wCaret,LV_SYMBOL_DOWN); }
void buildTopBar(lv_obj_t*scr){
  lv_obj_t*bar=lv_obj_create(scr); lv_obj_set_size(bar,800,48); lv_obj_set_pos(bar,0,0);
  lv_obj_set_style_bg_color(bar,lv_color_hex(COL_BG),0); lv_obj_set_style_pad_all(bar,0,0); lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_border_color(bar,lv_color_hex(COL_BORDER),0); lv_obj_set_style_border_width(bar,1,0); lv_obj_set_style_border_side(bar,LV_BORDER_SIDE_BOTTOM,0);  // #fix8: hairline under the top bar
  lv_obj_t*brand=lv_btn_create(bar); lv_obj_set_size(brand,200,46); lv_obj_align(brand,LV_ALIGN_LEFT_MID,2,0);
  lv_obj_set_style_bg_opa(brand,LV_OPA_TRANSP,0); lv_obj_set_style_shadow_width(brand,0,0); lv_obj_add_event_cb(brand,onNavToggle,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*mk=lv_img_create(brand); lv_img_set_src(mk,&slymark_img); lv_obj_align(mk,LV_ALIGN_LEFT_MID,0,0);
  lv_obj_t*wm=lv_label_create(brand); lv_label_set_text(wm,"SlyTherm"); lv_obj_set_style_text_color(wm,lv_color_hex(COL_INK),0); lv_obj_align(wm,LV_ALIGN_LEFT_MID,54,0);
  wCaret=lv_label_create(brand); lv_label_set_text(wCaret,LV_SYMBOL_DOWN); lv_obj_set_style_text_color(wCaret,lv_color_hex(COL_TEXT3),0); lv_obj_align(wCaret,LV_ALIGN_LEFT_MID,158,0);
  wOnline=lv_obj_create(bar); lv_obj_set_size(wOnline,8,8); lv_obj_set_style_radius(wOnline,LV_RADIUS_CIRCLE,0); lv_obj_set_style_border_width(wOnline,0,0); lv_obj_align(wOnline,LV_ALIGN_RIGHT_MID,-232,0);  // #fix8: 8px dot, re-pinned just left of "Outside" in renderMain
  wOat=lv_label_create(bar); lv_obj_set_style_text_color(wOat,lv_color_hex(COL_MUTED),0); lv_obj_align(wOat,LV_ALIGN_RIGHT_MID,-178,0);
  wClock=lv_label_create(bar); lv_obj_set_style_text_color(wClock,lv_color_hex(COL_MUTED),0); lv_obj_align(wClock,LV_ALIGN_RIGHT_MID,-12,0);
}
void buildNavMenu(lv_obj_t*scr){
  gNavMenu=lv_obj_create(scr); lv_obj_set_size(gNavMenu,800,480); lv_obj_set_pos(gNavMenu,0,0);
  lv_obj_set_style_bg_color(gNavMenu,lv_color_hex(0x03060A),0); lv_obj_set_style_bg_opa(gNavMenu,LV_OPA_60,0); lv_obj_set_style_border_width(gNavMenu,0,0);
  lv_obj_set_style_pad_all(gNavMenu,0,0); lv_obj_clear_flag(gNavMenu,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(gNavMenu,LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(gNavMenu,onNavToggle,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*panel=lv_obj_create(gNavMenu); lv_obj_set_size(panel,800,300); lv_obj_set_pos(panel,0,0);
  lv_obj_set_style_bg_color(panel,lv_color_hex(0x0F151C),0); lv_obj_set_style_border_width(panel,0,0); lv_obj_set_style_radius(panel,0,0);
  lv_obj_set_style_pad_all(panel,0,0); lv_obj_clear_flag(panel,LV_OBJ_FLAG_SCROLLABLE);
  const char* names[6]={"Home","Presets","Sensors","System","Settings","Diag"};
  const char* ic[6]={LV_SYMBOL_HOME,LV_SYMBOL_LIST,LV_SYMBOL_GPS,LV_SYMBOL_SETTINGS,LV_SYMBOL_SETTINGS,LV_SYMBOL_WARNING};
  for(int i=0;i<6;i++){ lv_obj_t*t=lv_btn_create(panel); lv_obj_set_size(t,244,128); lv_obj_align(t,LV_ALIGN_TOP_LEFT,16+(i%3)*256,16+(i/3)*140);
    lv_obj_set_style_bg_color(t,lv_color_hex(COL_CARD),0); lv_obj_add_event_cb(t,onNavPick,LV_EVENT_CLICKED,(void*)(intptr_t)i);
    lv_obj_t*l=lv_label_create(t); lv_label_set_text_fmt(l,"%s  %s",ic[i],names[i]); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l); }
}

// ---- #92: animated boot / warm-up splash ----
// Shown on a normal boot (not first-run, not safe-mode) while the key sensors come
// alive. Holds the UI off Home until BOTH outdoor temp and current (fused) temp are
// valid, or 60 s elapses — so the control UI never opens on stale 0.0° data. The logo
// gently breathes (opacity anim, no gradients) and a checklist fills in as each link
// comes up.
static void bootFade(void* v,int32_t o){ lv_obj_set_style_img_opa((lv_obj_t*)v,(lv_opa_t)o,0); }
static void bootZoom(void* v,int32_t z){ lv_img_set_zoom((lv_obj_t*)v,(uint16_t)z); }   // #92: size breathe
static void bootRow(lv_obj_t* l,const char* name,bool ok){ if(!l) return;
  char b[48]; snprintf(b,sizeof(b),"%s   %s",name, ok?"ready":"connecting...");   // ASCII dots: montserrat subset has no U+2026 ellipsis (tofu)
  setTxt(l,b); lv_obj_set_style_text_color(l,lv_color_hex(ok?COL_OK:COL_MUTED),0); }
void buildBoot(){
  scrBoot=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrBoot,lv_color_hex(COL_BG),0);
  lv_obj_set_style_pad_all(scrBoot,0,0); lv_obj_clear_flag(scrBoot,LV_OBJ_FLAG_SCROLLABLE);
  gBootMark=lv_img_create(scrBoot); lv_img_set_src(gBootMark,&slymark_img); lv_img_set_zoom(gBootMark,420); lv_obj_align(gBootMark,LV_ALIGN_TOP_MID,0,48);
  static lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a,gBootMark); lv_anim_set_exec_cb(&a,bootFade);
  lv_anim_set_values(&a,150,255); lv_anim_set_time(&a,1300); lv_anim_set_playback_time(&a,1300);
  lv_anim_set_repeat_count(&a,LV_ANIM_REPEAT_INFINITE); lv_anim_set_path_cb(&a,lv_anim_path_ease_in_out); lv_anim_start(&a);
  static lv_anim_t az; lv_anim_init(&az); lv_anim_set_var(&az,gBootMark); lv_anim_set_exec_cb(&az,bootZoom);   // #92: size breathe (grow/shrink) synced to the fade
  lv_anim_set_values(&az,384,468); lv_anim_set_time(&az,1300); lv_anim_set_playback_time(&az,1300);
  lv_anim_set_repeat_count(&az,LV_ANIM_REPEAT_INFINITE); lv_anim_set_path_cb(&az,lv_anim_path_ease_in_out); lv_anim_start(&az);
  lv_obj_t* h=lv_label_create(scrBoot); lv_label_set_text(h,"SlyTherm"); lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(h,lv_color_hex(COL_INK),0); lv_obj_align(h,LV_ALIGN_TOP_MID,0,246);
  wBootStat=lv_label_create(scrBoot); lv_obj_set_style_text_font(wBootStat,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(wBootStat,lv_color_hex(COL_MUTED),0); lv_obj_align(wBootStat,LV_ALIGN_TOP_MID,0,286);
  bcWifi=lv_label_create(scrBoot); lv_obj_set_style_text_font(bcWifi,&lv_font_montserrat_16,0); lv_obj_align(bcWifi,LV_ALIGN_TOP_MID,0,330);
  bcMqtt=lv_label_create(scrBoot); lv_obj_set_style_text_font(bcMqtt,&lv_font_montserrat_16,0); lv_obj_align(bcMqtt,LV_ALIGN_TOP_MID,0,358);
  bcOat=lv_label_create(scrBoot); lv_obj_set_style_text_font(bcOat,&lv_font_montserrat_16,0); lv_obj_align(bcOat,LV_ALIGN_TOP_MID,0,386);
  bcRoom=lv_label_create(scrBoot); lv_obj_set_style_text_font(bcRoom,&lv_font_montserrat_16,0); lv_obj_align(bcRoom,LV_ALIGN_TOP_MID,0,414); }
void renderBoot(const DisplayState& s){
  bootRow(bcWifi,"Wi-Fi",s.wifiOk);
  bootRow(bcMqtt,"Home Assistant",s.mqttOk);
  bootRow(bcOat,"Outdoor temperature",s.outdoorValid);
  bootRow(bcRoom,"Room sensors",s.fusedTempValid);
  setTxt(wBootStat, (s.outdoorValid&&s.fusedTempValid)?"Ready":"Warming up..."); }
// #92: warm-up done -> roll the logo across + off the right edge, spinning, then Home.
static void bootMoveX(void* v,int32_t x){ lv_obj_set_style_translate_x((lv_obj_t*)v,(lv_coord_t)x,0); }
static void bootSpin(void* v,int32_t a){ lv_img_set_angle((lv_obj_t*)v,(int16_t)a); }
static void bootDoneCb(lv_anim_t*){ gBootActive=false; gBootExiting=false;
  lv_scr_load(scrMain); if(gTabview) lv_tabview_set_act(gTabview,0,LV_ANIM_OFF);
  Serial.println("[ui] warm-up complete -> Home"); }
void bootExit(){
  if(!gBootMark){ bootDoneCb(nullptr); return; }
  lv_anim_del(gBootMark,nullptr);   // stop the breathing (fade+zoom) anims
  static lv_anim_t ex; lv_anim_init(&ex); lv_anim_set_var(&ex,gBootMark); lv_anim_set_exec_cb(&ex,bootMoveX);
  lv_anim_set_values(&ex,0,900); lv_anim_set_time(&ex,850); lv_anim_set_path_cb(&ex,lv_anim_path_ease_in);   // starts slow (roll reads) then accelerates off
  lv_anim_set_ready_cb(&ex,bootDoneCb); lv_anim_start(&ex);   // ready_cb loads Home when it's off-screen
  static lv_anim_t sp; lv_anim_init(&sp); lv_anim_set_var(&sp,gBootMark); lv_anim_set_exec_cb(&sp,bootSpin);
  lv_anim_set_values(&sp,0,7200); lv_anim_set_time(&sp,850); lv_anim_set_path_cb(&sp,lv_anim_path_linear); lv_anim_start(&sp); }   // TWO full turns -> clearly rolls

void buildUi(){ scrMain=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrMain,lv_color_hex(COL_BG),0); lv_obj_set_style_pad_all(scrMain,0,0);
  lv_obj_set_style_text_font(scrMain,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(scrMain,lv_color_hex(COL_INK),0); lv_obj_clear_flag(scrMain,LV_OBJ_FLAG_SCROLLABLE);
  buildTopBar(scrMain);
  lv_obj_t*tv=lv_tabview_create(scrMain,LV_DIR_TOP,0); gTabview=tv;
  lv_obj_set_pos(tv,0,48); lv_obj_set_size(tv,800,432);
  lv_obj_set_style_bg_color(tv,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(tv,0,0); lv_obj_set_style_pad_all(tv,0,0);
  lv_obj_add_flag(lv_tabview_get_tab_btns(tv),LV_OBJ_FLAG_HIDDEN);
  buildHome(lv_tabview_add_tab(tv,"Home")); buildPresets(lv_tabview_add_tab(tv,"Presets"));
  buildSensors(lv_tabview_add_tab(tv,"Sensors")); buildSystem(lv_tabview_add_tab(tv,"System"));
  buildSettings(lv_tabview_add_tab(tv,"Settings")); buildDiag(lv_tabview_add_tab(tv,"Diag"));
  // CRASH FIX (coredump: scroll_throw_predict_y -> lv_anim -> Panel_RGB::writeData): the
  // tabview swipe's momentum/throw animation re-enters during the RGB panel flush and
  // panics under fast/repeated swiping. Disable scroll momentum on the tab content —
  // swipe-to-switch still snaps, just no fling-throw. Also drop elastic overscroll.
  { lv_obj_t* tvc=lv_tabview_get_content(tv);
    lv_obj_clear_flag(tvc,LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(tvc,LV_OBJ_FLAG_SCROLL_ELASTIC); }
  buildNavMenu(scrMain);
  buildKeypad(scrMain); buildWifi(scrMain); buildServer(scrMain); buildHoldSheet(scrMain); buildVacationSheet(scrMain); buildAmbient(); buildWelcome(); buildBoot(); buildSniff(); lv_scr_load(scrMain); }

// #fix6: lay a setpoint card out as either the big single-mode card or a short
// Auto row (3px left color-rail). Children order in buildHome: [0]=eyebrow,
// [1]=value, [2]=minus, [3]=plus. Re-aligns them so nothing piles up at 62px.
void layoutCard(lv_obj_t*c,lv_obj_t*val,bool big,uint32_t rail){ if(!c||!val) return;
  lv_obj_t*eb=lv_obj_get_child(c,0),*mn=lv_obj_get_child(c,2),*pl=lv_obj_get_child(c,3);
  if(big){ lv_obj_set_size(c,360,204);   // larger box for single Heat/Cool
    lv_obj_set_style_border_side(c,LV_BORDER_SIDE_FULL,0); lv_obj_set_style_border_width(c,1,0); lv_obj_set_style_border_opa(c,LV_OPA_40,0); lv_obj_set_style_border_color(c,lv_color_hex(rail),0);
    if(eb){ lv_obj_set_style_text_font(eb,&lv_font_montserrat_28,0); lv_obj_align(eb,LV_ALIGN_TOP_MID,0,16); }   // 28px eyebrow (same as Auto)
    lv_obj_set_style_text_font(val,&font_set48,0); lv_obj_align(val,LV_ALIGN_TOP_MID,0,58);   // more space under the label
    if(mn){ lv_obj_set_size(mn,68,68); lv_obj_align(mn,LV_ALIGN_BOTTOM_MID,-82,-14); }
    if(pl){ lv_obj_set_size(pl,68,68); lv_obj_align(pl,LV_ALIGN_BOTTOM_MID,82,-14); } }
  else { lv_obj_set_size(c,340,150);   // Auto: big value on the LEFT, - + to the RIGHT (space between), gap between the two cards
    lv_obj_set_style_border_side(c,LV_BORDER_SIDE_LEFT,0); lv_obj_set_style_border_width(c,3,0); lv_obj_set_style_border_opa(c,LV_OPA_COVER,0); lv_obj_set_style_border_color(c,lv_color_hex(rail),0);
    if(eb){ lv_obj_set_style_text_font(eb,&lv_font_montserrat_28,0); lv_obj_align(eb,LV_ALIGN_TOP_LEFT,18,12); }   // "HEAT"/"COOL" (no TO), bigger
    lv_obj_set_style_text_font(val,&font_set48,0); lv_obj_align(val,LV_ALIGN_LEFT_MID,20,18);
    if(mn){ lv_obj_set_size(mn,60,60); lv_obj_align(mn,LV_ALIGN_RIGHT_MID,-92,18); }   // - and + to the RIGHT of the value, wide gap between them
    if(pl){ lv_obj_set_size(pl,60,60); lv_obj_align(pl,LV_ALIGN_RIGHT_MID,-16,18); } } }

// #88: presence line, shared by Home + ambient. The sticky HA-last-seen
// presence (3 h across all presence sensors) wins when a presence sensor is
// reporting: Present -> "Reading <dominant room> * Present"; away -> "Nobody
// home". With NO presence sensor reporting we fall back to the legacy temp-
// source description (which rooms are read / degraded / no sensor).
void fillPresenceLine(const DisplayState& s, char* b, size_t n){
  const DisplayState::PresenceView& p = s.presence;
  // #90: night Sleep state — subtle "Asleep" indicator on Home + ambient,
  // coherent with the #86 night deep-blank (same window drives both).
  if(p.asleep){
    if(p.valid && p.present && p.roomName[0]) snprintf(b,n,"Reading %s \xE2\x80\xA2 Asleep",p.roomName);
    else snprintf(b,n,"Asleep");
    return;
  }
  if(p.valid && p.anyReporting){
    if(p.present){
      if(p.roomName[0]) snprintf(b,n,"Reading %s \xE2\x80\xA2 Present",p.roomName);
      else              snprintf(b,n,"Present");
    } else snprintf(b,n,"Nobody home");
    return;
  }
  const SensorRow* recent=nullptr; uint32_t bestAge=0xFFFFFFFFu; int nHealthy=0;
  for(uint8_t i=0;i<s.sensorCount;i++){ const SensorRow&r=s.sensors[i]; if(!r.participating) continue;
    if(r.healthy) nHealthy++;                                   // count only rooms actually contributing
    uint32_t age=r.occupied?0u:r.lastOccAgeS;                   // most-recently-occupied wins (any room)
    if(age<bestAge){ bestAge=age; recent=&r; } }
  if(recent && recent->occupied) snprintf(b,n,"Reading %s \xE2\x80\xA2 Present",recent->name);
  else if(recent && bestAge<3600u) snprintf(b,n,"Reading %s \xE2\x80\xA2 Last entered %lu min ago",recent->name,(unsigned long)(bestAge/60u));
  else if(recent && bestAge<10800u) snprintf(b,n,"Reading %s \xE2\x80\xA2 Last entered %lu hr ago",recent->name,(unsigned long)((bestAge+1800u)/3600u));
  else if(nHealthy>0) snprintf(b,n,"Nobody home \xE2\x80\xA2 averaging %d rooms",nHealthy);
  else if(s.degradedMode) snprintf(b,n,"Local sensor only");   // only when a local slot exists (#73)
  else snprintf(b,n,"No room sensor reporting");                // no local fallback: fails to no-demand
}

// ---- render from a model snapshot ----
void renderMain(const DisplayState& s){ char b[128];
  snprintf(b,sizeof(b),"%.1f",(double)s.fusedTempC); setTxt(wTemp, s.fusedTempValid?b:"--.-");
  if(wDeg) lv_obj_align_to(wDeg,wTemp,LV_ALIGN_OUT_RIGHT_TOP,2,12);  // digits change width -> re-pin the ° superscript
  { const bool heat=s.action==HvacAction::kHeating||s.action==HvacAction::kDefrosting, cool=s.action==HvacAction::kCooling;
    if(heat){ snprintf(b,sizeof(b),"Heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_EMBER),0); }
    else if(cool){ snprintf(b,sizeof(b),"Cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_CRYO),0); }
    else if(s.mode==UserMode::kOff){ strcpy(b,"System off"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }
    else if(s.mode==UserMode::kAuto){ snprintf(b,sizeof(b),"Idle - holding %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }
    else { strcpy(b,"Idle"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }
    setTxt(wAction,b); }
  fillPresenceLine(s,b,sizeof(b)); setTxt(wFollow,b);   // #88: sticky HA-last-seen presence
  if(gHoldLbl){ char hb[40];   // hold pill (#81): active hold + remaining, or a prompt to set one
    switch(s.holdType){
      case HoldType::kTwoHours: case HoldType::kFourHours:
        snprintf(hb,sizeof(hb),"Hold \xE2\x80\xA2 %luh %02lum left",(unsigned long)(s.holdRemainS/3600u),(unsigned long)((s.holdRemainS%3600u)/60u)); break;   // #91: counts down
      case HoldType::kUntilNextPreset: strcpy(hb,"Hold until next schedule"); break;
      case HoldType::kIndefinite: strcpy(hb,"Hold until you change it"); break;
      default: strcpy(hb,"Set a hold"); break; }
    setTxt(gHoldLbl,hb); const bool held=s.holdType!=HoldType::kNone;
    lv_obj_set_style_text_color(gHoldLbl,lv_color_hex(held?COL_INK:COL_TEXT3),0);
    lv_obj_set_style_border_color(gHoldBtn,lv_color_hex(held?COL_OK:COL_BORDER),0); lv_obj_set_style_border_width(gHoldBtn,held?2:1,0);
    // #74: only show the pill when there's an ACTIVE hold (never at boot/default); hidden when off
    if(s.mode==UserMode::kOff || !held) lv_obj_add_flag(gHoldBtn,LV_OBJ_FLAG_HIDDEN); else lv_obj_clear_flag(gHoldBtn,LV_OBJ_FLAG_HIDDEN); }
  if(gVacHome){ if(s.vacationActive && s.vacationBanner[0]){ setTxt(gVacHome,s.vacationBanner); lv_obj_clear_flag(gVacHome,LV_OBJ_FLAG_HIDDEN); }   // #78 vacation banner
    else lv_obj_add_flag(gVacHome,LV_OBJ_FLAG_HIDDEN); }
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.heatSetpointC); setTxt(wHeatSp,b);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.coolSetpointC); setTxt(wCoolSp,b);
  lv_obj_set_style_bg_color(wOnline,lv_color_hex((s.wifiOk&&s.mqttOk)?COL_OK:(s.wifiOk?COL_WARN:COL_CRIT)),0);
  if(s.outdoorValid){ snprintf(b,sizeof(b),"Outside %.0f\xC2\xB0",(double)s.outdoorTempC); setTxt(wOat,b);} else setTxt(wOat,"Outside --");
  if(wOnline&&wOat) lv_obj_align_to(wOnline,wOat,LV_ALIGN_OUT_LEFT_MID,-8,0);  // #fix8: dot rides just left of the OAT label
  if(wClock) setTxt(wClock, s.clockStr[0]?s.clockStr:"");
  { const bool sh=s.mode==UserMode::kHeat||s.mode==UserMode::kAuto, sc=s.mode==UserMode::kCool||s.mode==UserMode::kAuto, au=s.mode==UserMode::kAuto;
    if(sh){ layoutCard(gHeatCard,wHeatSp,!au,COL_EMBER); lv_obj_align(gHeatCard,LV_ALIGN_TOP_RIGHT,-16,au?6:84); lv_obj_clear_flag(gHeatCard,LV_OBJ_FLAG_HIDDEN);} else lv_obj_add_flag(gHeatCard,LV_OBJ_FLAG_HIDDEN);
    if(sc){ layoutCard(gCoolCard,wCoolSp,!au,COL_CRYO); lv_obj_align(gCoolCard,LV_ALIGN_TOP_RIGHT,-16,au?182:84); lv_obj_clear_flag(gCoolCard,LV_OBJ_FLAG_HIDDEN);} else lv_obj_add_flag(gCoolCard,LV_OBJ_FLAG_HIDDEN);
    if(s.mode==UserMode::kOff) lv_obj_clear_flag(wOffMsg,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(wOffMsg,LV_OBJ_FLAG_HIDDEN);
    uint32_t tint=(sc?0x0D1720u:(s.mode==UserMode::kHeat?0x140D0Au:(uint32_t)COL_BG));  // #fix5 SOLID mode tint (no gradient — LVGL sw-gradient cache hangs the render task in our trimmed config)
    static uint32_t lastTint=0xFFFFFFFEu;  // only restyle on mode change
    if(gHomeTab && tint!=lastTint){ lastTint=tint;
      lv_obj_set_style_bg_opa(gHomeTab,LV_OPA_COVER,0); lv_obj_set_style_bg_color(gHomeTab,lv_color_hex(tint),0);
      lv_obj_set_style_bg_grad_dir(gHomeTab,LV_GRAD_DIR_NONE,0); } }
  for(int i=0;i<4;i++){ bool on=((i==0)&&s.mode==UserMode::kOff)||((i==1)&&s.mode==UserMode::kHeat)||((i==2)&&s.mode==UserMode::kCool)||((i==3)&&s.mode==UserMode::kAuto);
    lv_obj_t*sl=lv_obj_get_child(modeBtns[i],0);  // fill only the active segment (SOLID — no gradient)
    if(on){ const uint32_t c=(i==1)?COL_EMBER:COL_CRYO;
      lv_obj_set_style_bg_opa(modeBtns[i],LV_OPA_COVER,0); lv_obj_set_style_bg_color(modeBtns[i],lv_color_hex(c),0);
      lv_obj_set_style_bg_grad_dir(modeBtns[i],LV_GRAD_DIR_NONE,0);
      if(sl) lv_obj_set_style_text_color(sl,lv_color_hex(0x06202B),0); }
    else { lv_obj_set_style_bg_opa(modeBtns[i],LV_OPA_TRANSP,0); lv_obj_set_style_bg_grad_dir(modeBtns[i],LV_GRAD_DIR_NONE,0);
      if(sl) lv_obj_set_style_text_color(sl,lv_color_hex(COL_MUTED),0); } }
  { const bool optHeld=gPresetSel>=0 && millis()-gPresetSelMs<4000u;   // #75: optimistic highlight rides until the echo lands
    for(int i=0;i<(int)kMaxPresets;i++){ if(!gPresetBtns[i]) continue;
      if(i>=(int)s.presetCount){ lv_obj_add_flag(gPresetBtns[i],LV_OBJ_FLAG_HIDDEN); continue; }   // #74: only show live presets
      setTxt(gPresetName[i], s.presets[i].name);   // #74: name + values from the LIVE roster
      char pvv[40]; snprintf(pvv,sizeof(pvv),"heat %.0f\xC2\xB0   cool %.0f\xC2\xB0",(double)s.presets[i].heatC,(double)s.presets[i].coolC); setTxt(gPresetVal[i],pvv);
      bool match=fabsf(s.heatSetpointC-s.presets[i].heatC)<0.3f && fabsf(s.coolSetpointC-s.presets[i].coolC)<0.3f;
      if(match && gPresetSel==i) gPresetSel=-1;   // setpoints caught up -> drop the optimistic latch, keep the match
      bool act=match || (optHeld && gPresetSel==i);
      lv_obj_set_style_border_color(gPresetBtns[i],lv_color_hex(act?COL_OK:COL_BORDER),0); lv_obj_set_style_border_width(gPresetBtns[i],act?2:1,0);
      lv_obj_clear_flag(gPresetBtns[i],LV_OBJ_FLAG_HIDDEN); } }
  for(int i=0;i<7;i++){ SensorRowUi&ro=gSensorRows[i]; if(!ro.row) continue;
    if(i<(int)s.sensorCount){ const SensorRow&r=s.sensors[i];
      strlcpy(gRowName[i],r.name,sizeof(gRowName[i]));
      setTxt(ro.name, r.name);
      char tb[16]; snprintf(tb,sizeof(tb),"%.1f\xC2\xB0",(double)r.tempC); setTxt(ro.temp,tb);
      // #89 single status word: stale > Following (drives demand) > In use (occupied) > Away[ Nh]
      char st[24];
      if(!r.healthy) strcpy(st,"stale");
      else if(r.dominant) strcpy(st,"Following");
      else if(r.occupied) strcpy(st,"In use");
      else if(r.lastOccAgeS==0xFFFFFFFFu) strcpy(st,"Away");
      else if(r.lastOccAgeS<3600u) strcpy(st,"Away <1h");
      else snprintf(st,sizeof(st),"Away %luh",(unsigned long)(r.lastOccAgeS/3600u));
      setTxt(ro.pres,st);
      lv_obj_set_style_text_color(ro.pres,lv_color_hex(!r.healthy?COL_WARN:COL_MUTED),0);
      setTxt(ro.btnlbl, r.participating?"On":"Off"); lv_obj_set_style_bg_color(ro.btn,lv_color_hex(r.participating?COL_OK:COL_RAISED),0);
      lv_obj_clear_flag(ro.row,LV_OBJ_FLAG_HIDDEN); }
    else { gRowName[i][0]=0; lv_obj_add_flag(ro.row,LV_OBJ_FLAG_HIDDEN); } }
  if(wSysBody){ char sb[300];
    snprintf(sb,sizeof(sb),"Now running:   %s\nReading:       %.1f\xC2\xB0 from %d rooms\nOutdoor:       %s%.0f\xC2\xB0\nCompressor:    %s\nGas modulation: %.0f%%\nBus:           %s\nMode:          %s",
      actName(s.action),(double)s.fusedTempC,(int)s.sensorCount, s.outdoorValid?"":"-- ",(double)s.outdoorTempC,
      s.compressorLockoutRemainS>0?"locked out":"ready",(double)s.gasModulationPct, s.busOk?"connected (listen-only)":"--", modeName(s.mode));
    setTxt(wSysBody,sb); }
  if(wDiagBody){ char d[560]; snprintf(d,sizeof(d),"ALARMS (%u)\n",(unsigned)s.alarmCount);
    if(s.alarmCount==0) strncat(d,"  none - all clear\n",sizeof(d)-strlen(d)-1);
    for(uint8_t i=0;i<s.alarmCount && i<4;i++){ char ln[120]; snprintf(ln,sizeof(ln),"  ! %s\n",friendlyAlarm(s.alarms[i].text)); strncat(d,ln,sizeof(d)-strlen(d)-1); }
    char bus[160];
    if(s.busFrames>0) snprintf(bus,sizeof(bus),"\nCT-485 BUS\n  %lu frames decoded   bus %s\n",(unsigned long)s.busFrames,s.busOk?"alive":"quiet");
    else snprintf(bus,sizeof(bus),"\nCT-485 BUS (listen-only)\n  0 frames - wire RS-485 + enable UART to sniff\n");
    strncat(d,bus,sizeof(d)-strlen(d)-1);
    char lk[110]; snprintf(lk,sizeof(lk),"\nLINKS\n  WiFi %s   MQTT %s   Bus %s",s.wifiOk?"up":"down",s.mqttOk?"up":"down",s.busOk?"up":"down");
    strncat(d,lk,sizeof(d)-strlen(d)-1); setTxt(wDiagBody,d); }
  if(wLockState){ bool unlocked=false; L(); unlocked=gM->lockState()==LockState::kUnlocked; bool pin=gM->userPinSet(); U();
    snprintf(b,sizeof(b),"Lock: %s    PIN: %s",unlocked?"unlocked":"LOCKED",pin?"set":"none");
    setTxt(wLockState,b); lv_obj_set_style_text_color(wLockState,lv_color_hex(unlocked?COL_OK:COL_WARN),0); }
  // #77: Settings clock toggle label + green-when-working WiFi/Home-system status words
  if(gClkLbl) setTxt(gClkLbl, uiClock24()?"24-hour":"12-hour");
  if(wSetWifi){ setTxt(wSetWifi, s.wifiOk?"Connected":"Offline");
    lv_obj_set_style_text_color(wSetWifi,lv_color_hex(s.wifiOk?COL_OK:COL_CRIT),0); }
  if(wSetHome){ const bool setup=mqtt_cfg::hostSet();
    setTxt(wSetHome, s.mqttOk?"Connected":(setup?"Offline":"Not set up"));
    lv_obj_set_style_text_color(wSetHome,lv_color_hex(s.mqttOk?COL_OK:(s.wifiOk?COL_WARN:COL_CRIT)),0); } }

void renderAmbient(const DisplayState& s){ char b[80];
  // NOW temp — the same fused control temp the Home hero shows (not the raw dominant sensor)
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(aTemp, s.fusedTempValid?b:"--.-\xC2\xB0");
  { const bool heat=s.action==HvacAction::kHeating||s.action==HvacAction::kDefrosting, cool=s.action==HvacAction::kCooling;
    // #86: dim ambient theme — no bright-white temp; use the dimmed heat/cool greys
    lv_obj_set_style_text_color(aTemp,lv_color_hex(heat?COL_DIM_EMB:cool?COL_DIM_CRY:COL_TEXT3),0);
    if(heat){ snprintf(b,sizeof(b),"Heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_EMB),0); }
    else if(cool){ snprintf(b,sizeof(b),"Cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_CRY),0); }
    else if(s.mode==UserMode::kOff){ strcpy(b,"System off"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else if(s.mode==UserMode::kAuto){ snprintf(b,sizeof(b),"Idle - holding %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else { strcpy(b,"Idle"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    setTxt(aTarget,b); }
  fillPresenceLine(s,b,sizeof(b)); setTxt(aName,b);   // #88: sticky HA-last-seen presence
  if(s.alarmCount>0){ lv_obj_clear_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); setTxt(aAlarm, s.alarms[0].text[0]?friendlyAlarm(s.alarms[0].text):"alarm"); }
  else lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

// #76: push one 12 h-graph sample (~5 min cadence). Rings shift left so the
// newest point is always last (no visual wrap); Y auto-ranges around the data.
void sysGraphSample(const DisplayState& s){ if(!gSysChart) return;
  lv_coord_t* rs[3]={gRingA,gRingH,gRingC};
  for(int k=0;k<3;k++) memmove(rs[k],rs[k]+1,sizeof(lv_coord_t)*(kGraphPts-1));
  gRingA[kGraphPts-1]= s.fusedTempValid ? (lv_coord_t)lroundf(s.fusedTempC*10.0f) : LV_CHART_POINT_NONE;
  const bool sh=s.mode==UserMode::kHeat||s.mode==UserMode::kAuto, sc=s.mode==UserMode::kCool||s.mode==UserMode::kAuto;
  gRingH[kGraphPts-1]= sh ? (lv_coord_t)lroundf(s.heatSetpointC*10.0f) : LV_CHART_POINT_NONE;
  gRingC[kGraphPts-1]= sc ? (lv_coord_t)lroundf(s.coolSetpointC*10.0f) : LV_CHART_POINT_NONE;
  lv_coord_t lo=32767,hi=-32768;
  for(int k=0;k<3;k++) for(int i=0;i<kGraphPts;i++){ lv_coord_t v=rs[k][i]; if(v==LV_CHART_POINT_NONE) continue; if(v<lo)lo=v; if(v>hi)hi=v; }
  if(hi>=lo){ lo-=20; hi+=20; if(hi-lo<40){ lv_coord_t mid=(lv_coord_t)((lo+hi)/2); lo=(lv_coord_t)(mid-40); hi=(lv_coord_t)(mid+40); }  // >=2 deg padding, >=4 deg span
    lv_chart_set_range(gSysChart,LV_CHART_AXIS_PRIMARY_Y,lo,hi); }
  lv_chart_refresh(gSysChart);
  if(wSysGraphLbl){ char g[64];
    if(s.fusedTempValid) snprintf(g,sizeof(g),"now %.1f\xC2\xB0   set %.0f-%.0f\xC2\xB0",(double)s.fusedTempC,(double)s.heatSetpointC,(double)s.coolSetpointC);
    else snprintf(g,sizeof(g),"set %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC);
    setTxt(wSysGraphLbl,g); } }

// ---- reduced safe-UI (issue #80) ----
void onSafeRestore(lv_event_t*){ uiClearReducedMode(); }  // clears the NVS latch + reboots into the full UI
void buildSafeUi(){ scrSafe=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrSafe,lv_color_hex(COL_BG),0); lv_obj_set_style_pad_all(scrSafe,0,0); lv_obj_clear_flag(scrSafe,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(scrSafe); lv_label_set_text(t,"SAFE MODE"); eyebrow(t); lv_obj_set_style_text_color(t,lv_color_hex(COL_WARN),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,26,24);
  wSafeTemp=lv_label_create(scrSafe); lv_obj_set_style_text_font(wSafeTemp,&font_now80,0); lv_obj_set_style_text_color(wSafeTemp,lv_color_hex(COL_INK),0); lv_obj_align(wSafeTemp,LV_ALIGN_TOP_LEFT,20,54);
  wSafeMode=lv_label_create(scrSafe); lv_obj_set_style_text_font(wSafeMode,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(wSafeMode,lv_color_hex(COL_MUTED),0); lv_obj_align(wSafeMode,LV_ALIGN_TOP_LEFT,26,204);
  wSafeAlarm=lv_label_create(scrSafe); lv_obj_set_style_text_font(wSafeAlarm,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(wSafeAlarm,lv_color_hex(COL_CRIT),0); lv_obj_align(wSafeAlarm,LV_ALIGN_TOP_LEFT,26,252); lv_label_set_text(wSafeAlarm,"");
  lv_obj_t*sub=lv_label_create(scrSafe); lv_label_set_text(sub,"Recovered from a repeated restart. Optional screen features are off to keep the panel stable - heating/cooling control is unaffected.");
  lv_obj_set_style_text_color(sub,lv_color_hex(COL_TEXT3),0); lv_obj_set_width(sub,520); lv_label_set_long_mode(sub,LV_LABEL_LONG_WRAP); lv_obj_align(sub,LV_ALIGN_TOP_LEFT,26,300);
  lv_obj_t*b=lv_btn_create(scrSafe); lv_obj_set_size(b,300,60); lv_obj_align(b,LV_ALIGN_BOTTOM_RIGHT,-16,-16);
  lv_obj_set_style_bg_color(b,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(b,onSafeRestore,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,"Restore full screen"); lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); lv_obj_center(l);
  lv_scr_load(scrSafe); }
void renderSafe(const DisplayState& s){ char b[64];
  if(wSafeTemp){ snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(wSafeTemp, s.fusedTempValid?b:"--.-\xC2\xB0"); }
  if(wSafeMode){ snprintf(b,sizeof(b),"Mode: %s",modeName(s.mode)); setTxt(wSafeMode,b); }
  if(wSafeAlarm) setTxt(wSafeAlarm, s.alarmCount>0?friendlyAlarm(s.alarms[0].text):"All clear"); }

}  // namespace

void begin(UiModel* model, SemaphoreHandle_t mux, bool reducedUi, bool firstRun){
  gM=model; gMux=mux; gReduced=reducedUi;
  Wire.begin(kSda,kScl); Wire.setClock(400000);
  ch422M(0x01); ch422O(kBitTpRst|kBitLcdRst); delay(20);
  gfx.init(); gfx.setColorDepth(16); gfx.fillScreen(0);
  ch422O(kBitTpRst|kBitLcdRst|kBitBl);
  gtReset(); Wire.beginTransmission(kGt); gTouchOk=(Wire.endTransmission()==0);
  Serial.printf("[ui] GT911 %s\n", gTouchOk?"present":"NO ACK");
  lv_init();
  lv_disp_draw_buf_init(&drawBuf,buf1,nullptr,800*40);
  lv_disp_drv_init(&dispDrv); dispDrv.hor_res=800; dispDrv.ver_res=480; dispDrv.flush_cb=flushCb; dispDrv.draw_buf=&drawBuf; lv_disp_drv_register(&dispDrv);
  lv_indev_drv_init(&indDrv); indDrv.type=LV_INDEV_TYPE_POINTER; indDrv.read_cb=touchCb; lv_indev_drv_register(&indDrv);
  if(gReduced){ buildSafeUi(); Serial.println("[ui] SAFE MODE - reduced UI (reset-loop latch, #80)"); }
  else { buildUi();
    if(firstRun){ gWelcomeActive=true; lv_scr_load(scrWelcome); Serial.println("[ui] first run - Welcome onboarding (no saved WiFi)"); }
    else { gBootActive=true; gBootStartMs=millis(); lv_scr_load(scrBoot); Serial.println("[ui] #92 warm-up splash"); }
    Serial.println("[ui] SlyTherm wall UI up"); }
}

void service(){
  static uint32_t last=0; uint32_t now=millis(); lv_tick_inc(now-last); last=now;
  // snapshot the model under the mutex, render outside the lock
  static uint32_t lastRender=0;
  if(now-lastRender>=250){ lastRender=now;
    if(gReduced){ DisplayState s; L(); s=gM->state(); U(); renderSafe(s); screenshotPoll(); lv_timer_handler(); return; }  // #80: minimal safe screen only
    if(gSniffOpen){ renderSniff(); screenshotPoll(); lv_timer_handler(); return; }  // dedicated LISTEN screen: no ambient/main render
    DisplayState s; L(); s=gM->state(); U();
    if(gWelcomeActive){   // #82: first-run onboarding gate — stay here until Wi-Fi is up
      if(wifi_prov::connected()){ gWelcomeActive=false; gWifiOpen=false; if(wifiOv) lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN);
        lv_scr_load(scrMain); if(gTabview) lv_tabview_set_act(gTabview,0,LV_ANIM_OFF); }   // connected -> Home automatically
      else { if(!gWifiOpen && lv_scr_act()!=scrWelcome) lv_scr_load(scrWelcome);   // backed out of WiFi setup -> Welcome (never a bare screen)
        renderWifi(); screenshotPoll(); lv_timer_handler(); return; } }
    if(gBootActive){   // #92: warm-up splash — hold Home until outdoor + current temp are live (<=60s)
      if(!gBootExiting){ renderBoot(s);
        if((s.outdoorValid&&s.fusedTempValid) || now-gBootStartMs>60000u){ gBootExiting=true; bootExit(); } }   // ready: roll the logo off; its ready_cb loads Home
      screenshotPoll(); lv_timer_handler(); return; }   // stay on the splash through the roll-off anim
    if(gGraphLastMs==0 || now-gGraphLastMs>=300000u){ gGraphLastMs=now; sysGraphSample(s); }  // #76: 12 h trend, ~5 min cadence
    // Ambient starts on idle regardless of alarms; the ambient screen shows the
    // alarm banner (docs/04 §1c) rather than blocking the screensaver.
    if(!gAmbient && lv_disp_get_inactive_time(NULL)>kIdleMs){ gAmbient=true; gBlanked=false; lv_scr_load(scrAmb); gAmbShiftIdx=0; gAmbShiftMs=now; ambientShift(0); }
    if(gAmbient && now-gAmbShiftMs>=kAmbShiftMs){ gAmbShiftMs=now; ambientShift(++gAmbShiftIdx); }
    // #86a: NIGHT-ONLY deep screensaver. Within the night window (the SAME
    // kSleepStartHour..kSleepEndHour the #90 Sleep state uses, so the dark
    // screen and the Asleep state cover the same hours), after
    // kNightBlankIdleMs idle, fully blank the backlight (latched, issued once);
    // touch still reaches the GT911 with the light off and ambWake restores.
    // Outside the window we NEVER blank (ambient stays lit). getLocalTime FAIL
    // -> hour unknown -> fail SAFE: don't blank, and restore if already blanked.
    { int hour=-1; struct tm ti; if(getLocalTime(&ti,0)) hour=ti.tm_hour;
      const bool night=SleepState::inNightWindow(hour,kSleepStartHour,kSleepEndHour);
      if(gAmbient && !gBlanked && night && lv_disp_get_inactive_time(NULL)>kNightBlankIdleMs){
        gBlanked=true; ch422O(gCh&~kBitBl); }
      else if(gBlanked && !night){ renderAmbient(s); lv_refr_now(NULL); ch422O(gCh|kBitBl); gBlanked=false; }  // rolled past 06:00 (or clock lost): repaint ambient, THEN light on (no flash)
    }
    if(gAmbient){ if(!gBlanked) renderAmbient(s); }   // skip drawing while blanked (invisible)
    else { renderMain(s); renderWifi(); renderServer(); }
    static uint32_t lastSnap=0;
    if(now-lastSnap>=8000){ lastSnap=now;
      static const char* tabs[]={"Home","Presets","Sensors","System","Settings","Diag"};
      int ti=gTabview?(int)lv_tabview_get_tab_act(gTabview):0;
      const char* scr = gAmbient?"AMBIENT": gSrvOpen?"HomeSystem": gWifiOpen?"WiFi": (ti>=0&&ti<6?tabs[ti]:"?");
      telnet_log::logf("[ui] screen=%s fusedT=%.1f valid=%d mode=%d act=%d wifi=%d mqtt=%d bus=%d sensors=%d alarms=%d",
        scr,(double)s.fusedTempC,(int)s.fusedTempValid,(int)s.mode,(int)s.action,(int)s.wifiOk,(int)s.mqttOk,(int)s.busOk,(int)s.sensorCount,(int)s.alarmCount); }
  }
  screenshotPoll();
  lv_timer_handler();
}

}  // namespace slytherm_ui
