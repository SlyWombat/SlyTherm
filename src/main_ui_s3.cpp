// main_ui_s3.cpp — SlyTherm wall UI (issue #37), standalone LVGL app on the
// Waveshare ESP32-S3-4.3B. Builds the real multi-screen UI bound to
// lib/UiModel, driven here by a DEMO control-echo (drains UiModel intents and
// reflects them) so every screen is interactive and verifiable BEFORE wiring
// into the threaded control task in main_thermostat.cpp.
//
// Display/touch glue is the stage-1 validated recipe (see main_ui_smoke.cpp /
// memory: LVGL v8, 800x40 internal-RAM partial buffer, GT911 raw I2C @0x5D).

#include <Arduino.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>   // declares lgfx::Panel_RGB
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>     // declares lgfx::Bus_RGB
#include <Wire.h>
#include <lvgl.h>

#include "UiModel.h"

// LovyanGFX device for the Waveshare 4.3 RGB panel — its RGB driver manages the
// framebuffer DMA robustly (fixes the Arduino_GFX offset + touch lag). Config
// from the board's LovyanGFX reference (Pins.h): 14 MHz, hsync 20/10/10,
// vsync 10/10/10, pclk_active_neg 0.
class LGFX : public lgfx::LGFX_Device {
  lgfx::Bus_RGB   _bus;
  lgfx::Panel_RGB _panel;
 public:
  LGFX(){
    { auto c=_panel.config(); c.memory_width=800; c.memory_height=480;
      c.panel_width=800; c.panel_height=480; c.offset_x=0; c.offset_y=0; _panel.config(c); }
    { auto c=_panel.config_detail(); c.use_psram=1; _panel.config_detail(c); } // 768KB FB in PSRAM
    { auto c=_bus.config(); c.panel=&_panel;
      c.pin_d0=14; c.pin_d1=38; c.pin_d2=18; c.pin_d3=17; c.pin_d4=10;         // B0-4
      c.pin_d5=39; c.pin_d6=0;  c.pin_d7=45; c.pin_d8=48; c.pin_d9=47; c.pin_d10=21; // G0-5
      c.pin_d11=1; c.pin_d12=2; c.pin_d13=42; c.pin_d14=41; c.pin_d15=40;      // R0-4
      c.pin_henable=5; c.pin_vsync=3; c.pin_hsync=46; c.pin_pclk=7; c.freq_write=14000000;
      c.hsync_polarity=0; c.hsync_front_porch=20; c.hsync_pulse_width=10; c.hsync_back_porch=10;
      c.vsync_polarity=0; c.vsync_front_porch=10; c.vsync_pulse_width=10; c.vsync_back_porch=10;
      c.pclk_active_neg=0; c.de_idle_high=0; c.pclk_idle_high=0; _bus.config(c); }
    _panel.setBus(&_bus); setPanel(&_panel);
  }
};

using namespace dettson;
using namespace dettson::ui;

// ================= panel + CH422G + GT911 (validated stage 1) ================
static constexpr uint8_t kCh422ModeAddr = 0x24, kCh422OutAddr = 0x38;
static constexpr uint8_t kBitTpRst = 1 << 1, kBitLcdBl = 1 << 2, kBitLcdRst = 1 << 3;
static uint8_t gCh422Out = 0;
static void ch422Mode(uint8_t v){ Wire.beginTransmission(kCh422ModeAddr); Wire.write(v); Wire.endTransmission(); }
static void ch422SetOut(uint8_t v){ gCh422Out=v; Wire.beginTransmission(kCh422OutAddr); Wire.write(v); Wire.endTransmission(); }

static LGFX gfx;

static constexpr int     kSda=8, kScl=9, kTouchInt=4;
static constexpr uint8_t kGt911Addr=0x5D;
static bool gGt911Ok=false;

