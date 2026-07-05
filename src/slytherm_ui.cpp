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
#include <lvgl.h>

#include "slytherm_ui.h"
#include "wifi_prov.h"

using namespace dettson;
using namespace dettson::ui;

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
#define COL_CRYO     0x38BDF8
#define COL_OK       0x37D39A
#define COL_WARN     0xF2C14E
#define COL_CRIT     0xFF5D5D
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
lv_obj_t *scrMain=nullptr, *scrAmb=nullptr;
bool gAmbient=false;
// main-screen widgets
lv_obj_t *wTemp,*wAction,*wHeatSp,*wCoolSp,*wWifi,*wMqtt,*wBus,*wOat,*wSensorList,*wSysBody,*wDiagBody,*wLockState;
lv_obj_t *modeBtns[4];
// ambient widgets
lv_obj_t *aName,*aTemp,*aTarget,*aAlarm;
// WiFi provisioning screen widgets
lv_obj_t *wifiOv=nullptr,*lblWifiStat=nullptr,*listNets=nullptr,*pwPanel=nullptr,*lblSel=nullptr,*taPass=nullptr,*kbd=nullptr;
bool gWifiOpen=false, gScanShown=false;
char gSelSsid[33]={};
char gNetSsids[wifi_prov::kMaxNets][33]={};
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

// ---- event handlers (touch runs on the UI task; take the mutex around model) ----
int gHoldReps=0;
void spEvt(lv_event_t*e){ const int d=(int)(intptr_t)lv_event_get_user_data(e);
  const SetpointSide side=(d==1||d==-1)?SetpointSide::kHeat:SetpointSide::kCool; const float base=(d>0)?+0.5f:-0.5f;
  const lv_event_code_t c=lv_event_get_code(e); if(c==LV_EVENT_PRESSED){ gHoldReps=0; return; }
  float step=base; if(c==LV_EVENT_LONG_PRESSED_REPEAT && ++gHoldReps>12) step=base*2.0f;
  L(); gM->adjustSetpoint(side,step); U(); }
void modeEvt(lv_event_t*e){ UserMode m=(UserMode)(intptr_t)lv_event_get_user_data(e); L(); gM->setMode(m); U(); }
void presetEvt(lv_event_t*e){ Preset p=(Preset)(intptr_t)lv_event_get_user_data(e); L(); gM->setPreset(p); U(); }

lv_obj_t* card(lv_obj_t*p){ lv_obj_t*c=lv_obj_create(p); lv_obj_set_style_bg_color(c,lv_color_hex(COL_CARD),0);
  lv_obj_set_style_border_width(c,0,0); lv_obj_set_style_radius(c,14,0); lv_obj_clear_flag(c,LV_OBJ_FLAG_SCROLLABLE); return c; }
lv_obj_t* spBtn(lv_obj_t*p,const char*t,int code,lv_align_t al){ lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,64,64);
  lv_obj_align(b,al,0,0); lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),0); void*u=(void*)(intptr_t)code;
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_PRESSED,u); lv_obj_add_event_cb(b,spEvt,LV_EVENT_SHORT_CLICKED,u);
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_LONG_PRESSED_REPEAT,u);
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,t); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l); return b; }
lv_obj_t* header(lv_obj_t*tab,const char*t){ lv_obj_t*h=lv_label_create(tab); lv_label_set_text(h,t);
  lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(h,lv_color_hex(COL_CRYO),0);
  lv_obj_align(h,LV_ALIGN_TOP_LEFT,4,0); return h; }

