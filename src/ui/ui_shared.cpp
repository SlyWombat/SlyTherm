// ui_shared.cpp — shared state + small helpers for the split wall UI (#114).
// See ui_shared.h for the module map. Everything here moved verbatim from the
// pre-split slytherm_ui.cpp.

#include <Arduino.h>

#include "ui_shared.h"

#include <cstring>

namespace slytherm_ui {

// ---- shared model + mutex ----
UiModel*          gM   = nullptr;
SemaphoreHandle_t gMux = nullptr;

// ---- cross-module screens, widgets and mode flags ----
bool gReduced=false;   // #80 reduced safe-UI latch
lv_obj_t *scrMain=nullptr, *scrAmb=nullptr, *scrBoot=nullptr, *scrWelcome=nullptr, *gTabview=nullptr;
lv_obj_t *wifiOv=nullptr;
bool gAmbient=false;
bool gBlanked=false;  // #86: deep-screensaver latch — backlight off, restore on wake
bool gWelcomeActive=false;  // #82: first-run onboarding gate (no saved WiFi)
bool gBootActive=false,gBootExiting=false; uint32_t gBootStartMs=0;  // #92: warm-up splash gate (hold UI until key sensors live, <=60s)
uint32_t gAmbShiftMs=0; uint8_t gAmbShiftIdx=0;  // ambient burn-in pixel-shift ring (#70)
uint32_t gGraphLastMs=0;
bool gWifiOpen=false, gSrvOpen=false, gSniffOpen=false;

// ---- small helpers ----
uint32_t nowS(){ return millis()/1000; }

const char* modeName(UserMode m){ switch(m){case UserMode::kHeat:return "HEAT";case UserMode::kCool:return "COOL";
  case UserMode::kAuto:return "AUTO";case UserMode::kEmergencyHeat:return "EM HEAT";default:return "OFF";} }
const char* actName(HvacAction a){ switch(a){case HvacAction::kHeating:return "Heating";case HvacAction::kCooling:return "Cooling";
  case HvacAction::kFanOnly:return "Fan";case HvacAction::kDefrosting:return "Defrost";default:return "Idle";} }
uint32_t actCol(HvacAction a){ switch(a){case HvacAction::kHeating:case HvacAction::kDefrosting:return COL_EMBER;
  case HvacAction::kCooling:return COL_CRYO;default:return COL_MUTED;} }

void setTxt(lv_obj_t*o,const char*t){ const char*c=lv_label_get_text(o); if(c&&strcmp(c,t)==0)return; lv_label_set_text(o,t); }

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

lv_obj_t* card(lv_obj_t*p){ lv_obj_t*c=lv_obj_create(p); lv_obj_set_style_bg_color(c,lv_color_hex(COL_CARD),0);
  lv_obj_set_style_border_width(c,0,0); lv_obj_set_style_radius(c,14,0); lv_obj_clear_flag(c,LV_OBJ_FLAG_SCROLLABLE); return c; }

lv_obj_t* header(lv_obj_t*tab,const char*t){ lv_obj_t*h=lv_label_create(tab); lv_label_set_text(h,t);
  lv_obj_set_style_text_font(h,&lv_font_montserrat_28,0); lv_obj_set_style_text_color(h,lv_color_hex(COL_CRYO),0);
  lv_obj_align(h,LV_ALIGN_TOP_LEFT,4,0); return h; }

lv_obj_t* mkBtn(lv_obj_t*p,const char*t,lv_event_cb_t cb,lv_align_t al,int x,int y,uint32_t bg,int w){
  lv_obj_t*b=lv_btn_create(p); lv_obj_set_size(b,w,46); lv_obj_align(b,al,x,y);
  lv_obj_set_style_bg_color(b,lv_color_hex(bg),0); lv_obj_add_event_cb(b,cb,LV_EVENT_CLICKED,nullptr);
  lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,t);
  if(bg==COL_CRYO) lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); lv_obj_center(l); return b; }

// tracked-out caption ("eyebrow"): small Montserrat + wide letter-spacing (docs/09)
void eyebrow(lv_obj_t*l){ lv_obj_set_style_text_font(l,&lv_font_montserrat_16,0); lv_obj_set_style_text_letter_space(l,2,0); }

bool uiLocked(){ L(); bool lk=gM->lockState()!=LockState::kUnlocked; U(); return lk; }  // locked = read-only

}  // namespace slytherm_ui