static void gt911Reset(){
  pinMode(kTouchInt,OUTPUT); digitalWrite(kTouchInt,LOW);
  ch422SetOut(gCh422Out & ~kBitTpRst); delay(10);
  digitalWrite(kTouchInt,LOW); delayMicroseconds(120);
  ch422SetOut(gCh422Out | kBitTpRst); delay(5);
  digitalWrite(kTouchInt,LOW); delay(50);
  pinMode(kTouchInt,INPUT); delay(50);
}
static int gt911ReadReg(uint16_t reg,uint8_t* b,uint8_t n){
  Wire.beginTransmission(kGt911Addr); Wire.write(reg>>8); Wire.write(reg&0xFF);
  if(Wire.endTransmission(false)!=0) return 0;
  uint8_t got=Wire.requestFrom(kGt911Addr,n),i=0;
  while(i<got && Wire.available()) b[i++]=Wire.read();
  return i;
}
static bool gt911Read(uint16_t& x,uint16_t& y){
  // Hold the last state BETWEEN GT911 samples so a continuous press reads as
  // continuously down (the ready-bit is only set per new sample; reporting
  // "released" in the gaps broke press-and-hold / made touch feel laggy).
  static uint16_t lx=0,ly=0; static bool down=false;
  uint8_t s=0;
  if(gt911ReadReg(0x814E,&s,1)==1 && (s&0x80)){        // a new sample is ready
    if((s&0x0F)>0){ uint8_t d[6]={0};
      if(gt911ReadReg(0x8150,d,6)>=4){ lx=d[0]|(d[1]<<8); ly=d[2]|(d[3]<<8); down=true; } }
    else down=false;                                    // fresh sample, no touch = up
    Wire.beginTransmission(kGt911Addr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission();
  }
  x=lx; y=ly; return down;
}

static constexpr uint16_t kHor=800, kVer=480;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[kHor*40];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void flushCb(lv_disp_drv_t* d,const lv_area_t* a,lv_color_t* px){
  gfx.pushImage(a->x1,a->y1,a->x2-a->x1+1,a->y2-a->y1+1,(lgfx::rgb565_t*)px);
  lv_disp_flush_ready(d);
}
static void touchCb(lv_indev_drv_t*,lv_indev_data_t* data){
  uint16_t x,y;
  if(gGt911Ok && gt911Read(x,y)){ data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y; }
  else data->state=LV_INDEV_STATE_REL;
}

// ================= UI palette — SlyTherm design system (docs/09) ==============
#define COL_BG       0x0B0F14   // neutral bg
#define COL_CARD     0x151C25   // surface
#define COL_RAISED   0x1F2A36   // surface.raised (buttons/keypad)
#define COL_BORDER   0x2B3947   // border
#define COL_INK      0xEAF0F6   // text.primary  (headings, values)
#define COL_MUTED    0xAEB9C4   // text.secondary (body rows — bright enough to kill grey fringing)
#define COL_TEXT3    0x7A8895   // text.tertiary  (captions only)
#define COL_EMBER    0xFF7A18   // accent.heat
#define COL_EMBER_HI 0xFFB020   // accent.heat.hi
#define COL_CRYO     0x38BDF8   // accent.cool
#define COL_CRYO_HI  0x7DD3FC   // accent.cool.hi
#define COL_OK       0x37D39A   // state.ok
#define COL_WARN     0xF2C14E   // state.warning
#define COL_CRIT     0xFF5D5D   // state.critical

// ================= model + demo control echo =================================
static UiModel gModel;

// Demo "control task": simulates a room that drifts toward the active setpoint
// and drains UiModel intents so touch changes are reflected.
struct DemoCtl {
  float roomC = 21.3f;
  UserMode mode = UserMode::kOff;
  float heatC = 20.0f, coolC = 24.0f;
  bool heating = false, cooling = false;   // latched with hysteresis
  uint32_t frames = 0, framesOk = 0, badcrc = 0;   // demo CT-485 sniffer counters
  void step(uint32_t nowS) {
    UiIntent it;
    while (gModel.popIntent(it)) {
      switch (it.type) {
        case IntentType::kSetSetpoints: heatC = it.heatC; coolC = it.coolC; break;
        case IntentType::kSetMode:      mode = it.mode; break;
        case IntentType::kSetPreset:
          switch (it.preset) {
            case Preset::kHome:  heatC = 21.0f; coolC = 24.0f; break;
            case Preset::kAway:  heatC = 17.0f; coolC = 28.0f; break;
            case Preset::kSleep: heatC = 19.0f; coolC = 23.0f; break;
          } break;
        default: break;
      }
    }
    // Latching hysteresis: heat turns on 0.4 below setpoint, off at setpoint
    // (cool mirrored) — so it settles to Idle instead of chattering.
    const bool heatMode = (mode==UserMode::kHeat||mode==UserMode::kAuto||mode==UserMode::kEmergencyHeat);
    const bool coolMode = (mode==UserMode::kCool||mode==UserMode::kAuto);
    if (heatMode){ if(!heating && roomC <= heatC-0.4f) heating=true; if(heating && roomC >= heatC) heating=false; } else heating=false;
    if (coolMode){ if(!cooling && roomC >= coolC+0.4f) cooling=true; if(cooling && roomC <= coolC) cooling=false; } else cooling=false;
    HvacAction act = HvacAction::kIdle; uint8_t equip = kEquipNone; float mod = 0;
    if (heating){ roomC += 0.06f; act=HvacAction::kHeating; equip=kEquipGas|kEquipFan; mod=60; }
    else if (cooling){ roomC -= 0.06f; act=HvacAction::kCooling; equip=kEquipHpCool|kEquipFan; }
    else roomC += (roomC>19.0f ? -0.004f : 0.004f);   // slow drift toward ambient when idle
    gModel.setFusedTemp(roomC, true);
    gModel.setUserMode(mode);
    gModel.setSetpoints(heatC, coolC);
    gModel.setHvacAction(act);
    gModel.setActiveEquipment(equip);
    gModel.setGasModulationPct(mod);
    gModel.setOutdoor(-4.0f, true, OutdoorSource::kHaWeather);
    gModel.setLinkHealth(true, true, false);

    // demo remote sensors (so the Sensors tab has content)
    SensorRow rows[3] = {};
    strncpy(rows[0].name,"living", sizeof(rows[0].name)-1);
    rows[0].tempC=roomC;        rows[0].occupied=true;  rows[0].ageS=5;  rows[0].participating=true;  rows[0].healthy=true;
    strncpy(rows[1].name,"bedroom",sizeof(rows[1].name)-1);
    rows[1].tempC=roomC-0.8f;   rows[1].occupied=false; rows[1].ageS=14; rows[1].participating=true;  rows[1].healthy=true;
    strncpy(rows[2].name,"office", sizeof(rows[2].name)-1);
    rows[2].tempC=roomC+0.6f;   rows[2].occupied=false; rows[2].ageS=95; rows[2].participating=false; rows[2].healthy=true;
    gModel.setSensorRows(rows,3);

    // demo CT-485 sniffer traffic
    frames += 7; framesOk += 7;
    if ((nowS % 9) == 0) badcrc++;
  }
} gDemo;

// ================= screen widgets (updated in render) ========================
static lv_obj_t *wTemp, *wAction, *wHeatSp, *wCoolSp, *wModeLbl;
static lv_obj_t *wWifi, *wMqtt, *wBus, *wOat;
static lv_obj_t *wSensorList, *wSysBody, *wDiagBody, *wLockState;
static lv_obj_t *modeBtns[4];

// PIN keypad overlay (exercises UiModel's lock API)
static lv_obj_t *kpad=nullptr, *kpadTitle=nullptr, *kpadDots=nullptr;
enum class KpMode { Set, Unlock };
static KpMode  kpMode = KpMode::Set;
static uint8_t kpBuf[4];
static int     kpN = 0;
static uint32_t nowSeconds(){ return millis()/1000; }
static constexpr uint32_t kPinSalt = 0x5A17C0DE;

static const char* modeName(UserMode m){
  switch(m){case UserMode::kHeat:return "HEAT";case UserMode::kCool:return "COOL";
    case UserMode::kAuto:return "AUTO";case UserMode::kEmergencyHeat:return "EM HEAT";default:return "OFF";}
}
static const char* actionName(HvacAction a){
  switch(a){case HvacAction::kHeating:return "Heating";case HvacAction::kCooling:return "Cooling";
    case HvacAction::kFanOnly:return "Fan";case HvacAction::kDefrosting:return "Defrost";default:return "Idle";}
}
static uint32_t actionColor(HvacAction a){
  switch(a){case HvacAction::kHeating:case HvacAction::kDefrosting:return COL_EMBER;
    case HvacAction::kCooling:return COL_CRYO;default:return COL_MUTED;}
}

// ---- event handlers ----
static int gHoldReps = 0;
static void spEvt(lv_event_t* e){
  // encoded code: 1=heat+, -1=heat-, 2=cool+, -2=cool-
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  const SetpointSide side = (d==1||d==-1) ? SetpointSide::kHeat : SetpointSide::kCool;
  const float base = (d>0) ? +0.5f : -0.5f;
  const lv_event_code_t code = lv_event_get_code(e);
  if (code == LV_EVENT_PRESSED) { gHoldReps = 0; return; }  // new press: reset accel
  float step = base;                                        // tap or held repeat
  if (code == LV_EVENT_LONG_PRESSED_REPEAT && ++gHoldReps > 12)
    step = base * 2.0f;                                     // held ~1s+: 1.0 deg steps
  gModel.adjustSetpoint(side, step);
}
static void modeEvt(lv_event_t* e){
  UserMode m = (UserMode)(intptr_t)lv_event_get_user_data(e);
  gModel.setMode(m);
}

// ---- builders ----
static lv_obj_t* card(lv_obj_t* parent){
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_style_bg_color(c, lv_color_hex(COL_CARD), 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_radius(c, 14, 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}
static lv_obj_t* spButton(lv_obj_t* parent,const char* txt,int code,lv_align_t al){
  lv_obj_t* b=lv_btn_create(parent); lv_obj_set_size(b,64,64);
  lv_obj_align(b,al,0,0);
  lv_obj_set_style_bg_color(b, lv_color_hex(0x223040), 0);
  void* ud=(void*)(intptr_t)code;
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_PRESSED,ud);              // reset accel
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_SHORT_CLICKED,ud);        // single tap
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_LONG_PRESSED_REPEAT,ud);  // hold = repeat+accel
  lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,txt);
  lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l);
  return b;
}