void buildHome(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*bar=lv_obj_create(tab); lv_obj_set_size(bar,760,34); lv_obj_align(bar,LV_ALIGN_TOP_MID,0,-6);
  lv_obj_set_style_bg_opa(bar,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(bar,0,0); lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
  wWifi=lv_label_create(bar); lv_obj_align(wWifi,LV_ALIGN_LEFT_MID,0,0);
  wMqtt=lv_label_create(bar); lv_obj_align(wMqtt,LV_ALIGN_LEFT_MID,90,0);
  wBus=lv_label_create(bar); lv_obj_align(wBus,LV_ALIGN_LEFT_MID,180,0);
  wOat=lv_label_create(bar); lv_obj_align(wOat,LV_ALIGN_LEFT_MID,270,0);
  lv_obj_t*logo=lv_obj_create(tab); lv_obj_set_size(logo,36,36); lv_obj_align(logo,LV_ALIGN_TOP_RIGHT,-8,-2);
  lv_obj_set_style_radius(logo,LV_RADIUS_CIRCLE,0); lv_obj_set_style_bg_color(logo,lv_color_hex(COL_EMBER),0);
  lv_obj_set_style_bg_grad_color(logo,lv_color_hex(COL_CRYO),0); lv_obj_set_style_bg_grad_dir(logo,LV_GRAD_DIR_HOR,0);
  lv_obj_set_style_border_color(logo,lv_color_hex(COL_CRYO),0); lv_obj_set_style_border_width(logo,2,0); lv_obj_clear_flag(logo,LV_OBJ_FLAG_SCROLLABLE);
  wTemp=lv_label_create(tab); lv_obj_set_style_text_font(wTemp,&lv_font_montserrat_48,0);
  lv_obj_set_style_text_color(wTemp,lv_color_hex(COL_INK),0); lv_obj_align(wTemp,LV_ALIGN_TOP_MID,-60,40);
  wAction=lv_label_create(tab); lv_obj_set_style_text_font(wAction,&lv_font_montserrat_28,0); lv_obj_align(wAction,LV_ALIGN_TOP_MID,-60,110);
  lv_obj_t*hc=card(tab); lv_obj_set_size(hc,210,150); lv_obj_align(hc,LV_ALIGN_LEFT_MID,20,30);
  lv_obj_t*hl=lv_label_create(hc); lv_label_set_text(hl,"HEAT"); lv_obj_set_style_text_color(hl,lv_color_hex(COL_EMBER),0); lv_obj_align(hl,LV_ALIGN_TOP_MID,0,0);
  wHeatSp=lv_label_create(hc); lv_obj_set_style_text_font(wHeatSp,&lv_font_montserrat_28,0); lv_obj_align(wHeatSp,LV_ALIGN_CENTER,0,-6);
  spBtn(hc,"-",-1,LV_ALIGN_BOTTOM_LEFT); spBtn(hc,"+",1,LV_ALIGN_BOTTOM_RIGHT);
  lv_obj_t*cc=card(tab); lv_obj_set_size(cc,210,150); lv_obj_align(cc,LV_ALIGN_RIGHT_MID,-20,30);
  lv_obj_t*cl=lv_label_create(cc); lv_label_set_text(cl,"COOL"); lv_obj_set_style_text_color(cl,lv_color_hex(COL_CRYO),0); lv_obj_align(cl,LV_ALIGN_TOP_MID,0,0);
  wCoolSp=lv_label_create(cc); lv_obj_set_style_text_font(wCoolSp,&lv_font_montserrat_28,0); lv_obj_align(wCoolSp,LV_ALIGN_CENTER,0,-6);
  spBtn(cc,"-",-2,LV_ALIGN_BOTTOM_LEFT); spBtn(cc,"+",2,LV_ALIGN_BOTTOM_RIGHT);
  const char*mn[4]={"OFF","HEAT","COOL","AUTO"}; UserMode mv[4]={UserMode::kOff,UserMode::kHeat,UserMode::kCool,UserMode::kAuto};
  lv_obj_t*mrow=lv_obj_create(tab); lv_obj_set_size(mrow,440,56); lv_obj_align(mrow,LV_ALIGN_BOTTOM_MID,0,-6);
  lv_obj_set_style_bg_opa(mrow,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(mrow,0,0);
  lv_obj_set_flex_flow(mrow,LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(mrow,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(mrow,LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<4;i++){ lv_obj_t*b=lv_btn_create(mrow); lv_obj_set_size(b,100,48);
    lv_obj_add_event_cb(b,modeEvt,LV_EVENT_CLICKED,(void*)(intptr_t)mv[i]);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,mn[i]); lv_obj_center(l); modeBtns[i]=b; } }

void buildPresets(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Presets");
  const char*nm[3]={"Home","Away","Sleep"}; const char*sub[3]={"21 / 24","17 / 28","19 / 23"};
  Preset pv[3]={Preset::kHome,Preset::kAway,Preset::kSleep};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,230,150); lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*250,52);
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_CARD),0); lv_obj_add_event_cb(b,presetEvt,LV_EVENT_CLICKED,(void*)(intptr_t)pv[i]);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,nm[i]); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,4);
    lv_obj_t*s=lv_label_create(b); lv_label_set_text_fmt(s,"heat/cool  %s",sub[i]); lv_obj_set_style_text_color(s,lv_color_hex(COL_MUTED),0); lv_obj_align(s,LV_ALIGN_BOTTOM_MID,0,-6); } }

