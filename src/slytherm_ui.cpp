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
void touchCb(lv_indev_drv_t*,lv_indev_data_t*dt){ uint16_t x,y;
  if(gTouchOk&&gtRead(x,y)){ dt->state=LV_INDEV_STATE_PR; dt->point.x=x; dt->point.y=y; }
  else dt->state=LV_INDEV_STATE_REL; }

// ---- screens ----
lv_obj_t *scrMain=nullptr, *scrAmb=nullptr, *gTabview=nullptr;
bool gAmbient=false;
uint32_t gAmbShiftMs=0; uint8_t gAmbShiftIdx=0;  // ambient burn-in pixel-shift ring (#70)
// main-screen widgets
lv_obj_t *wTemp,*wDeg=nullptr,*wAction,*wHeatSp,*wCoolSp,*wWifi,*wMqtt,*wBus,*wOat,*wClock,*wSysBody,*wDiagBody,*wLockState;
lv_obj_t *gHomeTab=nullptr;  // Home tab page — parent of the hero, for the mode-tinted bg gradient (#fix5)
// Sensors screen: interactive per-room rows (#68)
struct SensorRowUi{ lv_obj_t*row,*info,*btn,*btnlbl; }; SensorRowUi gSensorRows[7]={};
char gRowName[7][16]={};
lv_obj_t *wFollow,*gHeatCard,*gCoolCard,*wOffMsg,*wOnline,*gPresetBtns[3];  // UI v2 Home/Presets
lv_obj_t *gNavMenu=nullptr,*wCaret=nullptr;  // pull-down navigation
struct PresetDef{ const char* name; float heat; float cool; };
const PresetDef kPresetDefs[3]={{"Home",21.0f,24.0f},{"Away",17.0f,28.0f},{"Sleep",19.0f,23.0f}};
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
void presetEvt(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; } Preset p=(Preset)(intptr_t)lv_event_get_user_data(e); L(); gM->setPreset(p); U(); }

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
  wTemp=lv_label_create(tab); lv_obj_set_style_text_font(wTemp,&font_now80,0); lv_obj_set_style_text_color(wTemp,lv_color_hex(COL_INK),0); lv_obj_align(wTemp,LV_ALIGN_TOP_LEFT,26,102);
  wDeg=lv_label_create(tab); lv_label_set_text(wDeg,"\xC2\xB0"); lv_obj_set_style_text_font(wDeg,&lv_font_montserrat_48,0); lv_obj_set_style_text_color(wDeg,lv_color_hex(COL_INK),0); lv_obj_align_to(wDeg,wTemp,LV_ALIGN_OUT_RIGHT_TOP,2,12);  // ° superscript (re-aligned each render — digits change width)
  wAction=lv_label_create(tab); lv_obj_set_style_text_font(wAction,&lv_font_montserrat_20,0); lv_obj_set_style_bg_opa(wAction,LV_OPA_TRANSP,0);
  lv_obj_align(wAction,LV_ALIGN_TOP_LEFT,26,270);
  wFollow=lv_label_create(tab); lv_obj_set_style_text_color(wFollow,lv_color_hex(COL_MUTED),0); lv_obj_align(wFollow,LV_ALIGN_TOP_LEFT,26,302);
  // heat + cool cards (right), big target font, shown per mode
  gHeatCard=card(tab); lv_obj_set_size(gHeatCard,340,170); lv_obj_align(gHeatCard,LV_ALIGN_TOP_RIGHT,-16,84); lv_obj_set_style_pad_all(gHeatCard,0,0);
  lv_obj_set_style_border_color(gHeatCard,lv_color_hex(COL_EMBER),0); lv_obj_set_style_border_width(gHeatCard,1,0); lv_obj_set_style_border_opa(gHeatCard,LV_OPA_40,0);
  { lv_obj_t*l=lv_label_create(gHeatCard); lv_label_set_text(l,"HEAT TO"); eyebrow(l); lv_obj_set_style_text_color(l,lv_color_hex(COL_EMBER),0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,10); }
  wHeatSp=lv_label_create(gHeatCard); lv_obj_set_style_text_font(wHeatSp,&font_set48,0); lv_obj_align(wHeatSp,LV_ALIGN_TOP_MID,0,34);
  spBtn(gHeatCard,"-",-1,LV_ALIGN_BOTTOM_MID,-76,-8); spBtn(gHeatCard,"+",1,LV_ALIGN_BOTTOM_MID,76,-8);
  gCoolCard=card(tab); lv_obj_set_size(gCoolCard,340,170); lv_obj_align(gCoolCard,LV_ALIGN_TOP_RIGHT,-16,84); lv_obj_set_style_pad_all(gCoolCard,0,0);
  lv_obj_set_style_border_color(gCoolCard,lv_color_hex(COL_CRYO),0); lv_obj_set_style_border_width(gCoolCard,1,0); lv_obj_set_style_border_opa(gCoolCard,LV_OPA_40,0);
  { lv_obj_t*l=lv_label_create(gCoolCard); lv_label_set_text(l,"COOL TO"); eyebrow(l); lv_obj_set_style_text_color(l,lv_color_hex(COL_CRYO),0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,10); }
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

