// ui_main.cpp — the tabbed main screen (#114): top bar, pull-down nav, the
// six tabs (Home/Presets/Sensors/System/Settings/Diag) with their event
// handlers, renderMain, and the 12 h trend graph. Moved verbatim from the
// pre-split slytherm_ui.cpp.

#include <Arduino.h>
#include <cmath>
#include <cstring>

#include "ui_shared.h"
#include "mqtt_cfg.h"
#include "wifi_prov.h"   // Settings reorg: Networking card summary (ssid/ip/rssi)
#ifdef SLYTHERM_CAM
#include "remote_camera.h"  // #150: top-bar dot while a camera client is served
#endif

// #113: injected by tools/version_flag.py; fallback keeps ad-hoc builds compiling.
#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

namespace slytherm_ui {

// main-screen widgets
lv_obj_t *wTemp,*wDeg=nullptr,*wAction,*wHeatSp,*wCoolSp,*wWifi,*wMqtt,*wBus,*wOat,*wClock,*wSysBody,*wDiagBody;
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
lv_obj_t *wFollow,*gHeatCard,*gCoolCard,*wOnline,*gPresetBtns[kMaxPresets]={};  // UI v2 Home/Presets
lv_obj_t *wRssiBox=nullptr,*wRssiBar[4]={};  // #127 top-bar RSSI indicator
lv_obj_t *gPresetName[kMaxPresets]={},*gPresetVal[kMaxPresets]={};  // #74: live-roster card labels
lv_obj_t *gHoldBtn=nullptr,*gHoldLbl=nullptr;   // Home hold pill (#81): shows active hold, opens the chooser
lv_obj_t *gVacHome=nullptr;                      // Home vacation banner (#78): "Vacation until <date>"
lv_obj_t *gPresetHold=nullptr;                   // Presets page: centered hold status ("On hold - Xh Ym left")
// Settings information-architecture reorg: one live one-line summary per
// category card (updated each renderMain, like the old WiFi/Home status words).
lv_obj_t *wCatNet=nullptr,*wCatFan=nullptr,*wCatDisp=nullptr,*wCatSec=nullptr,*wCatSys=nullptr,*wCatMode=nullptr;
lv_obj_t *gModeSheet=nullptr;   // System Mode sub-sheet (Off/Heat/Cool/Auto — moved off Home into Settings)
#ifdef SLYTHERM_CAM
lv_obj_t *wCamDot=nullptr;   // #150: red top-bar dot while a camera client is being served
#endif
// System 12 h trend graph (#76): ~5-min RAM ring, 144 pts (temps x10 as lv_coord_t).
constexpr int kGraphPts=144;
lv_obj_t *gSysChart=nullptr,*wSysGraphLbl=nullptr;
lv_chart_series_t *gSerActual=nullptr,*gSerHeat=nullptr,*gSerCool=nullptr;
lv_coord_t gRingA[kGraphPts],gRingH[kGraphPts],gRingC[kGraphPts];  // #156: room(avg) / setpoint / outside
// #156: central trend series from the SlyLog graph-publisher. The MQTT task
// parses the retained slytherm/graph/system into gGraphIn (plain ints, never
// touches LVGL); the UI task copies it into the rings + refreshes. gGraphCentral
// gates the legacy on-device sampler once real data has arrived.
lv_coord_t gGraphIn[3][kGraphPts];   // 0=room 1=setpoint 2=outside
volatile bool gGraphDirty=false; bool gGraphCentral=false;
lv_obj_t *gNavMenu=nullptr,*wCaret=nullptr;  // pull-down navigation
lv_obj_t *modeBtns[4];
lv_obj_t *gBtnListen=nullptr;   // Diag LISTEN button — render-gated on DisplayState.hasBus (#101)
// ---- event handlers (touch runs on the UI task; take the mutex around model) ----
int gHoldReps=0;
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

lv_obj_t* spBtn(lv_obj_t*p,const char*t,int code,lv_align_t al,int xo=0,int yo=0){ lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,64,64);
  lv_obj_align(b,al,xo,yo); lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),0); void*u=(void*)(intptr_t)code;
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_PRESSED,u); lv_obj_add_event_cb(b,spEvt,LV_EVENT_SHORT_CLICKED,u);
  lv_obj_add_event_cb(b,spEvt,LV_EVENT_LONG_PRESSED_REPEAT,u);
  const char*sym=(t[0]=='-'&&!t[1])?LV_SYMBOL_MINUS:((t[0]=='+'&&!t[1])?LV_SYMBOL_PLUS:t);  // real +/- glyphs, not off-center ASCII
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,sym); lv_obj_set_style_text_font(l,&lv_font_montserrat_28,0); lv_obj_center(l); return b; }

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
  // No on-Home "System off" hint: the action line under the temperature already
  // reads "System off" when off, so the redundant (and tofu-prone "...Settings >
  // System Mode") right-side label was removed.
  // Mode selector (Off/Heat/Cool/Auto) was here; moved off Home into
  // Settings > System Mode (set-once, not a daily control). modeBtns[] are now
  // created in buildModeSheet(); Home's bottom strip stays clean.
}