static void buildHome(lv_obj_t* tab){
  lv_obj_clear_flag(tab, LV_OBJ_FLAG_SCROLLABLE);
  // status bar
  lv_obj_t* bar=lv_obj_create(tab); lv_obj_set_size(bar,760,34); lv_obj_align(bar,LV_ALIGN_TOP_MID,0,-6);
  lv_obj_set_style_bg_opa(bar,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(bar,0,0);
  lv_obj_clear_flag(bar,LV_OBJ_FLAG_SCROLLABLE);
  wWifi=lv_label_create(bar); lv_obj_align(wWifi,LV_ALIGN_LEFT_MID,0,0);
  wMqtt=lv_label_create(bar); lv_obj_align(wMqtt,LV_ALIGN_LEFT_MID,90,0);
  wBus =lv_label_create(bar); lv_obj_align(wBus,LV_ALIGN_LEFT_MID,180,0);
  wOat =lv_label_create(bar); lv_obj_align(wOat,LV_ALIGN_LEFT_MID,270,0);

  // small SlyTherm dial logo (top-right): ember->cryo gradient roundel + ring
  lv_obj_t* logo=lv_obj_create(tab);
  lv_obj_set_size(logo,36,36); lv_obj_align(logo,LV_ALIGN_TOP_RIGHT,-8,-2);
  lv_obj_set_style_radius(logo,LV_RADIUS_CIRCLE,0);
  lv_obj_set_style_bg_color(logo,lv_color_hex(COL_EMBER),0);
  lv_obj_set_style_bg_grad_color(logo,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_bg_grad_dir(logo,LV_GRAD_DIR_HOR,0);
  lv_obj_set_style_border_color(logo,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(logo,2,0);
  lv_obj_clear_flag(logo,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* lw=lv_label_create(tab); lv_label_set_text(lw,"SlyTherm");
  lv_obj_set_style_text_color(lw,lv_color_hex(COL_INK),0);
  lv_obj_align_to(lw,logo,LV_ALIGN_OUT_LEFT_MID,-6,0);

  // big temperature
  wTemp=lv_label_create(tab); lv_obj_set_style_text_font(wTemp,&lv_font_montserrat_48,0);
  lv_obj_set_style_text_color(wTemp,lv_color_hex(COL_INK),0);
  lv_obj_align(wTemp,LV_ALIGN_TOP_MID,-60,40);
  wAction=lv_label_create(tab); lv_obj_set_style_text_font(wAction,&lv_font_montserrat_28,0);
  lv_obj_align(wAction,LV_ALIGN_TOP_MID,-60,110);

  // setpoint cards
  lv_obj_t* hc=card(tab); lv_obj_set_size(hc,210,150); lv_obj_align(hc,LV_ALIGN_LEFT_MID,20,30);
  lv_obj_t* hl=lv_label_create(hc); lv_label_set_text(hl,"HEAT"); lv_obj_set_style_text_color(hl,lv_color_hex(COL_EMBER),0);
  lv_obj_align(hl,LV_ALIGN_TOP_MID,0,0);
  wHeatSp=lv_label_create(hc); lv_obj_set_style_text_font(wHeatSp,&lv_font_montserrat_28,0);
  lv_obj_align(wHeatSp,LV_ALIGN_CENTER,0,-6);
  spButton(hc,"-",-1,LV_ALIGN_BOTTOM_LEFT);
  spButton(hc,"+",1, LV_ALIGN_BOTTOM_RIGHT);

  lv_obj_t* cc=card(tab); lv_obj_set_size(cc,210,150); lv_obj_align(cc,LV_ALIGN_RIGHT_MID,-20,30);
  lv_obj_t* cl=lv_label_create(cc); lv_label_set_text(cl,"COOL"); lv_obj_set_style_text_color(cl,lv_color_hex(COL_CRYO),0);
  lv_obj_align(cl,LV_ALIGN_TOP_MID,0,0);
  wCoolSp=lv_label_create(cc); lv_obj_set_style_text_font(wCoolSp,&lv_font_montserrat_28,0);
  lv_obj_align(wCoolSp,LV_ALIGN_CENTER,0,-6);
  spButton(cc,"-",-2,LV_ALIGN_BOTTOM_LEFT);
  spButton(cc,"+",2, LV_ALIGN_BOTTOM_RIGHT);

  // mode selector
  const char* mn[4]={"OFF","HEAT","COOL","AUTO"};
  UserMode mv[4]={UserMode::kOff,UserMode::kHeat,UserMode::kCool,UserMode::kAuto};
  lv_obj_t* mrow=lv_obj_create(tab); lv_obj_set_size(mrow,440,56); lv_obj_align(mrow,LV_ALIGN_BOTTOM_MID,0,-6);
  lv_obj_set_style_bg_opa(mrow,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(mrow,0,0);
  lv_obj_set_flex_flow(mrow,LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(mrow,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(mrow,LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<4;i++){ lv_obj_t* b=lv_btn_create(mrow); lv_obj_set_size(b,100,48);
    lv_obj_add_event_cb(b,modeEvt,LV_EVENT_CLICKED,(void*)(intptr_t)mv[i]);
    lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,mn[i]); lv_obj_center(l); modeBtns[i]=b; }
  wModeLbl=lv_label_create(tab); lv_obj_add_flag(wModeLbl,LV_OBJ_FLAG_HIDDEN);
}

static void buildSensors(lv_obj_t* tab){
  lv_obj_t* t=lv_label_create(tab); lv_label_set_text(t,"Room sensors");
  lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,4,0);
  wSensorList=lv_label_create(tab); lv_obj_set_style_text_color(wSensorList,lv_color_hex(COL_INK),0);
  lv_obj_set_style_text_font(wSensorList,&lv_font_montserrat_20,0);
  lv_obj_align(wSensorList,LV_ALIGN_TOP_LEFT,4,44);
}
static void buildSystem(lv_obj_t* tab){
  lv_obj_t* t=lv_label_create(tab); lv_label_set_text(t,"System");
  lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,4,0);
  wSysBody=lv_label_create(tab); lv_obj_set_style_text_color(wSysBody,lv_color_hex(COL_MUTED),0);
  lv_obj_align(wSysBody,LV_ALIGN_TOP_LEFT,4,44);
}
static void buildDiag(lv_obj_t* tab){
  lv_obj_t* t=lv_label_create(tab); lv_label_set_text(t,"Diagnostics / CT-485 sniffer");
  lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,4,0);
  wDiagBody=lv_label_create(tab); lv_obj_set_style_text_color(wDiagBody,lv_color_hex(COL_MUTED),0);
  lv_obj_align(wDiagBody,LV_ALIGN_TOP_LEFT,4,44);
  lv_obj_t* n=lv_label_create(tab);
  lv_label_set_text(n,"(demo counters — folds onto the real control-task\nCT-485 stack when the UI is integrated)");
  lv_obj_set_style_text_color(n,lv_color_hex(0x556270),0); lv_obj_align(n,LV_ALIGN_BOTTOM_LEFT,4,-4);
}

// ---- Presets tab ----
static void presetEvt(lv_event_t* e){
  gModel.setPreset((Preset)(intptr_t)lv_event_get_user_data(e));
}
static void buildPresets(lv_obj_t* tab){
  lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t=lv_label_create(tab); lv_label_set_text(t,"Presets");
  lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,4,0);
  const char* nm[3]={"Home","Away","Sleep"};
  const char* sub[3]={"21 / 24","17 / 28","19 / 23"};
  Preset pv[3]={Preset::kHome,Preset::kAway,Preset::kSleep};
  for(int i=0;i<3;i++){
    lv_obj_t* b=lv_btn_create(tab); lv_obj_set_size(b,230,150);
    lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*250,52);
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_CARD),0);
    lv_obj_add_event_cb(b,presetEvt,LV_EVENT_CLICKED,(void*)(intptr_t)pv[i]);
    lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,nm[i]);
    lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_align(l,LV_ALIGN_TOP_MID,0,4);
    lv_obj_t* s=lv_label_create(b); lv_label_set_text_fmt(s,"heat/cool  %s",sub[i]);
    lv_obj_set_style_text_color(s,lv_color_hex(COL_MUTED),0); lv_obj_align(s,LV_ALIGN_BOTTOM_MID,0,-6);
  }
}