void buildSensors(lv_obj_t*tab){ header(tab,"Room sensors");
  wSensorList=lv_label_create(tab); lv_obj_set_style_text_color(wSensorList,lv_color_hex(COL_INK),0);
  lv_obj_set_style_text_font(wSensorList,&lv_font_montserrat_20,0); lv_obj_align(wSensorList,LV_ALIGN_TOP_LEFT,4,48);
  lv_label_set_text(wSensorList,"waiting for sensors..."); }
void buildSystem(lv_obj_t*tab){ header(tab,"System");
  wSysBody=lv_label_create(tab); lv_obj_set_style_text_color(wSysBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wSysBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wSysBody,""); }
void buildDiag(lv_obj_t*tab){ header(tab,"Diagnostics");
  wDiagBody=lv_label_create(tab); lv_obj_set_style_text_color(wDiagBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wDiagBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wDiagBody,""); }

// PIN keypad
void kpadRefresh(){ char d[16]=""; for(int i=0;i<4;i++) strcat(d,i<kpN?"* ":"_ "); lv_label_set_text(kpadDots,d); }
void kpadOpen(KpMode m,const char*t){ kpMode=m; kpN=0; lv_label_set_text(kpadTitle,t); kpadRefresh();
  lv_obj_clear_flag(kpad,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(kpad); }
void kpEvt(lv_event_t*e){ const int v=(int)(intptr_t)lv_event_get_user_data(e);
  if(v==-1){ lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); return; } if(v==-2){ kpN=0; kpadRefresh(); return; }
  if(kpN<4){ kpBuf[kpN++]=(uint8_t)v; kpadRefresh(); }
  if(kpN==4){ if(kpMode==KpMode::Set){ L(); gM->setUserPin(kpBuf,kPinSalt); U(); lv_label_set_text(kpadTitle,"PIN saved - close"); }
    else { L(); gM->beginPinEntry(PinContext::kUnlock,nowS()); PinResult r=PinResult::kIdle;
      for(int i=0;i<4;i++) r=gM->enterPinDigit(kpBuf[i],nowS()); U();
      lv_label_set_text(kpadTitle, r==PinResult::kAccepted?"Unlocked - close":"Wrong PIN"); }
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

void setPinEvt(lv_event_t*){ kpadOpen(KpMode::Set,"Set a 4-digit PIN"); }
void lockEvt(lv_event_t*){ L(); bool set=gM->userPinSet(); if(set) gM->lockNow(nowS()); U(); }
void unlockEvt(lv_event_t*){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
void openWifi(lv_event_t*);  // defined below (before buildUi)
void buildSettings(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Settings");
  wLockState=lv_label_create(tab); lv_obj_set_style_text_color(wLockState,lv_color_hex(COL_MUTED),0); lv_obj_align(wLockState,LV_ALIGN_TOP_LEFT,4,60);
  struct B{const char*t; lv_event_cb_t cb;} bs[3]={{"Set PIN",setPinEvt},{"Lock",lockEvt},{"Unlock",unlockEvt}};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(tab); lv_obj_set_size(b,150,60); lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*170,120);
    lv_obj_add_event_cb(b,bs[i].cb,LV_EVENT_CLICKED,nullptr); lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,bs[i].t); lv_obj_center(l); }
  lv_obj_t*wb=lv_btn_create(tab); lv_obj_set_size(wb,220,60); lv_obj_align(wb,LV_ALIGN_TOP_LEFT,4,200);
  lv_obj_set_style_bg_color(wb,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(wb,openWifi,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*wl=lv_label_create(wb); lv_label_set_text(wl,LV_SYMBOL_WIFI "  WiFi setup"); lv_obj_set_style_text_color(wl,lv_color_hex(0x06202B),0); lv_obj_center(wl); }

// ---- ambient (idle) screen ----
void ambWake(lv_event_t*){ gAmbient=false; lv_scr_load(scrMain); }
void buildAmbient(){ scrAmb=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrAmb,lv_color_hex(COL_BG),0);
  lv_obj_add_event_cb(scrAmb,ambWake,LV_EVENT_CLICKED,nullptr); lv_obj_add_flag(scrAmb,LV_OBJ_FLAG_CLICKABLE);
  aName=lv_label_create(scrAmb); lv_obj_set_style_text_color(aName,lv_color_hex(COL_DIM),0);
  lv_obj_set_style_text_font(aName,&lv_font_montserrat_28,0); lv_obj_align(aName,LV_ALIGN_CENTER,0,-90);
  aTemp=lv_label_create(scrAmb); lv_obj_set_style_text_color(aTemp,lv_color_hex(COL_MUTED),0);
  lv_obj_set_style_text_font(aTemp,&lv_font_montserrat_48,0); lv_obj_align(aTemp,LV_ALIGN_CENTER,0,-10);
  aTarget=lv_label_create(scrAmb); lv_obj_set_style_text_font(aTarget,&lv_font_montserrat_28,0); lv_obj_align(aTarget,LV_ALIGN_CENTER,0,80);
  aAlarm=lv_label_create(scrAmb); lv_obj_set_style_text_color(aAlarm,lv_color_hex(COL_CRIT),0);   // alarm visible even in ambient (docs/04 §1c)
  lv_obj_set_style_text_font(aAlarm,&lv_font_montserrat_20,0); lv_obj_align(aAlarm,LV_ALIGN_BOTTOM_MID,0,-16); lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

// ---- WiFi provisioning screen (Settings -> WiFi setup) ----
void wifiClose(lv_event_t*){ gWifiOpen=false; lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN); }
void pwHide(){ if(pwPanel) lv_obj_add_flag(pwPanel,LV_OBJ_FLAG_HIDDEN); }
void wifiScanBtn(lv_event_t*){ wifi_prov::requestScan(); gScanShown=false; if(listNets) lv_obj_clean(listNets); }
void wifiForgetBtn(lv_event_t*){ wifi_prov::forget(); }
void wifiConnectBtn(lv_event_t*){ wifi_prov::requestConnect(gSelSsid, lv_textarea_get_text(taPass)); pwHide(); }
void kbEvt(lv_event_t*e){ lv_event_code_t c=lv_event_get_code(e); if(c==LV_EVENT_READY) wifiConnectBtn(nullptr); else if(c==LV_EVENT_CANCEL) pwHide(); }
void netEvt(lv_event_t*e){ int idx=(int)(intptr_t)lv_event_get_user_data(e); if(idx<0||idx>=wifi_prov::kMaxNets) return;
  strlcpy(gSelSsid,gNetSsids[idx],sizeof(gSelSsid)); lv_label_set_text_fmt(lblSel,"Password for %s",gSelSsid);
  lv_textarea_set_text(taPass,""); lv_keyboard_set_textarea(kbd,taPass); lv_obj_clear_flag(pwPanel,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(pwPanel); }
void buildWifi(lv_obj_t*scr){
  wifiOv=lv_obj_create(scr); lv_obj_set_size(wifiOv,800,480); lv_obj_set_pos(wifiOv,0,0);
  lv_obj_set_style_bg_color(wifiOv,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(wifiOv,0,0);
  lv_obj_clear_flag(wifiOv,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(wifiOv,LV_OBJ_FLAG_HIDDEN);
  lblWifiStat=lv_label_create(wifiOv); lv_obj_set_style_text_color(lblWifiStat,lv_color_hex(COL_INK),0);
  lv_obj_set_style_text_font(lblWifiStat,&lv_font_montserrat_20,0); lv_obj_align(lblWifiStat,LV_ALIGN_TOP_LEFT,6,12);
  struct WB{const char*t; lv_event_cb_t cb; int x;} wb[3]={{"Scan",wifiScanBtn,-320},{"Forget",wifiForgetBtn,-180},{"Close",wifiClose,-8}};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(wifiOv); lv_obj_set_size(b,120,44); lv_obj_align(b,LV_ALIGN_TOP_RIGHT,wb[i].x,4);
    lv_obj_add_event_cb(b,wb[i].cb,LV_EVENT_CLICKED,nullptr); lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,wb[i].t); lv_obj_center(l); }
  listNets=lv_list_create(wifiOv); lv_obj_set_size(listNets,780,412); lv_obj_align(listNets,LV_ALIGN_BOTTOM_MID,0,-4);
  lv_obj_set_style_bg_color(listNets,lv_color_hex(COL_CARD),0);
  pwPanel=lv_obj_create(wifiOv); lv_obj_set_size(pwPanel,800,480); lv_obj_set_pos(pwPanel,0,0);
  lv_obj_set_style_bg_color(pwPanel,lv_color_hex(COL_BG),0); lv_obj_set_style_border_width(pwPanel,0,0);
  lv_obj_clear_flag(pwPanel,LV_OBJ_FLAG_SCROLLABLE); lv_obj_add_flag(pwPanel,LV_OBJ_FLAG_HIDDEN);
  lblSel=lv_label_create(pwPanel); lv_obj_set_style_text_color(lblSel,lv_color_hex(COL_INK),0);
  lv_obj_set_style_text_font(lblSel,&lv_font_montserrat_20,0); lv_obj_align(lblSel,LV_ALIGN_TOP_LEFT,8,10);
  taPass=lv_textarea_create(pwPanel); lv_textarea_set_one_line(taPass,true); lv_textarea_set_password_mode(taPass,true);
  lv_obj_set_size(taPass,520,44); lv_obj_align(taPass,LV_ALIGN_TOP_LEFT,8,44);
  lv_obj_t*bc=lv_btn_create(pwPanel); lv_obj_set_size(bc,150,44); lv_obj_align(bc,LV_ALIGN_TOP_RIGHT,-8,44);
  lv_obj_add_event_cb(bc,wifiConnectBtn,LV_EVENT_CLICKED,nullptr); lv_obj_t*bl=lv_label_create(bc); lv_label_set_text(bl,"Connect"); lv_obj_center(bl);
  kbd=lv_keyboard_create(pwPanel); lv_keyboard_set_textarea(kbd,taPass); lv_obj_add_event_cb(kbd,kbEvt,LV_EVENT_ALL,nullptr);
}
void openWifi(lv_event_t*){ gWifiOpen=true; gScanShown=false; if(listNets) lv_obj_clean(listNets);
  lv_obj_clear_flag(wifiOv,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(wifiOv); pwHide(); wifi_prov::requestScan(); }
void renderWifi(){ if(!gWifiOpen||!lblWifiStat) return; char ss[33],ip[20]; int8_t rssi=0; bool conn=false;
  wifi_prov::status(ss,sizeof(ss),ip,sizeof(ip),&rssi,&conn); wifi_prov::ConnState cs=wifi_prov::connState(); char b[100];
  const char* cst = cs==wifi_prov::ConnState::kConnecting?"connecting...":cs==wifi_prov::ConnState::kFailed?"connect failed":"";
  if(conn) snprintf(b,sizeof(b),"Connected: %s  %s (%d dBm)",ss,ip,(int)rssi);
  else snprintf(b,sizeof(b),"Not connected  %s",cst[0]?cst:ss);
  setTxt(lblWifiStat,b); lv_obj_set_style_text_color(lblWifiStat,lv_color_hex(conn?COL_OK:COL_MUTED),0);
  if(!gScanShown && wifi_prov::scanState()==wifi_prov::ScanState::kDone){ gScanShown=true;
    wifi_prov::Net nets[wifi_prov::kMaxNets]; int n=wifi_prov::scanResults(nets,wifi_prov::kMaxNets); lv_obj_clean(listNets);
    for(int i=0;i<n;i++){ strlcpy(gNetSsids[i],nets[i].ssid,sizeof(gNetSsids[i])); char it[56];
      snprintf(it,sizeof(it),"%s   %d dBm%s",nets[i].ssid,(int)nets[i].rssi,nets[i].locked?"":"  (open)");
      lv_obj_t*btn=lv_list_add_btn(listNets,LV_SYMBOL_WIFI,it); lv_obj_add_event_cb(btn,netEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i); }
    if(n==0) lv_list_add_text(listNets,"no networks found - tap Scan"); }
}

void buildUi(){ scrMain=lv_obj_create(NULL); lv_obj_set_style_bg_color(scrMain,lv_color_hex(COL_BG),0);
  lv_obj_set_style_text_font(scrMain,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(scrMain,lv_color_hex(COL_INK),0);
  lv_obj_t*tv=lv_tabview_create(scrMain,LV_DIR_BOTTOM,56); lv_obj_set_style_bg_color(tv,lv_color_hex(COL_BG),0);
  buildHome(lv_tabview_add_tab(tv,"Home")); buildPresets(lv_tabview_add_tab(tv,"Presets"));
  buildSensors(lv_tabview_add_tab(tv,"Sensors")); buildSystem(lv_tabview_add_tab(tv,"System"));
  buildSettings(lv_tabview_add_tab(tv,"Settings")); buildDiag(lv_tabview_add_tab(tv,"Diag"));
  buildKeypad(scrMain); buildWifi(scrMain); buildAmbient(); lv_scr_load(scrMain); }

// ---- render from a model snapshot ----
void renderMain(const DisplayState& s){ char b[128];
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(wTemp, s.fusedTempValid?b:"--.-\xC2\xB0");
  setTxt(wAction,actName(s.action)); lv_obj_set_style_text_color(wAction,lv_color_hex(actCol(s.action)),0);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.heatSetpointC); setTxt(wHeatSp,b);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.coolSetpointC); setTxt(wCoolSp,b);
  snprintf(b,sizeof(b),"WiFi %s",s.wifiOk?"OK":"--"); setTxt(wWifi,b); lv_obj_set_style_text_color(wWifi,lv_color_hex(s.wifiOk?COL_OK:COL_WARN),0);
  snprintf(b,sizeof(b),"MQTT %s",s.mqttOk?"OK":"--"); setTxt(wMqtt,b); lv_obj_set_style_text_color(wMqtt,lv_color_hex(s.mqttOk?COL_OK:COL_WARN),0);
  snprintf(b,sizeof(b),"BUS %s",s.busOk?"OK":"--"); setTxt(wBus,b); lv_obj_set_style_text_color(wBus,lv_color_hex(s.busOk?COL_OK:COL_TEXT3),0);
  if(s.outdoorValid){ snprintf(b,sizeof(b),"OUT %.1f\xC2\xB0",(double)s.outdoorTempC); setTxt(wOat,b);} else setTxt(wOat,"OUT --");
  for(int i=0;i<4;i++){ bool on=((i==0)&&s.mode==UserMode::kOff)||((i==1)&&s.mode==UserMode::kHeat)||((i==2)&&s.mode==UserMode::kCool)||((i==3)&&s.mode==UserMode::kAuto);
    lv_obj_set_style_bg_color(modeBtns[i],lv_color_hex(on?COL_CRYO:COL_RAISED),0); }
  // Sensors hierarchy
  if(wSensorList){ char big[320]=""; if(s.sensorCount==0) strcpy(big,"(no sensors)");
    for(uint8_t i=0;i<s.sensorCount && i<6;i++){ const SensorRow&r=s.sensors[i]; char row[72];
      const char* pres = r.occupied?"here":(r.lastOccAgeS==0xFFFFFFFFu?"--":(r.lastOccAgeS<3600?"<1h":">1h"));
      const char* part = !r.healthy?"! fault":(r.participating?(r.dominant?"* driving":"in"):"off");
      snprintf(row,sizeof(row),"%-9s %5.1f\xC2\xB0  %-4s  %s\n", r.name,(double)r.tempC,pres,part);
      strncat(big,row,sizeof(big)-strlen(big)-1); }
    setTxt(wSensorList,big); }
  if(wSysBody){ snprintf(b,sizeof(b),"Outdoor: %.1f\xC2\xB0 (%s)\nGas mod: %.0f%%\nLockout: %lus\nMode: %s",
    (double)s.outdoorTempC,s.outdoorValid?"valid":"--",(double)s.gasModulationPct,(unsigned long)s.compressorLockoutRemainS,modeName(s.mode)); setTxt(wSysBody,b); }
  if(wDiagBody){ snprintf(b,sizeof(b),"WiFi:%s  MQTT:%s  BUS:%s\nDegraded: %s\nAlarms: %u",
    s.wifiOk?"up":"down",s.mqttOk?"up":"down",s.busOk?"up":"down",s.degradedMode?"yes":"no",(unsigned)s.alarmCount); setTxt(wDiagBody,b); }
  if(wLockState){ bool unlocked=false; L(); unlocked=gM->lockState()==LockState::kUnlocked; bool pin=gM->userPinSet(); U();
    snprintf(b,sizeof(b),"Lock: %s    PIN: %s",unlocked?"unlocked":"LOCKED",pin?"set":"none");
    setTxt(wLockState,b); lv_obj_set_style_text_color(wLockState,lv_color_hex(unlocked?COL_OK:COL_WARN),0); } }

void renderAmbient(const DisplayState& s){ char b[64];
  const SensorRow* dom=nullptr; for(uint8_t i=0;i<s.sensorCount;i++) if(s.sensors[i].dominant) dom=&s.sensors[i];
  setTxt(aName, dom?dom->name:"control temp");
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)(dom?dom->tempC:s.fusedTempC)); setTxt(aTemp,b);
  const bool heat=s.action==HvacAction::kHeating||s.action==HvacAction::kDefrosting; const bool cool=s.action==HvacAction::kCooling;
  if(heat){ snprintf(b,sizeof(b),"heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_EMB),0); }
  else if(cool){ snprintf(b,sizeof(b),"cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_CRY),0); }
  else { snprintf(b,sizeof(b),"idle"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM),0); }
  setTxt(aTarget,b);
  if(s.alarmCount>0){ lv_obj_clear_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); setTxt(aAlarm, s.alarms[0].text[0]?s.alarms[0].text:"alarm"); }
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
    DisplayState s; L(); s=gM->state(); U();
    // Ambient starts on idle regardless of alarms; the ambient screen shows the
    // alarm banner (docs/04 §1c) rather than blocking the screensaver.
    if(!gAmbient && lv_disp_get_inactive_time(NULL)>kIdleMs){ gAmbient=true; lv_scr_load(scrAmb); }
    if(gAmbient) renderAmbient(s);
    else { renderMain(s); renderWifi(); }
  }
  lv_timer_handler();
}

}  // namespace slytherm_ui