// #90/preset-highlight: title-case a stored preset id for DISPLAY only ("home"->"Home",
// "night sleep"->"Night Sleep"). The stored lowercase name stays the HA preset_mode
// identifier and roster key — never mutated here.
static void presetLabel(const char* src, char* dst, size_t n){
  size_t j=0; bool up=true;
  for(size_t i=0; src && src[i] && j+1<n; ++i){ char c=src[i];
    if(c==' '||c=='-'||c=='_'){ up=true; dst[j++]=c; continue; }
    dst[j++] = (up && c>='a' && c<='z') ? (char)(c-32) : c; up=false; }
  dst[j]=0;
}

// #74: build up to kMaxPresets cards once (3-wide grid); renderMain fills the
// name/values from the LIVE roster (DisplayState.presets) and shows/hides by
// presetCount. Card index == roster index == the value passed to presetEvt.
// TODO(#74 deferred): on-device EDIT of a preset (long-press -> stepper ->
// stage retained slytherm/config/presets via mqttTask). Displaying the live
// roster is done; editing is PARTIAL.
void buildPresets(lv_obj_t*tab){ lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLLABLE); header(tab,"Presets");
  // Centered hold indicator above the preset cards: shows the active hold + time remaining
  // (or hidden when no hold). Sits at top-mid; the "Presets" header is left-aligned so no clash.
  gPresetHold=lv_label_create(tab); lv_obj_set_style_text_font(gPresetHold,&lv_font_montserrat_20,0);
  lv_obj_set_style_text_color(gPresetHold,lv_color_hex(COL_WARN),0); lv_label_set_text(gPresetHold,"");
  lv_obj_align(gPresetHold,LV_ALIGN_TOP_MID,0,6); lv_obj_add_flag(gPresetHold,LV_OBJ_FLAG_HIDDEN);
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
  wSysGraphLbl=lv_label_create(tab); lv_label_set_recolor(wSysGraphLbl,true); lv_obj_set_style_text_font(wSysGraphLbl,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(wSysGraphLbl,lv_color_hex(COL_MUTED),0); lv_obj_align(wSysGraphLbl,LV_ALIGN_TOP_RIGHT,-8,322); lv_label_set_text(wSysGraphLbl,""); }
void buildDiag(lv_obj_t*tab){ header(tab,"Diagnostics");
  wDiagBody=lv_label_create(tab); lv_obj_set_style_text_color(wDiagBody,lv_color_hex(COL_MUTED),0); lv_obj_align(wDiagBody,LV_ALIGN_TOP_LEFT,4,48); lv_label_set_text(wDiagBody,"");
  // #101: built unconditionally, render-gated on DisplayState.hasBus so the
  // busless Remote persona never shows a dead LISTEN button.
  gBtnListen=mkBtn(tab,LV_SYMBOL_EYE_OPEN "  LISTEN on RS-485",openSniff,LV_ALIGN_BOTTOM_LEFT,4,-8,COL_CRYO,300); }

