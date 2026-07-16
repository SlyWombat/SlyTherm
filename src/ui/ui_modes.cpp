// ui_modes.cpp — full-screen modes (#114): ambient idle screen (+burn-in
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
// ambient widgets
lv_obj_t *aName,*aTemp,*aTarget,*aAlarm,*aNow=nullptr;

// ---- ambient (idle) screen ----
// Burn-in guard (#70/#86b/#86c): the WHOLE ambient hero does a smooth ping-pong
// "walk" across the panel — one step per drift, right to the right clamp, then
// reverse and walk back left, bouncing at the edges, with a gentle diagonal Y
// drift so it uses the vertical space too. NO teleporting between corners.
// The clamp is computed from the ACTUAL rendered label geometry every drift, so
// nothing ever clips regardless of room-name length or temperature width.
constexpr int      kAmbSteps   = 6;                 // steps across each direction
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
  if(gBlanked){ portBacklight(true); gBlanked=false; } }
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
  // #108 persona wording: on the busless Remote the later rows gate on the
  // Controller link (echo -> fused temp), not HA/local sensors — the same
  // underlying flags, homeowner-appropriate labels.
  bootRow(bcMqtt, s.hasBus?"Home Assistant":"Home system", s.mqttOk);
  bootRow(bcOat,"Outdoor temperature",s.outdoorValid);
  bootRow(bcRoom, s.hasBus?"Room sensors":"Controller link", s.fusedTempValid);
  setTxt(wBootStat, (s.outdoorValid&&s.fusedTempValid)?"Ready":(s.hasBus?"Warming up...":"Discovering Controller...")); }
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

void renderAmbient(const DisplayState& s){ char b[80];
  // NOW temp — the same fused control temp the Home hero shows (not the raw dominant sensor)
  snprintf(b,sizeof(b),"%.1f\xC2\xB0",(double)s.fusedTempC); setTxt(aTemp, s.fusedTempValid?b:"--.-\xC2\xB0");
  { const bool heat=s.action==HvacAction::kHeating||s.action==HvacAction::kDefrosting, cool=s.action==HvacAction::kCooling;
    // #86: dim ambient theme — no bright-white temp; use the dimmed heat/cool greys
    lv_obj_set_style_text_color(aTemp,lv_color_hex(heat?COL_DIM_EMB:cool?COL_DIM_CRY:COL_TEXT3),0);
    if(s.mode==UserMode::kOff && (heat||cool)){ strcpy(b,"Off \xE2\x80\xA2 finishing cycle"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }  // #173: mode off but compressor still serving its min-ON
    else if(heat){ snprintf(b,sizeof(b),"Heating to %.1f\xC2\xB0",(double)s.heatSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_EMB),0); }
    else if(cool){ snprintf(b,sizeof(b),"Cooling to %.1f\xC2\xB0",(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_DIM_CRY),0); }
    else if(s.compressorHeldOff){ const bool cs=s.compressorHeldSide==SetpointSide::kCool;   // min-OFF ack (mirrors Home hero)
      snprintf(b,sizeof(b),"%s soon \xE2\x80\xA2 %lu min",cs?"Cooling":"Heating",(unsigned long)((s.compressorHeldRemainS+59u)/60u)); lv_obj_set_style_text_color(aTarget,lv_color_hex(cs?COL_DIM_CRY:COL_DIM_EMB),0); }
    else if(!s.fusedTempValid){ strcpy(b,"Connecting to controller..."); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }   // no controller link yet -> never falsely read "off"
    else if(s.mode==UserMode::kOff){ strcpy(b,"System off"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else if(s.mode==UserMode::kAuto){ snprintf(b,sizeof(b),"Idle - holding %.0f-%.0f\xC2\xB0",(double)s.heatSetpointC,(double)s.coolSetpointC); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    else { strcpy(b,"Idle"); lv_obj_set_style_text_color(aTarget,lv_color_hex(COL_MUTED),0); }
    setTxt(aTarget,b); }
  fillPresenceLine(s,b,sizeof(b)); setTxt(aName,b);   // #88: sticky HA-last-seen presence
  if(s.alarmCount>0){ lv_obj_clear_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); setTxt(aAlarm, s.alarms[0].text[0]?friendlyAlarm(s.alarms[0].text):"alarm"); }
  else lv_obj_add_flag(aAlarm,LV_OBJ_FLAG_HIDDEN); }

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

}  // namespace slytherm_ui