// ---- PIN keypad overlay ----
static void kpadRefresh(){ char d[16]=""; for(int i=0;i<4;i++) strcat(d, i<kpN?"* ":"_ "); lv_label_set_text(kpadDots,d); }
static void kpadOpen(KpMode m,const char* title){ kpMode=m; kpN=0; lv_label_set_text(kpadTitle,title); kpadRefresh();
  lv_obj_clear_flag(kpad,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(kpad); }
static void kpadClose(){ lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN); }
static void kpEvt(lv_event_t* e){
  const int v=(int)(intptr_t)lv_event_get_user_data(e);
  if(v==-1){ kpadClose(); return; }
  if(v==-2){ kpN=0; kpadRefresh(); return; }
  if(kpN<4){ kpBuf[kpN++]=(uint8_t)v; kpadRefresh(); }
  if(kpN==4){
    if(kpMode==KpMode::Set){ gModel.setUserPin(kpBuf,kPinSalt); lv_label_set_text(kpadTitle,"PIN saved - close"); }
    else { gModel.beginPinEntry(PinContext::kUnlock,nowSeconds());
      PinResult r=PinResult::kIdle; for(int i=0;i<4;i++) r=gModel.enterPinDigit(kpBuf[i],nowSeconds());
      lv_label_set_text(kpadTitle, r==PinResult::kAccepted?"Unlocked - close":"Wrong PIN"); }
    kpN=0; kpadRefresh();
  }
}
static void buildKeypad(lv_obj_t* scr){
  kpad=lv_obj_create(scr); lv_obj_set_size(kpad,360,420); lv_obj_center(kpad);
  lv_obj_set_style_bg_color(kpad,lv_color_hex(0x141B24),0);
  lv_obj_set_style_border_color(kpad,lv_color_hex(COL_CRYO),0); lv_obj_set_style_border_width(kpad,2,0);
  lv_obj_clear_flag(kpad,LV_OBJ_FLAG_SCROLLABLE);
  kpadTitle=lv_label_create(kpad); lv_obj_align(kpadTitle,LV_ALIGN_TOP_MID,0,4);
  kpadDots=lv_label_create(kpad); lv_obj_set_style_text_font(kpadDots,&lv_font_montserrat_28,0);
  lv_obj_align(kpadDots,LV_ALIGN_TOP_MID,0,36);
  lv_obj_t* grid=lv_obj_create(kpad); lv_obj_set_size(grid,340,300); lv_obj_align(grid,LV_ALIGN_BOTTOM_MID,0,-8);
  lv_obj_set_style_bg_opa(grid,LV_OPA_TRANSP,0); lv_obj_set_style_border_width(grid,0,0);
  lv_obj_set_flex_flow(grid,LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_flex_align(grid,LV_FLEX_ALIGN_SPACE_EVENLY,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(grid,LV_OBJ_FLAG_SCROLLABLE);
  const char* keys[12]={"1","2","3","4","5","6","7","8","9","Esc","0","Clr"};
  const int   vals[12]={1,2,3,4,5,6,7,8,9,-1,0,-2};
  for(int i=0;i<12;i++){ lv_obj_t* b=lv_btn_create(grid); lv_obj_set_size(b,96,64);
    lv_obj_add_event_cb(b,kpEvt,LV_EVENT_CLICKED,(void*)(intptr_t)vals[i]);
    lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,keys[i]); lv_obj_center(l); }
  lv_obj_add_flag(kpad,LV_OBJ_FLAG_HIDDEN);
}

// ---- Settings tab ----
static void setPinEvt(lv_event_t*){ kpadOpen(KpMode::Set,"Set a 4-digit PIN"); }
static void lockEvt(lv_event_t*){ if(gModel.userPinSet()) gModel.lockNow(nowSeconds()); }
static void unlockEvt(lv_event_t*){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
static void buildSettings(lv_obj_t* tab){
  lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* t=lv_label_create(tab); lv_label_set_text(t,"Settings");
  lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,4,0);
  lv_obj_t* tun=lv_label_create(tab);
  lv_label_set_text(tun,"Balance point:   -2.0 C\nCompressor min OAT: -12.0 C\nAux max OAT:      4.0 C\nMin deadband:    2.0 C");
  lv_obj_set_style_text_color(tun,lv_color_hex(COL_MUTED),0); lv_obj_align(tun,LV_ALIGN_TOP_LEFT,4,44);
  wLockState=lv_label_create(tab); lv_obj_align(wLockState,LV_ALIGN_TOP_LEFT,4,180);
  struct B{const char* t; lv_event_cb_t cb;} bs[3]={{"Set PIN",setPinEvt},{"Lock",lockEvt},{"Unlock",unlockEvt}};
  for(int i=0;i<3;i++){ lv_obj_t* b=lv_btn_create(tab); lv_obj_set_size(b,150,60);
    lv_obj_align(b,LV_ALIGN_TOP_LEFT,4+i*170,230);
    lv_obj_add_event_cb(b,bs[i].cb,LV_EVENT_CLICKED,nullptr);
    lv_obj_t* l=lv_label_create(b); lv_label_set_text(l,bs[i].t); lv_obj_center(l); }
}

static void buildUi(){
  lv_obj_t* scr=lv_scr_act(); lv_obj_set_style_bg_color(scr,lv_color_hex(COL_BG),0);
  lv_obj_set_style_text_font(scr,&lv_font_montserrat_20,0);   // larger default; cascades to labels
  lv_obj_set_style_text_color(scr,lv_color_hex(COL_INK),0);   // light default so headers aren't dark-on-dark
  lv_obj_t* tv=lv_tabview_create(scr,LV_DIR_BOTTOM,56);
  lv_obj_set_style_bg_color(tv,lv_color_hex(COL_BG),0);
  buildHome    (lv_tabview_add_tab(tv,"Home"));
  buildPresets (lv_tabview_add_tab(tv,"Presets"));
  buildSensors (lv_tabview_add_tab(tv,"Sensors"));
  buildSystem  (lv_tabview_add_tab(tv,"System"));
  buildSettings(lv_tabview_add_tab(tv,"Settings"));
  buildDiag    (lv_tabview_add_tab(tv,"Diag"));
  buildKeypad(scr);
}

// Update a label only when its text changed — skips the invalidate/redraw and
// the PSRAM framebuffer write that would otherwise starve the LCD DMA.
static void setTxt(lv_obj_t* o,const char* t){
  const char* c=lv_label_get_text(o);
  if(c && strcmp(c,t)==0) return;
  lv_label_set_text(o,t);
}
static void setTxtCol(lv_obj_t* o,const char* t,uint32_t col){
  static struct{lv_obj_t* o;uint32_t c;} seen[16]; static int n=0;
  int idx=-1; for(int i=0;i<n;i++) if(seen[i].o==o){idx=i;break;}
  if(idx<0 && n<16){ idx=n++; seen[idx].o=o; seen[idx].c=~col; }
  bool colChanged = (idx<0) || seen[idx].c!=col;
  if(colChanged){ lv_obj_set_style_text_color(o,lv_color_hex(col),0); if(idx>=0) seen[idx].c=col; }
  setTxt(o,t);
}

static void render(){
  const DisplayState& s=gModel.state();
  char b[128];
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(wTemp,b);
  setTxtCol(wAction,actionName(s.action),actionColor(s.action));
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.heatSetpointC); setTxt(wHeatSp,b);
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.coolSetpointC); setTxt(wCoolSp,b);
  snprintf(b,sizeof(b),"WiFi %s",s.wifiOk?"OK":"--"); setTxtCol(wWifi,b,s.wifiOk?COL_OK:COL_WARN);
  snprintf(b,sizeof(b),"MQTT %s",s.mqttOk?"OK":"--"); setTxtCol(wMqtt,b,s.mqttOk?COL_OK:COL_WARN);
  snprintf(b,sizeof(b),"BUS %s",s.busOk?"OK":"--");   setTxtCol(wBus,b,s.busOk?COL_OK:COL_MUTED);
  if(s.outdoorValid){ snprintf(b,sizeof(b),"OUT %.1f\xC2\xB0",(double)s.outdoorTempC); setTxt(wOat,b);} else setTxt(wOat,"OUT --");
  for(int i=0;i<4;i++){
    static uint32_t lastBg[4]={0,0,0,0};
    bool on = ((i==0)&&s.mode==UserMode::kOff)||((i==1)&&s.mode==UserMode::kHeat)||
              ((i==2)&&s.mode==UserMode::kCool)||((i==3)&&s.mode==UserMode::kAuto);
    uint32_t bg = on?COL_CRYO:0x223040;
    if(lastBg[i]!=bg){ lastBg[i]=bg; lv_obj_set_style_bg_color(modeBtns[i],lv_color_hex(bg),0); }
  }
  if(wSensorList){
    if(s.sensorCount==0) setTxt(wSensorList,"(no remote sensors configured)");
    else { char big[256]=""; for(uint8_t i=0;i<s.sensorCount && i<4;i++){ char row[64];
      snprintf(row,sizeof(row),"%-10s %.1f\xC2\xB0  %s\n",s.sensors[i].name,(double)s.sensors[i].tempC,
        s.sensors[i].participating?"active":"idle"); strncat(big,row,sizeof(big)-strlen(big)-1);} setTxt(wSensorList,big);} }
  if(wSysBody){ snprintf(b,sizeof(b),
    "Outdoor:  %.1f\xC2\xB0  (%s)\nGas mod:  %.0f%%\nCompressor lockout: %lus\nMode:     %s",
    (double)s.outdoorTempC, s.outdoorValid?"valid":"--", (double)s.gasModulationPct,
    (unsigned long)s.compressorLockoutRemainS, modeName(s.mode)); setTxt(wSysBody,b); }
  if(wLockState){ const bool unlocked = gModel.lockState()==LockState::kUnlocked;
    snprintf(b,sizeof(b),"Lock: %s    PIN: %s", unlocked?"unlocked":"LOCKED",
             gModel.userPinSet()?"set":"none");
    setTxtCol(wLockState,b, unlocked?COL_OK:COL_WARN); }
  if(wDiagBody){ snprintf(b,sizeof(b),
    "baud:      9600 (demo)\nframes ok: %lu\nbad crc:   %lu\ntotal:     %lu",
    (unsigned long)gDemo.framesOk,(unsigned long)gDemo.badcrc,(unsigned long)gDemo.frames);
    setTxt(wDiagBody,b); }
}