void buildPresets(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Presets");
  Preset pv[3]={Preset::kHome,Preset::kAway,Preset::kSleep};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,236,150); lv_obj_align(b,LV_ALIGN_TOP_LEFT,6+i*256,52);
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(b,lv_color_hex(COL_BORDER),0); lv_obj_set_style_border_width(b,1,0);  // #fix2: dim hairline, not wireframe
    lv_obj_add_event_cb(b,presetEvt,LV_EVENT_CLICKED,(void*)(intptr_t)pv[i]); gPresetBtns[i]=b;
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,kPresetDefs[i].name); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,6);
    lv_obj_t*s=lv_label_create(b); lv_label_set_text_fmt(s,"heat %.0f\xC2\xB0   cool %.0f\xC2\xB0",(double)kPresetDefs[i].heat,(double)kPresetDefs[i].cool); lv_obj_set_style_text_color(s,lv_color_hex(COL_MUTED),0); lv_obj_align(s,LV_ALIGN_CENTER,0,6);
    lv_obj_t*h=lv_label_create(b); lv_label_set_text(h,"tap to apply"); lv_obj_set_style_text_color(h,lv_color_hex(COL_TEXT3),0); lv_obj_align(h,LV_ALIGN_BOTTOM_MID,0,-6); } }

void sensorToggleEvt(lv_event_t*e){ int i=(int)(intptr_t)lv_event_get_user_data(e); if(i<0||i>=7) return;
  if(uiLocked()){ promptUnlock(); return; } if(gRowName[i][0]) uiToggleSensor(gRowName[i]); }