// System Mode sub-sheet (Settings): the Off/Heat/Cool/Auto selector, relocated
// off Home (set-once, not a daily control). Same segmented-control grammar as
// the old Home bottom bar; modeBtns[] live here now and renderMain still fills
// the active segment. modeEvt (mode intent -> gM->setMode) is unchanged.
void openMode(lv_event_t*){ if(!gModeSheet) return; lv_obj_clear_flag(gModeSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gModeSheet); }
void buildModeSheet(lv_obj_t*scr){ gModeSheet=sheetShell(scr,480,270,"System Mode","How the system runs");
  const char*mn[4]={"OFF","HEAT","COOL","AUTO"}; UserMode mv[4]={UserMode::kOff,UserMode::kHeat,UserMode::kCool,UserMode::kAuto};
  lv_obj_t*mrow=lv_obj_create(gModeSheet); lv_obj_set_size(mrow,440,64); lv_obj_align(mrow,LV_ALIGN_TOP_MID,0,110);
  lv_obj_set_style_bg_color(mrow,lv_color_hex(COL_BG),0); lv_obj_set_style_bg_opa(mrow,LV_OPA_COVER,0); lv_obj_set_style_border_width(mrow,0,0);
  lv_obj_set_style_radius(mrow,12,0); lv_obj_set_style_pad_all(mrow,4,0);  // segmented control track
  lv_obj_set_flex_flow(mrow,LV_FLEX_FLOW_ROW); lv_obj_set_flex_align(mrow,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(mrow,LV_OBJ_FLAG_SCROLLABLE);
  for(int i=0;i<4;i++){ lv_obj_t*b=lv_btn_create(mrow); lv_obj_set_size(b,102,52);
    lv_obj_set_style_bg_opa(b,LV_OPA_TRANSP,0); lv_obj_set_style_shadow_width(b,0,0); lv_obj_set_style_radius(b,9,0);  // flat segment; renderMain fills the active one
    lv_obj_add_event_cb(b,modeEvt,LV_EVENT_CLICKED,(void*)(intptr_t)mv[i]);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,mn[i]); lv_obj_set_style_text_color(l,lv_color_hex(COL_MUTED),0); lv_obj_center(l); modeBtns[i]=b; }
  lv_obj_t*hint=lv_label_create(gModeSheet); lv_obj_set_style_text_font(hint,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(hint,lv_color_hex(COL_TEXT3),0); lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
  lv_obj_set_width(hint,440); lv_obj_align(hint,LV_ALIGN_TOP_MID,0,196);
  lv_label_set_text(hint,"Set once - not a daily control.\nAdjust temperatures on the Home screen."); }

// Settings information-architecture reorg (#128/settings): the flat button list
// became a short list of CATEGORY CARDS. Each card shows a live one-line summary
// (filled in renderMain) and drills into a grouping sub-sheet (ui_overlays.cpp).
// The event handlers for the controls those sheets contain (clock toggle, PIN/
// lock, VPN retry) moved into ui_overlays.cpp alongside the sheets they live on.
// Card tap -> open the matching sub-sheet. Index dispatch (the codebase idiom)
// avoids a function-pointer-through-void* cast; keep in sync with cats[] below.
void catCardEvt(lv_event_t*e){ switch((int)(intptr_t)lv_event_get_user_data(e)){
  case 0: openMode(e); break;     case 1: openNet(e); break;      case 2: openFan(e); break;
  case 3: openDisplay(e); break;  case 4: openSecurity(e); break; case 5: openSystem(e); break; } }
void buildSettings(lv_obj_t*tab){
  // The 6 category cards are taller than the tab viewport, so allow VERTICAL
  // scroll (the last card, System, was clipped). Momentum + elastic stay OFF —
  // the throw animation re-entering the RGB flush is the tabview-scroll panic
  // (coredump-debugged). scroll_dir VER so it never fights the tabview's
  // horizontal tab-swipe.
  lv_obj_add_flag(tab,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scroll_dir(tab,LV_DIR_VER);
  lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLL_MOMENTUM);
  lv_obj_clear_flag(tab,LV_OBJ_FLAG_SCROLL_ELASTIC);
  lv_obj_set_scrollbar_mode(tab,LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_pad_bottom(tab,18,0);  // breathing room so the last card scrolls fully clear
  header(tab,"Settings");
  // One tap-through card per category: title + live summary + chevron. Sensors
  // deliberately stays its own top tab; Fan reuses the #128 sheet. The whole
  // card is the tap target; index (user-data) selects the sub-sheet.
  struct Cat{const char*title; lv_obj_t**sum;};
  Cat cats[6]={{"System Mode",&wCatMode},{"Networking",&wCatNet},{"Fan",&wCatFan},
               {"Display",&wCatDisp},{"Security",&wCatSec},{"System",&wCatSys}};
  for(int i=0;i<6;i++){ lv_obj_t*c=lv_btn_create(tab); lv_obj_set_size(c,748,66); lv_obj_align(c,LV_ALIGN_TOP_LEFT,6,46+i*74);
    lv_obj_set_style_bg_color(c,lv_color_hex(COL_CARD),0); lv_obj_set_style_bg_color(c,lv_color_hex(COL_RAISED),LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(c,0,0); lv_obj_set_style_radius(c,10,0); lv_obj_set_style_pad_all(c,0,0);
    lv_obj_add_event_cb(c,catCardEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i);
    lv_obj_t*t=lv_label_create(c); lv_label_set_text(t,cats[i].title); lv_obj_set_style_text_font(t,&lv_font_montserrat_24,0);
    lv_obj_set_style_text_color(t,lv_color_hex(COL_INK),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,16,10);
    lv_obj_t*sm=lv_label_create(c); lv_obj_set_style_text_font(sm,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(sm,lv_color_hex(COL_MUTED),0);
    lv_obj_set_width(sm,600); lv_label_set_long_mode(sm,LV_LABEL_LONG_DOT); lv_obj_align(sm,LV_ALIGN_TOP_LEFT,16,40); lv_label_set_text(sm,"");
    *cats[i].sum=sm;
    lv_obj_t*ch=lv_label_create(c); lv_label_set_text(ch,LV_SYMBOL_RIGHT); lv_obj_set_style_text_color(ch,lv_color_hex(COL_TEXT3),0); lv_obj_align(ch,LV_ALIGN_RIGHT_MID,-16,0); } }

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
#ifdef SLYTHERM_CAM
  // #150: camera-serving indicator, same 8px-dot pattern as wOnline; hidden
  // unless remote_camera::clientActive() (toggled in renderMain).
  wCamDot=lv_obj_create(bar); lv_obj_set_size(wCamDot,8,8); lv_obj_set_style_radius(wCamDot,LV_RADIUS_CIRCLE,0); lv_obj_set_style_border_width(wCamDot,0,0);
  lv_obj_set_style_bg_color(wCamDot,lv_color_hex(0xE05555),0); lv_obj_align(wCamDot,LV_ALIGN_RIGHT_MID,-284,0); lv_obj_add_flag(wCamDot,LV_OBJ_FLAG_HIDDEN);
#endif
  wOat=lv_label_create(bar); lv_obj_set_style_text_color(wOat,lv_color_hex(COL_MUTED),0); lv_obj_align(wOat,LV_ALIGN_RIGHT_MID,-202,0);
  // #127: WiFi RSSI bars between Outside and the clock; tap opens WiFi status.
  // Cluster pinned 24px further left than the original -140/-178 so the widest
  // 12h clock ("Wed  02:30 PM", ~133px) clears the RSSI box instead of colliding.
  wRssiBox=lv_btn_create(bar); lv_obj_set_size(wRssiBox,28,24); lv_obj_align(wRssiBox,LV_ALIGN_RIGHT_MID,-164,0);
  lv_obj_set_style_bg_opa(wRssiBox,LV_OPA_TRANSP,0); lv_obj_set_style_shadow_width(wRssiBox,0,0);
  lv_obj_set_style_pad_all(wRssiBox,0,0); lv_obj_add_event_cb(wRssiBox,openWifi,LV_EVENT_CLICKED,nullptr);
  for(int i=0;i<4;i++){ const int h=4+3*i;  // 4/7/10/13px ascending
    wRssiBar[i]=lv_obj_create(wRssiBox); lv_obj_set_size(wRssiBar[i],3,h);
    lv_obj_set_style_radius(wRssiBar[i],1,0); lv_obj_set_style_border_width(wRssiBar[i],0,0);
    lv_obj_set_style_bg_color(wRssiBar[i],lv_color_hex(COL_RAISED),0);
    lv_obj_clear_flag(wRssiBar[i],LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(wRssiBar[i],LV_ALIGN_BOTTOM_LEFT,3+5*i,-4); }
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
  // CRASH FIX (coredump: scroll_throw_predict_y -> lv_anim -> Panel_RGB::writeData): the
  // tabview swipe's momentum/throw animation re-enters during the RGB panel flush and
  // panics under fast/repeated swiping. Disable scroll momentum on the tab content —
  // swipe-to-switch still snaps, just no fling-throw. Also drop elastic overscroll.
  { lv_obj_t* tvc=lv_tabview_get_content(tv);
    lv_obj_clear_flag(tvc,LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(tvc,LV_OBJ_FLAG_SCROLL_ELASTIC); }
  buildNavMenu(scrMain);
  buildKeypad(scrMain); buildWifi(scrMain); buildServer(scrMain); buildHoldSheet(scrMain); buildVacationSheet(scrMain); buildFanSheet(scrMain);
  buildModeSheet(scrMain);   // System Mode sheet (Off/Heat/Cool/Auto) — must run before first renderMain so modeBtns[] exist
  buildNetSheet(scrMain); buildDisplaySheet(scrMain); buildSecuritySheet(scrMain); buildSystemSheet(scrMain);   // Settings reorg drill-in sheets
  buildAmbient(); buildWelcome(); buildBoot(); buildSniff(); lv_scr_load(scrMain); }

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
    if(s.mode==UserMode::kOff && (heat||cool)){ strcpy(b,"Off \xE2\x80\xA2 finishing cycle"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }  // #173: mode off but compressor still serving its anti-short-cycle min-ON
    else if(heat){ snprintf(b,sizeof(b),"Heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_EMBER),0); }
    else if(cool){ snprintf(b,sizeof(b),"Cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_CRYO),0); }
    else if(s.compressorHeldOff){ const bool cs=s.compressorHeldSide==SetpointSide::kCool;   // min-OFF ack: wants to run, waiting out compressor protection
      snprintf(b,sizeof(b),"%s soon \xE2\x80\xA2 %lu min",cs?"Cooling":"Heating",(unsigned long)((s.compressorHeldRemainS+59u)/60u)); lv_obj_set_style_text_color(wAction,lv_color_hex(cs?COL_CRYO:COL_EMBER),0); }
    else if(!s.fusedTempValid){ strcpy(b,"Connecting to controller..."); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }   // no controller link yet -> never falsely read "off"
    else if(s.mode==UserMode::kOff){ strcpy(b,"System off"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }
    else if(s.mode==UserMode::kAuto){ snprintf(b,sizeof(b),"Idle - holding %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_MUTED),0); }
    // #Deadband: room past setpoint but below the hysteresis ON threshold -> holding for the next call, not idle
    else if(s.mode==UserMode::kCool && s.fusedTempC>s.coolSetpointC){ strcpy(b,"Waiting to cool"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_CRYO),0); }
    else if((s.mode==UserMode::kHeat||s.mode==UserMode::kEmergencyHeat) && s.fusedTempC<s.heatSetpointC){ strcpy(b,"Waiting to heat"); lv_obj_set_style_text_color(wAction,lv_color_hex(COL_EMBER),0); }
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
#ifdef SLYTHERM_CAM
  if(wCamDot){ if(wOnline) lv_obj_align_to(wCamDot,wOnline,LV_ALIGN_OUT_LEFT_MID,-8,0);  // ride left of the online dot (fixed -260 overlapped the OAT text)
    if(remote_camera::clientActive()) lv_obj_clear_flag(wCamDot,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(wCamDot,LV_OBJ_FLAG_HIDDEN); }  // #150
#endif
  // #127 RSSI bars. Thresholds from real bench data on this hardware (P4
  // mesh: association fails ~-75, solid at -59) — keep as named constants.
  { constexpr int8_t kRssi4=-55, kRssi3=-65, kRssi2=-75;
    int lvl = !s.wifiOk||s.wifiRssi==0 ? 0
              : s.wifiRssi>=kRssi4 ? 4 : s.wifiRssi>=kRssi3 ? 3
              : s.wifiRssi>=kRssi2 ? 2 : 1;
    static int lastLvl=-1;
    if(lvl!=lastLvl && wRssiBox){ lastLvl=lvl;
      for(int i=0;i<4;i++)
        lv_obj_set_style_bg_color(wRssiBar[i], lv_color_hex(
            i<lvl ? (lvl==1?COL_WARN:COL_OK) : COL_RAISED), 0); } }
  if(wClock) setTxt(wClock, s.clockStr[0]?s.clockStr:"");
  { const bool sh=s.mode==UserMode::kHeat||s.mode==UserMode::kAuto, sc=s.mode==UserMode::kCool||s.mode==UserMode::kAuto, au=s.mode==UserMode::kAuto;
    if(sh){ layoutCard(gHeatCard,wHeatSp,!au,COL_EMBER); lv_obj_align(gHeatCard,LV_ALIGN_TOP_RIGHT,-16,au?6:84); lv_obj_clear_flag(gHeatCard,LV_OBJ_FLAG_HIDDEN);} else lv_obj_add_flag(gHeatCard,LV_OBJ_FLAG_HIDDEN);
    if(sc){ layoutCard(gCoolCard,wCoolSp,!au,COL_CRYO); lv_obj_align(gCoolCard,LV_ALIGN_TOP_RIGHT,-16,au?182:84); lv_obj_clear_flag(gCoolCard,LV_OBJ_FLAG_HIDDEN);} else lv_obj_add_flag(gCoolCard,LV_OBJ_FLAG_HIDDEN);
    uint32_t tint=(sc?0x0D1720u:(s.mode==UserMode::kHeat?0x140D0Au:(uint32_t)COL_BG));  // #fix5 SOLID mode tint (no gradient — LVGL sw-gradient cache hangs the render task in our trimmed config)
    static uint32_t lastTint=0xFFFFFFFEu;  // only restyle on mode change
    if(gHomeTab && tint!=lastTint){ lastTint=tint;
      lv_obj_set_style_bg_opa(gHomeTab,LV_OPA_COVER,0); lv_obj_set_style_bg_color(gHomeTab,lv_color_hex(tint),0);
      lv_obj_set_style_bg_grad_dir(gHomeTab,LV_GRAD_DIR_NONE,0); } }
  for(int i=0;i<4;i++){ if(!modeBtns[i]) continue;   // now built in buildModeSheet (Settings); guard first render
    bool on=((i==0)&&s.mode==UserMode::kOff)||((i==1)&&s.mode==UserMode::kHeat)||((i==2)&&s.mode==UserMode::kCool)||((i==3)&&s.mode==UserMode::kAuto);
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
      char plbl[kUiPresetNameLen+4]; presetLabel(s.presets[i].name,plbl,sizeof(plbl));   // #90: Title-Case for display only
      setTxt(gPresetName[i], plbl);
      char pvv[40]; snprintf(pvv,sizeof(pvv),"heat %.0f\xC2\xB0   cool %.0f\xC2\xB0",(double)s.presets[i].heatC,(double)s.presets[i].coolC); setTxt(gPresetVal[i],pvv);
      // #90/preset-highlight: match the AUTHORITATIVE active preset by name (empty once a
      // manual change clears it) — not by setpoint proximity, which lit the wrong card and
      // dropped out when a hold/schedule nudged the setpoints off the preset's exact values.
      bool match = s.activePreset[0] && strcmp(s.activePreset, s.presets[i].name)==0;
      if(match && gPresetSel==i) gPresetSel=-1;   // echo landed -> drop the optimistic latch, keep the match
      bool act=match || (optHeld && gPresetSel==i);
      lv_obj_set_style_border_color(gPresetBtns[i],lv_color_hex(act?COL_OK:COL_BORDER),0); lv_obj_set_style_border_width(gPresetBtns[i],act?2:1,0);
      lv_obj_clear_flag(gPresetBtns[i],LV_OBJ_FLAG_HIDDEN); } }
  // Presets page: centered hold indicator above the cards — active hold + time remaining,
  // hidden when no hold. (Tapping a preset overrides the hold, so this clears on selection.)
  if(gPresetHold){ char hb[48]; const bool held=s.holdType!=HoldType::kNone && s.mode!=UserMode::kOff;
    switch(s.holdType){
      case HoldType::kTwoHours: case HoldType::kFourHours:
        snprintf(hb,sizeof(hb),LV_SYMBOL_WARNING "  On hold - %luh %02lum left",(unsigned long)(s.holdRemainS/3600u),(unsigned long)((s.holdRemainS%3600u)/60u)); break;
      case HoldType::kUntilNextPreset: strcpy(hb,LV_SYMBOL_WARNING "  On hold until next schedule"); break;
      case HoldType::kIndefinite: strcpy(hb,LV_SYMBOL_WARNING "  On hold until you change it"); break;
      default: hb[0]=0; break; }
    if(held){ setTxt(gPresetHold,hb); lv_obj_align(gPresetHold,LV_ALIGN_TOP_MID,0,6); lv_obj_clear_flag(gPresetHold,LV_OBJ_FLAG_HIDDEN); }
    else lv_obj_add_flag(gPresetHold,LV_OBJ_FLAG_HIDDEN); }
  for(int i=0;i<7;i++){ SensorRowUi&ro=gSensorRows[i]; if(!ro.row) continue;
    if(i<(int)s.sensorCount){ const SensorRow&r=s.sensors[i];
      strlcpy(gRowName[i],r.name,sizeof(gRowName[i]));
      setTxt(ro.name, r.name);
      char tb[16]; snprintf(tb,sizeof(tb),"%.1f\xC2\xB0",(double)r.tempC); setTxt(ro.temp,tb);
      // #89 single status word: stale > Following (drives demand) > In use (occupied) > Away[ Nh]
      char st[24];
      if(r.emergency) strcpy(st, r.emergencyActive?"Active":"Standby");   // #163 controller-attached
      else if(!r.healthy) strcpy(st,"stale");
      else if(r.dominant) strcpy(st,"Following");
      else if(r.occupied) strcpy(st,"In use");
      else if(r.lastOccAgeS==0xFFFFFFFFu) strcpy(st,"Away");
      else if(r.lastOccAgeS<3600u) strcpy(st,"Away <1h");
      else snprintf(st,sizeof(st),"Away %luh",(unsigned long)(r.lastOccAgeS/3600u));
      setTxt(ro.pres,st);
      lv_obj_set_style_text_color(ro.pres,lv_color_hex(!r.healthy&&!r.emergency?COL_WARN:COL_MUTED),0);
      // #163 emergency sensor: a fixed, non-selectable "EMERG" pill (green when it
      // is actively driving demand, gray on standby) instead of the On/Off toggle.
      // Clearing CLICKABLE alone makes it non-selectable (no click event fires, so
      // sensorToggleEvt never runs); avoid LV_STATE_DISABLED so the theme doesn't
      // dim our explicit green.
      if(r.emergency){
        setTxt(ro.btnlbl,"EMERG"); lv_obj_clear_flag(ro.btn,LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(ro.btn,lv_color_hex(r.emergencyActive?COL_OK:COL_RAISED),0);
      } else {
        lv_obj_add_flag(ro.btn,LV_OBJ_FLAG_CLICKABLE);
        setTxt(ro.btnlbl, r.participating?"On":"Off"); lv_obj_set_style_bg_color(ro.btn,lv_color_hex(r.participating?COL_OK:COL_RAISED),0);
      }
      lv_obj_clear_flag(ro.row,LV_OBJ_FLAG_HIDDEN); }
    else { gRowName[i][0]=0; lv_obj_add_flag(ro.row,LV_OBJ_FLAG_HIDDEN); } }
  if(wSysBody){ char sb[300];
    // #101: the "Bus:" line only exists on the bus-wired persona (hasBus).
    if(s.hasBus)
      snprintf(sb,sizeof(sb),"Now running:   %s\nReading:       %.1f\xC2\xB0 from %d rooms\nOutdoor:       %s%.0f\xC2\xB0\nCompressor:    %s\nGas modulation: %.0f%%\nBus:           %s\nMode:          %s",
        actName(s.action),(double)s.fusedTempC,(int)s.sensorCount, s.outdoorValid?"":"-- ",(double)s.outdoorTempC,
        s.compressorLockoutRemainS>0?"locked out":"ready",(double)s.gasModulationPct, s.busOk?"connected (listen-only)":"--", modeName(s.mode));
    else
      snprintf(sb,sizeof(sb),"Now running:   %s\nReading:       %.1f\xC2\xB0 from %d rooms\nOutdoor:       %s%.0f\xC2\xB0\nCompressor:    %s\nGas modulation: %.0f%%\nMode:          %s",
        actName(s.action),(double)s.fusedTempC,(int)s.sensorCount, s.outdoorValid?"":"-- ",(double)s.outdoorTempC,
        s.compressorLockoutRemainS>0?"locked out":"ready",(double)s.gasModulationPct, modeName(s.mode));
    setTxt(wSysBody,sb); }
  if(wDiagBody){ char d[560]; snprintf(d,sizeof(d),"ALARMS (%u)\n",(unsigned)s.alarmCount);
    if(s.alarmCount==0) strncat(d,"  none - all clear\n",sizeof(d)-strlen(d)-1);
    for(uint8_t i=0;i<s.alarmCount && i<4;i++){ char ln[120]; snprintf(ln,sizeof(ln),"  ! %s\n",friendlyAlarm(s.alarms[i].text)); strncat(d,ln,sizeof(d)-strlen(d)-1); }
    if(s.hasBus){   // #101: CT-485 section is bus-persona only
      char bus[160];
      if(s.busFrames>0) snprintf(bus,sizeof(bus),"\nCT-485 BUS\n  %lu frames decoded   bus %s\n",(unsigned long)s.busFrames,s.busOk?"alive":"quiet");
      else snprintf(bus,sizeof(bus),"\nCT-485 BUS (listen-only)\n  0 frames - wire RS-485 + enable UART to sniff\n");
      strncat(d,bus,sizeof(d)-strlen(d)-1); }
    char lk[110];
    if(s.hasBus) snprintf(lk,sizeof(lk),"\nLINKS\n  WiFi %s   MQTT %s   Bus %s",s.wifiOk?"up":"down",s.mqttOk?"up":"down",s.busOk?"up":"down");
    else         snprintf(lk,sizeof(lk),"\nLINKS\n  WiFi %s   MQTT %s",s.wifiOk?"up":"down",s.mqttOk?"up":"down");
    strncat(d,lk,sizeof(d)-strlen(d)-1); setTxt(wDiagBody,d); }
  if(gBtnListen){ if(s.hasBus) lv_obj_clear_flag(gBtnListen,LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(gBtnListen,LV_OBJ_FLAG_HIDDEN); }   // #101
  // Settings category-card live summaries (IA reorg): one condensed line per
  // card, refreshed every renderMain like the old WiFi/Home status words were.
  if(wCatMode){ const char*mnm; switch(s.mode){ case UserMode::kHeat:mnm="Heat"; break; case UserMode::kCool:mnm="Cool"; break;
      case UserMode::kAuto:mnm="Auto"; break; case UserMode::kEmergencyHeat:mnm="Emergency heat"; break; default:mnm="Off"; }
    setTxt(wCatMode,mnm); }
  if(wCatNet){ char ss[33],ip[20]; int8_t rs=0; bool wc=false; wifi_prov::status(ss,sizeof(ss),ip,sizeof(ip),&rs,&wc);
    snprintf(b,sizeof(b),"WiFi %s    %s    Home %s", wc?ss:"not set", wc?ip:"offline", s.mqttOk?"connected":"offline");
    setTxt(wCatNet,b); }
  if(wCatFan){ const uint8_t fm=uiFanMode();
    if(fm==2){ const uint8_t pc=uiFanCircPct(); const char*sp=pc>=63?"High":(pc>=38?"Med":"Low");
      snprintf(b,sizeof(b),"Circulate    %lumin %s",(unsigned long)uiFanCircMin(),sp); }
    else strcpy(b, fm==1?"On":"Auto");
    setTxt(wCatFan,b); }
  if(wCatDisp) setTxt(wCatDisp, uiClock24()?"24-hour clock":"12-hour clock");
  if(wCatSec){ bool unlocked=false,pin=false; L(); unlocked=gM->lockState()==LockState::kUnlocked; pin=gM->userPinSet(); U();
    snprintf(b,sizeof(b),"%s    PIN %s",unlocked?"Unlocked":"Locked",pin?"set":"none");
    setTxt(wCatSec,b); lv_obj_set_style_text_color(wCatSec,lv_color_hex(unlocked?COL_MUTED:COL_WARN),0); }
  if(wCatSys){ using O=DisplayState::OtaUi;
    const char*hint = s.otaState==O::kUpdateAvailable?"  \xE2\x80\xA2 update available":
                      s.otaState==O::kDownloading?"  \xE2\x80\xA2 downloading":
                      s.otaState==O::kStaged?"  \xE2\x80\xA2 update ready":"";
    snprintf(b,sizeof(b),"Firmware " SLYTHERM_FW_BUILD "%s",hint); setTxt(wCatSys,b); }
  renderSysOta(s);
  }

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

// #156: parse a "<key>":[d0,d1,...] int-deci-degree array into out[kGraphPts];
// short/missing -> LV_CHART_POINT_NONE (the publisher emits -32768 == NONE).
static void parseDeciArray(const char* json,const char* key,lv_coord_t* out){
  for(int i=0;i<kGraphPts;i++) out[i]=LV_CHART_POINT_NONE;
  const char* p=strstr(json,key); if(!p) return; p=strchr(p,'['); if(!p) return; p++;
  int i=0;
  while(*p && *p!=']' && i<kGraphPts){
    while(*p==' '||*p==',') p++;
    if(*p==']'||!*p) break;
    char* end; long v=strtol(p,&end,10);
    if(end==p) break;                 // no digits consumed -> malformed, stop
    out[i++]=(lv_coord_t)v; p=end;
  }
}
// MQTT-task side: parse the retained series into gGraphIn + flag dirty. No LVGL.
void ingestGraphSeries(const char* json){
  parseDeciArray(json,"\"room\"",gGraphIn[0]);
  parseDeciArray(json,"\"set\"", gGraphIn[1]);
  parseDeciArray(json,"\"oat\"", gGraphIn[2]);
  gGraphDirty=true;
  Serial.printf("[graph] rx %u B (room[0]=%d set[0]=%d oat[0]=%d)\n",
                (unsigned)strlen(json),(int)gGraphIn[0][0],(int)gGraphIn[1][0],(int)gGraphIn[2][0]);
}
// UI-task side: copy the parsed series into the chart rings + auto-range + refresh.
void applyGraphIfDirty(){
  if(!gGraphDirty || !gSysChart) return;
  gGraphDirty=false;
  memcpy(gRingA,gGraphIn[0],sizeof(gRingA));   // room (COL_INK / white)
  memcpy(gRingH,gGraphIn[1],sizeof(gRingH));   // setpoint (COL_EMBER / amber)
  memcpy(gRingC,gGraphIn[2],sizeof(gRingC));   // outside (COL_CRYO / cyan)
  lv_coord_t lo=32767,hi=-32768; lv_coord_t* rs[3]={gRingA,gRingH,gRingC};
  for(int k=0;k<3;k++) for(int i=0;i<kGraphPts;i++){ lv_coord_t v=rs[k][i]; if(v==LV_CHART_POINT_NONE) continue; if(v<lo)lo=v; if(v>hi)hi=v; }
  if(hi>=lo){ lo-=20; hi+=20; if(hi-lo<40){ lv_coord_t mid=(lv_coord_t)((lo+hi)/2); lo=(lv_coord_t)(mid-40); hi=(lv_coord_t)(mid+40); }
    lv_chart_set_range(gSysChart,LV_CHART_AXIS_PRIMARY_Y,lo,hi); }
  lv_chart_refresh(gSysChart);
  gGraphCentral=true;
  if(wSysGraphLbl) lv_label_set_text(wSysGraphLbl,"#EAF0F4 Room#   #F0A030 Setpoint#   #38C0E8 Outside#");  // colored legend matching the 3 lines
}

}  // namespace slytherm_ui