void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("[ui] SlyTherm wall UI (stage 2)");
  Wire.begin(kSda,kScl); Wire.setClock(400000);
  ch422Mode(0x01); ch422SetOut(kBitTpRst|kBitLcdRst); delay(20);
  gfx.init();
  gfx.setColorDepth(16);
  gfx.fillScreen(0x0000);
  ch422SetOut(kBitTpRst|kBitLcdRst|kBitLcdBl);
  gt911Reset(); Wire.beginTransmission(kGt911Addr); gGt911Ok=(Wire.endTransmission()==0);
  Serial.printf("[ui] GT911 %s\n", gGt911Ok?"present":"NO ACK");

  lv_init();
  lv_disp_draw_buf_init(&draw_buf,buf1,nullptr,kHor*40);
  lv_disp_drv_init(&disp_drv); disp_drv.hor_res=kHor; disp_drv.ver_res=kVer;
  disp_drv.flush_cb=flushCb; disp_drv.draw_buf=&draw_buf; lv_disp_drv_register(&disp_drv);
  lv_indev_drv_init(&indev_drv); indev_drv.type=LV_INDEV_TYPE_POINTER; indev_drv.read_cb=touchCb;
  lv_indev_drv_register(&indev_drv);

  gModel.setSetpoints(20.0f,24.0f);
  buildUi();
  Serial.println("[ui] LVGL UI up");
}

void loop(){
  static uint32_t last=0, lastStep=0, lastRender=0;
  uint32_t now=millis();
  lv_tick_inc(now-last); last=now;
  if(now-lastStep>=1000){ lastStep=now; gDemo.step(now/1000); }
  if(now-lastRender>=500){ lastRender=now; render(); }   // 2 Hz, update-on-change
  lv_timer_handler();
  delay(5);
}
