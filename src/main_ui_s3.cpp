// main_ui_s3.cpp — SlyTherm wall UI (issue #37), standalone LVGL app on the
// Waveshare ESP32-S3-4.3B. Builds the real multi-screen UI bound to
// lib/UiModel, driven here by a DEMO control-echo (drains UiModel intents and
// reflects them) so every screen is interactive and verifiable BEFORE wiring
// into the threaded control task in main_thermostat.cpp.
//
// Display/touch glue is the stage-1 validated recipe (see main_ui_smoke.cpp /
// memory: LVGL v8, 800x40 internal-RAM partial buffer, GT911 raw I2C @0x5D).

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <lvgl.h>

#include "UiModel.h"

using namespace dettson;
using namespace dettson::ui;

// ================= panel + CH422G + GT911 (validated stage 1) ================
static constexpr uint8_t kCh422ModeAddr = 0x24, kCh422OutAddr = 0x38;
static constexpr uint8_t kBitTpRst = 1 << 1, kBitLcdBl = 1 << 2, kBitLcdRst = 1 << 3;
static uint8_t gCh422Out = 0;
static void ch422Mode(uint8_t v){ Wire.beginTransmission(kCh422ModeAddr); Wire.write(v); Wire.endTransmission(); }
static void ch422SetOut(uint8_t v){ gCh422Out=v; Wire.beginTransmission(kCh422OutAddr); Wire.write(v); Wire.endTransmission(); }

static Arduino_ESP32RGBPanel* panel = nullptr;
static Arduino_RGB_Display*   gfx   = nullptr;

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
  uint8_t s=0; if(gt911ReadReg(0x814E,&s,1)!=1) return false;
  bool hit=false;
  if((s&0x80)&&(s&0x0F)>0){ uint8_t d[6]={0};
    if(gt911ReadReg(0x8150,d,6)>=4){ x=d[0]|(d[1]<<8); y=d[2]|(d[3]<<8); hit=true; } }
  if(s&0x80){ Wire.beginTransmission(kGt911Addr); Wire.write(0x81); Wire.write(0x4E); Wire.write(0); Wire.endTransmission(); }
  return hit;
}

static constexpr uint16_t kHor=800, kVer=480;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf1[kHor*40];
static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

static void flushCb(lv_disp_drv_t* d,const lv_area_t* a,lv_color_t* px){
  gfx->draw16bitRGBBitmap(a->x1,a->y1,(uint16_t*)&px->full,a->x2-a->x1+1,a->y2-a->y1+1);
  lv_disp_flush_ready(d);
}
static void touchCb(lv_indev_drv_t*,lv_indev_data_t* data){
  uint16_t x,y;
  if(gGt911Ok && gt911Read(x,y)){ data->state=LV_INDEV_STATE_PR; data->point.x=x; data->point.y=y; }
  else data->state=LV_INDEV_STATE_REL;
}

// ================= UI palette / theme ========================================
#define COL_BG      0x0A0E13
#define COL_CARD    0x141B24
#define COL_INK     0xE6EDF3
#define COL_MUTED   0x8A97A3
#define COL_EMBER   0xFF7A18
#define COL_CRYO    0x38BDF8
#define COL_OK      0x36D399
#define COL_WARN    0xF87272

// ================= model + demo control echo =================================
static UiModel gModel;

// Demo "control task": simulates a room that drifts toward the active setpoint
// and drains UiModel intents so touch changes are reflected.
struct DemoCtl {
  float roomC = 21.3f;
  UserMode mode = UserMode::kOff;
  float heatC = 20.0f, coolC = 24.0f;
  void step(uint32_t nowS) {
    UiIntent it;
    while (gModel.popIntent(it)) {
      switch (it.type) {
        case IntentType::kSetSetpoints: heatC = it.heatC; coolC = it.coolC; break;
        case IntentType::kSetMode:      mode = it.mode; break;
        default: break;
      }
    }
    // drift + action
    HvacAction act = HvacAction::kIdle; uint8_t equip = kEquipNone; float mod = 0;
    const bool heatCall = (mode==UserMode::kHeat||mode==UserMode::kAuto||mode==UserMode::kEmergencyHeat) && roomC < heatC-0.2f;
    const bool coolCall = (mode==UserMode::kCool||mode==UserMode::kAuto) && roomC > coolC+0.2f;
    if (heatCall){ roomC += 0.05f; act=HvacAction::kHeating; equip=kEquipGas|kEquipFan; mod=60; }
    else if (coolCall){ roomC -= 0.05f; act=HvacAction::kCooling; equip=kEquipHpCool|kEquipFan; }
    else roomC += (roomC>21.5f?-0.01f:0.01f);
    gModel.setFusedTemp(roomC, true);
    gModel.setUserMode(mode);
    gModel.setSetpoints(heatC, coolC);
    gModel.setHvacAction(act);
    gModel.setActiveEquipment(equip);
    gModel.setGasModulationPct(mod);
    gModel.setOutdoor(-4.0f, true, OutdoorSource::kHaWeather);
    gModel.setLinkHealth(true, true, false);
  }
} gDemo;

// ================= screen widgets (updated in render) ========================
static lv_obj_t *wTemp, *wAction, *wHeatSp, *wCoolSp, *wModeLbl;
static lv_obj_t *wWifi, *wMqtt, *wBus, *wOat;
static lv_obj_t *wSensorList, *wSysBody;
static lv_obj_t *modeBtns[4];

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
  wSensorList=lv_label_create(tab); lv_obj_set_style_text_color(wSensorList,lv_color_hex(COL_MUTED),0);
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
  lv_obj_t* n=lv_label_create(tab);
  lv_label_set_text(n,"Live bus counters fold in here once the UI is\nwired to the control task's CT-485 stack.");
  lv_obj_set_style_text_color(n,lv_color_hex(COL_MUTED),0); lv_obj_align(n,LV_ALIGN_TOP_LEFT,4,44);
}

static void buildUi(){
  lv_obj_t* scr=lv_scr_act(); lv_obj_set_style_bg_color(scr,lv_color_hex(COL_BG),0);
  lv_obj_t* tv=lv_tabview_create(scr,LV_DIR_BOTTOM,56);
  lv_obj_set_style_bg_color(tv,lv_color_hex(COL_BG),0);
  buildHome  (lv_tabview_add_tab(tv,"Home"));
  buildSensors(lv_tabview_add_tab(tv,"Sensors"));
  buildSystem (lv_tabview_add_tab(tv,"System"));
  buildDiag   (lv_tabview_add_tab(tv,"Diag"));
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
}

void setup(){
  Serial.begin(115200); delay(200);
  Serial.println("[ui] SlyTherm wall UI (stage 2)");
  Wire.begin(kSda,kScl); Wire.setClock(400000);
  ch422Mode(0x01); ch422SetOut(kBitTpRst|kBitLcdRst); delay(20);
  // prefer_speed MUST stay 16 MHz — the panel shows a solid-white (no-signal)
  // screen at 12 MHz. Shake is mitigated by update-on-change rendering instead
  // (minimal PSRAM writes); if that's not enough the fix is LovyanGFX (bounce buffer).
  panel=new Arduino_ESP32RGBPanel(5,3,46,7,1,2,42,41,40,39,0,45,48,47,21,14,38,18,17,10,
    0,40,48,88,0,13,3,32,1,16000000);
  gfx=new Arduino_RGB_Display(kHor,kVer,panel,0,true);
  if(!gfx->begin()) Serial.println("[ui] gfx begin FAILED");
  gfx->fillScreen(BLACK);
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