void buildSensors(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Room sensors");
  for(int i=0;i<7;i++){ lv_obj_t*r=lv_obj_create(tab); lv_obj_set_size(r,760,44); lv_obj_align(r,LV_ALIGN_TOP_LEFT,4,48+i*50);
    lv_obj_set_style_bg_color(r,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_width(r,0,0); lv_obj_set_style_radius(r,8,0);
    lv_obj_set_style_pad_all(r,0,0); lv_obj_clear_flag(r,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t*info=lv_label_create(r); lv_obj_set_style_text_color(info,lv_color_hex(COL_INK),0);
    lv_obj_set_style_text_font(info,&lv_font_montserrat_20,0); lv_obj_align(info,LV_ALIGN_LEFT_MID,10,0);
    lv_obj_t*b=lv_btn_create(r); lv_obj_set_size(b,92,36); lv_obj_align(b,LV_ALIGN_RIGHT_MID,-8,0);
    lv_obj_add_event_cb(b,sensorToggleEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i);
    lv_obj_t*bl=lv_label_create(b); lv_label_set_text(bl,"--"); lv_obj_center(bl);
    gSensorRows[i]={r,info,b,bl}; gRowName[i][0]=0; lv_obj_add_flag(r,LV_OBJ_FLAG_HIDDEN); } }
void buildSystem(lv_obj_t*tab){ header(tab,"System");
  wSysBody=lv_label_create(tab); lv_obj_set_style_text_color(wSysBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wSysBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wSysBody,""); }
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

void clkEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } uiToggleClock24(); }
void setPinEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } kpadOpen(KpMode::Set,"Set a 4-digit PIN"); }
void lockEvt(lv_event_t*){ L(); bool set=gM->userPinSet(); if(set) gM->lockNow(nowS()); U(); }
void unlockEvt(lv_event_t*){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
void openWifi(lv_event_t*);    // defined below (before buildUi)
void openServer(lv_event_t*);  // defined below (before buildUi)
void buildSettings(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Settings");
  wLockState=lv_label_create(tab); lv_obj_set_style_text_color(wLockState,lv_color_hex(COL_MUTED),0); lv_obj_align(wLockState,LV_ALIGN_TOP_LEFT,4,60);
  struct B{const char*t; lv_event_cb_t cb;} bs[4]={{"Set PIN",setPinEvt},{"Lock",lockEvt},{"Unlock",unlockEvt},{"12/24h",clkEvt}};
  for(int i=0;i<4;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,150,60); lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*170,120);
    lv_obj_add_event_cb(b,bs[i].cb,LV_EVENT_CLICKED,nullptr); lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,bs[i].t); lv_obj_center(l); }
  lv_obj_t*wb=lv_btn_create(tab); lv_obj_set_size(wb,220,60); lv_obj_align(wb,LV_ALIGN_TOP_LEFT,4,200);
  lv_obj_set_style_bg_color(wb,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(wb,openWifi,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*wl=lv_label_create(wb); lv_label_set_text(wl,LV_SYMBOL_WIFI "  WiFi setup"); lv_obj_set_style_text_color(wl,lv_color_hex(0x06202B),0); lv_obj_center(wl);
  lv_obj_t*sb=lv_btn_create(tab); lv_obj_set_size(sb,220,60); lv_obj_align(sb,LV_ALIGN_TOP_LEFT,240,200);
  lv_obj_set_style_bg_color(sb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(sb,openServer,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*sl=lv_label_create(sb); lv_label_set_text(sl,LV_SYMBOL_HOME "  Home system"); lv_obj_center(sl); }

// ---- ambient (idle) screen ----
// Burn-in guard (#70): drift the whole ambient hero by a few px every 15 min.
void ambientShift(uint8_t idx){ static const int8_t dx[]={0,8,4,-6,-8,2,6}; static const int8_t dy[]={0,6,12,8,-4,-10,-8};
  int i=idx%7; int X=dx[i],Y=dy[i];
  if(aNow)lv_obj_align(aNow,LV_ALIGN_TOP_LEFT,26+X,38+Y); if(aTemp)lv_obj_align(aTemp,LV_ALIGN_TOP_LEFT,18+X,72+Y);
  if(aTarget)lv_obj_align(aTarget,LV_ALIGN_TOP_LEFT,26+X,220+Y); if(aName)lv_obj_align(aName,LV_ALIGN_TOP_LEFT,26+X,254+Y); }
void ambWake(lv_event_t*){ gAmbient=false; gAmbShiftIdx=0; ambientShift(0); lv_scr_load(scrMain); }
void buildAmbient(){ scrAmb=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrAmb,lv_color_hex(COL_BG),0); lv_obj_set_style_pad_all(scrAmb,0,0);
  lv_obj_add_event_cb(scrAmb,ambWake,LV_EVENT_CLICKED,nullptr); lv_obj_add_flag(scrAmb,LV_OBJ_FLAG_CLICKABLE); lv_obj_clear_flag(scrAmb,LV_OBJ_FLAG_SCROLLABLE);
  // mirror the Home hero: NOW + big fused temp + action + what's being tracked
  aNow=lv_label_create(scrAmb); lv_label_set_text(aNow,"NOW"); eyebrow(aNow); lv_obj_set_style_text_color(aNow,lv_color_hex(COL_TEXT3),0); lv_obj_align(aNow,LV_ALIGN_TOP_LEFT,26,38);
  aTemp=lv_label_create(scrAmb); lv_obj_set_style_text_font(aTemp,&font_now80,0); lv_obj_set_style_text_color(aTemp,lv_color_hex(COL_INK),0); lv_obj_align(aTemp,LV_ALIGN_TOP_LEFT,18,72);
  aTarget=lv_label_create(scrAmb); lv_obj_set_style_text_font(aTarget,&lv_font_montserrat_20,0); lv_obj_align(aTarget,LV_ALIGN_TOP_LEFT,26,220);
  aName=lv_label_create(scrAmb); lv_obj_set_style_text_color(aName,lv_color_hex(COL_MUTED),0); lv_obj_set_style_text_font(aName,&lv_font_montserrat_20,0); lv_obj_align(aName,LV_ALIGN_TOP_LEFT,26,254);
  aAlarm=lv_label_create(scrAmb); lv_obj_set_style_text_color(aAlarm,lv_color_hex(COL_CRIT),0);   // alarm visible even in ambient (docs/04 §1c)
  lv_obj_set_style_text_font(aAlarm,&lv_font_montserrat_20,0); lv_obj_align(aAlarm,LV_ALIGN_BOTTOM_MID,0,-16); lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

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
void onDone(lv_event_t*){ wifiGoto(WifiState::Status); }
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
    uint32_t total=(uint32_t)dsc.header.w*dsc.header.h*2, sent=0;
    while(sent<total && c.connected()){ uint32_t ch=total-sent; if(ch>1460) ch=1460;
      int w=c.write(sbuf+sent,ch); if(w<=0) break; sent+=(uint32_t)w; }
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
  buildNavMenu(scrMain);
  buildKeypad(scrMain); buildWifi(scrMain); buildServer(scrMain); buildAmbient(); buildSniff(); lv_scr_load(scrMain); }

// #fix6: lay a setpoint card out as either the big single-mode card or a short
// Auto row (3px left color-rail). Children order in buildHome: [0]=eyebrow,
// [1]=value, [2]=minus, [3]=plus. Re-aligns them so nothing piles up at 62px.
void layoutCard(lv_obj_t*c,lv_obj_t*val,bool big,uint32_t rail){ if(!c||!val) return;
  lv_obj_t*eb=lv_obj_get_child(c,0),*mn=lv_obj_get_child(c,2),*pl=lv_obj_get_child(c,3);
  if(big){ lv_obj_set_size(c,340,170);
    lv_obj_set_style_border_side(c,LV_BORDER_SIDE_FULL,0); lv_obj_set_style_border_width(c,1,0); lv_obj_set_style_border_opa(c,LV_OPA_40,0); lv_obj_set_style_border_color(c,lv_color_hex(rail),0);
    if(eb) lv_obj_align(eb,LV_ALIGN_TOP_MID,0,10);
    lv_obj_set_style_text_font(val,&font_set48,0); lv_obj_align(val,LV_ALIGN_TOP_MID,0,34);
    if(mn){ lv_obj_set_size(mn,64,64); lv_obj_align(mn,LV_ALIGN_BOTTOM_MID,-76,-8); }
    if(pl){ lv_obj_set_size(pl,64,64); lv_obj_align(pl,LV_ALIGN_BOTTOM_MID,76,-8); } }
  else { lv_obj_set_size(c,340,168);   // Auto: two FULL-size cards (same big value + steppers as single), left color-rail
    lv_obj_set_style_border_side(c,LV_BORDER_SIDE_LEFT,0); lv_obj_set_style_border_width(c,3,0); lv_obj_set_style_border_opa(c,LV_OPA_COVER,0); lv_obj_set_style_border_color(c,lv_color_hex(rail),0);
    if(eb) lv_obj_align(eb,LV_ALIGN_TOP_MID,0,8);
    lv_obj_set_style_text_font(val,&font_set48,0); lv_obj_align(val,LV_ALIGN_TOP_MID,0,30);
    if(mn){ lv_obj_set_size(mn,64,64); lv_obj_align(mn,LV_ALIGN_BOTTOM_MID,-88,-6); }   // big steppers, wide gap between - and +
    if(pl){ lv_obj_set_size(pl,64,64); lv_obj_align(pl,LV_ALIGN_BOTTOM_MID,88,-6); } } }

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
  { const SensorRow* recent=nullptr; uint32_t bestAge=0xFFFFFFFFu; int nHealthy=0;
    for(uint8_t i=0;i<s.sensorCount;i++){ const SensorRow&r=s.sensors[i]; if(!r.participating) continue;
      if(r.healthy) nHealthy++;                                   // count only rooms actually contributing
      uint32_t age=r.occupied?0u:r.lastOccAgeS;                   // most-recently-occupied wins (any room, not just dominant)
      if(age<bestAge){ bestAge=age; recent=&r; } }
    if(recent && recent->occupied) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Present",recent->name);
    else if(recent && bestAge<3600u) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Last entered %lu min ago",recent->name,(unsigned long)(bestAge/60u));
    else if(recent && bestAge<10800u) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Last entered %lu hr ago",recent->name,(unsigned long)((bestAge+1800u)/3600u));
    else if(nHealthy>0) snprintf(b,sizeof(b),"Nobody home \xE2\x80\xA2 averaging %d rooms",nHealthy);
    else strcpy(b,"Local sensor only");
    setTxt(wFollow,b); }
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
  for(int i=0;i<3;i++){ if(!gPresetBtns[i]) continue;
    bool act=fabsf(s.heatSetpointC-kPresetDefs[i].heat)<0.3f && fabsf(s.coolSetpointC-kPresetDefs[i].cool)<0.3f;
    lv_obj_set_style_border_color(gPresetBtns[i],lv_color_hex(act?COL_OK:COL_BORDER),0); lv_obj_set_style_border_width(gPresetBtns[i],act?2:1,0); }
  for(int i=0;i<7;i++){ SensorRowUi&ro=gSensorRows[i]; if(!ro.row) continue;
    if(i<(int)s.sensorCount){ const SensorRow&r=s.sensors[i];
      strlcpy(gRowName[i],r.name,sizeof(gRowName[i]));
      const char* pres = r.occupied?"here":(r.lastOccAgeS==0xFFFFFFFFu?"-":(r.lastOccAgeS<3600u?"<1h":">1h"));
      const char* use = !r.healthy?"check it":(r.participating?(r.dominant?"following":"in use"):"off");
      char row[80]; snprintf(row,sizeof(row),"%-11s %5.1f\xC2\xB0  %-4s  %s", r.name,(double)r.tempC,pres,use); setTxt(ro.info,row);
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
    setTxt(wLockState,b); lv_obj_set_style_text_color(wLockState,lv_color_hex(unlocked?COL_OK:COL_WARN),0); } }

void renderAmbient(const DisplayState& s){ char b[80];
  // NOW temp — the same fused control temp the Home hero shows (not the raw dominant sensor)
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(aTemp, s.fusedTempValid?b:"--.-\xC2\xB0");
  { const bool heat=s.action==HvacAction::kHeating||s.action==HvacAction::kDefrosting, cool=s.action==HvacAction::kCooling;
    if(heat){ snprintf(b,sizeof(b),"Heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_EMBER),0); }
    else if(cool){ snprintf(b,sizeof(b),"Cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_CRYO),0); }
    else if(s.mode==UserMode::kOff){ strcpy(b,"System off"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else if(s.mode==UserMode::kAuto){ snprintf(b,sizeof(b),"Idle - holding %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else { strcpy(b,"Idle"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    setTxt(aTarget,b); }
  { const SensorRow* recent=nullptr; uint32_t bestAge=0xFFFFFFFFu; int nHealthy=0;
    for(uint8_t i=0;i<s.sensorCount;i++){ const SensorRow&r=s.sensors[i]; if(!r.participating) continue;
      if(r.healthy) nHealthy++;
      uint32_t age=r.occupied?0u:r.lastOccAgeS;
      if(age<bestAge){ bestAge=age; recent=&r; } }
    if(recent && recent->occupied) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Present",recent->name);
    else if(recent && bestAge<3600u) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Last entered %lu min ago",recent->name,(unsigned long)(bestAge/60u));
    else if(recent && bestAge<10800u) snprintf(b,sizeof(b),"Reading %s \xE2\x80\xA2 Last entered %lu hr ago",recent->name,(unsigned long)((bestAge+1800u)/3600u));
    else if(nHealthy>0) snprintf(b,sizeof(b),"Nobody home \xE2\x80\xA2 averaging %d rooms",nHealthy);
    else strcpy(b,"Local sensor only");
    setTxt(aName,b); }
  if(s.alarmCount>0){ lv_obj_clear_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); setTxt(aAlarm, s.alarms[0].text[0]?friendlyAlarm(s.alarms[0].text):"alarm"); }
  else lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

}  // namespace

void begin(UiModel* model, SemaphoreHandle_t mux){
  gM=model; gMux=mux;
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
  buildUi();
  Serial.println("[ui] SlyTherm wall UI up");
}

void service(){
  static uint32_t last=0; uint32_t now=millis(); lv_tick_inc(now-last); last=now;
  // snapshot the model under the mutex, render outside the lock
  static uint32_t lastRender=0;
  if(now-lastRender>=250){ lastRender=now;
    if(gSniffOpen){ renderSniff(); screenshotPoll(); lv_timer_handler(); return; }  // dedicated LISTEN screen: no ambient/main render
    DisplayState s; L(); s=gM->state(); U();
    // Ambient starts on idle regardless of alarms; the ambient screen shows the
    // alarm banner (docs/04 §1c) rather than blocking the screensaver.
    if(!gAmbient && lv_disp_get_inactive_time(NULL)>kIdleMs){ gAmbient=true; lv_scr_load(scrAmb); gAmbShiftIdx=0; gAmbShiftMs=now; ambientShift(0); }
    if(gAmbient && now-gAmbShiftMs>=900000u){ gAmbShiftMs=now; ambientShift(++gAmbShiftIdx); }
    if(gAmbient) renderAmbient(s);
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
