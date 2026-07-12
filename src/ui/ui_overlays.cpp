// ui_overlays.cpp — overlays + auxiliary screens (#114): PIN keypad, hold
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
#ifdef SLYTHERM_WG
#include "remote_vpn.h"   // Networking sheet: VPN status word + tap-to-retry
#endif

// #113: injected by tools/version_flag.py; fallback keeps ad-hoc builds compiling.
#ifndef SLYTHERM_FW_BUILD
#define SLYTHERM_FW_BUILD "0.0.0-dev"
#endif

namespace slytherm_ui {

// WiFi setup — state machine (one screen per state; flow per docs/09 + mockup)
lv_obj_t *gHoldSheet=nullptr;                    // hold-duration chooser overlay (#81)
lv_obj_t *taPass=nullptr, *taSsid=nullptr, *kbd=nullptr, *wfLine=nullptr, *wfSub=nullptr;
enum class WifiState { Status, Scanning, List, Password, Other, Connecting, ResultOk, ResultFail };
WifiState gWs=WifiState::Status;
char gSelSsid[33]={};
char gNetSsids[wifi_prov::kMaxNets][33]={};
// Home-system (MQTT broker) manual entry — Installer/Advanced (mqtt_cfg)
lv_obj_t *srvOv=nullptr,*taHost=nullptr,*taPort=nullptr,*taUser=nullptr,*taSrvPass=nullptr,*kbdS=nullptr,*lblSrvStat=nullptr;
// RS-485 LISTEN capture screen (#71) — dedicated top-level screen (like scrAmb)
lv_obj_t *scrSniff=nullptr,*wSniffStat=nullptr,*wSniffCount=nullptr,*wSniffList=nullptr,*wSniffFoot=nullptr;
// keypad
lv_obj_t *kpad=nullptr,*kpadTitle=nullptr,*kpadDots=nullptr;
KpMode kpMode=KpMode::Set; uint8_t kpBuf[4]; int kpN=0;
constexpr uint32_t kPinSalt=0x5A17C0DE;

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
static void showVac(){ if(!gVacSheet) return; vacRefresh(); lv_obj_clear_flag(gVacSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gVacSheet); }
void vacOpen(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } showVac(); }
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

// ---- Fan settings sheet (issue #128) ---------------------------------------
// Mode = Auto / On / Circulate; when Circulate, minutes-per-hour + speed
// Low/Med/High. Each control APPLIES IMMEDIATELY through the shared
// uiSetFanMode/uiSetFanCirculate hooks (like the clock toggle): the Controller
// applies locally (globals + NVS + retained MQTT state); a Remote forwards the
// change to the Controller over the fan cmd topics. On open we seed the local
// selection from the current values (uiFanMode/uiFanCircMin/uiFanCircPct).
lv_obj_t *gFanSheet=nullptr,*gFanModeBtn[3]={},*gFanMinLbl=nullptr,*gFanSpdBtn[3]={},*gFanCircHdr=nullptr;
uint8_t gFanSelMode=0, gFanSpdSel=0;    // mode 0/1/2 (auto/on/circulate); speed idx 0/1/2 -> 25/50/75
uint32_t gFanMinSel=15;
static const uint32_t kFanMinSet[5]={5,10,15,20,30};  // the panel's minutes-per-hour choices
static const uint8_t  kFanSpdPct[3]={25,50,75};       // Low/Med/High (docs/02 §5a)
static int fanMinIdx(uint32_t m){ int best=2; uint32_t bd=~0u;   // nearest slot in the set (default 15)
  for(int i=0;i<5;i++){ uint32_t d=m>kFanMinSet[i]?m-kFanMinSet[i]:kFanMinSet[i]-m; if(d<bd){bd=d;best=i;} } return best; }
static int fanSpdIdx(uint8_t p){ int best=0; int bd=999;
  for(int i=0;i<3;i++){ int d=(int)p-(int)kFanSpdPct[i]; if(d<0)d=-d; if(d<bd){bd=d;best=i;} } return best; }
// Segmented fill: the active segment gets a solid accent, the rest stay a muted
// raised track (matches the Home mode row's SOLID fill — no gradient).
static void fanSeg(lv_obj_t*b,bool on,uint32_t accent){ if(!b) return; lv_obj_t*l=lv_obj_get_child(b,0);
  if(on){ lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0); lv_obj_set_style_bg_color(b,lv_color_hex(accent),0);
    if(l) lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); }
  else { lv_obj_set_style_bg_opa(b,LV_OPA_COVER,0); lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),0);
    if(l) lv_obj_set_style_text_color(l,lv_color_hex(COL_MUTED),0); } }
void fanRefresh(){ if(!gFanSheet) return;
  for(int i=0;i<3;i++) fanSeg(gFanModeBtn[i], gFanSelMode==(uint8_t)i, COL_CRYO);
  const bool circ=gFanSelMode==2;   // circulate controls shown always, greyed unless active (#128)
  char b[16]; snprintf(b,sizeof(b),"%lu min",(unsigned long)gFanMinSel); setTxt(gFanMinLbl,b);
  for(int i=0;i<3;i++) fanSeg(gFanSpdBtn[i], circ&&gFanSpdSel==(uint8_t)i, COL_CRYO);
  if(gFanCircHdr) lv_obj_set_style_text_color(gFanCircHdr,lv_color_hex(circ?COL_MUTED:COL_TEXT3),0);
  if(gFanMinLbl)  lv_obj_set_style_text_color(gFanMinLbl,lv_color_hex(circ?COL_INK:COL_TEXT3),0); }
void fanModeEvt(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; }
  gFanSelMode=(uint8_t)(intptr_t)lv_event_get_user_data(e); uiSetFanMode(gFanSelMode); fanRefresh(); }
void fanMinStep(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; } if(gFanSelMode!=2) return;  // greyed unless circulate
  int d=(int)(intptr_t)lv_event_get_user_data(e); int i=fanMinIdx(gFanMinSel)+d; if(i<0)i=0; if(i>4)i=4;
  gFanMinSel=kFanMinSet[i]; uiSetFanCirculate(gFanMinSel,kFanSpdPct[gFanSpdSel]); fanRefresh(); }
void fanSpdEvt(lv_event_t*e){ if(uiLocked()){ promptUnlock(); return; } if(gFanSelMode!=2) return;
  gFanSpdSel=(uint8_t)(intptr_t)lv_event_get_user_data(e); uiSetFanCirculate(gFanMinSel,kFanSpdPct[gFanSpdSel]); fanRefresh(); }
void fanClose(lv_event_t*){ if(gFanSheet) lv_obj_add_flag(gFanSheet,LV_OBJ_FLAG_HIDDEN); }
// Seed the selection from the live values and show the sheet. Split out from
// openFan so the screenshot-capture nav (screenshotPoll) can present the sheet
// WITHOUT the lock gate — opening a sheet is view-only, it actuates nothing.
static void showFan(){ if(!gFanSheet) return;
  gFanSelMode=uiFanMode(); if(gFanSelMode>2) gFanSelMode=0;
  gFanMinSel=kFanMinSet[fanMinIdx(uiFanCircMin())]; gFanSpdSel=(uint8_t)fanSpdIdx(uiFanCircPct());
  fanRefresh(); lv_obj_clear_flag(gFanSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gFanSheet); }
void openFan(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } showFan(); }
void buildFanSheet(lv_obj_t*scr){ gFanSheet=lv_obj_create(scr); lv_obj_set_size(gFanSheet,470,400); lv_obj_center(gFanSheet);
  lv_obj_set_style_bg_color(gFanSheet,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(gFanSheet,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(gFanSheet,2,0); lv_obj_set_style_radius(gFanSheet,14,0); lv_obj_set_style_pad_all(gFanSheet,0,0); lv_obj_clear_flag(gFanSheet,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(gFanSheet); lv_label_set_text(t,"Fan"); lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(t,lv_color_hex(COL_INK),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,18,14);
  lv_obj_t*sub=lv_label_create(gFanSheet); lv_label_set_text(sub,"How the blower runs between heat/cool calls"); lv_obj_set_style_text_font(sub,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(sub,lv_color_hex(COL_TEXT3),0); lv_obj_align(sub,LV_ALIGN_TOP_LEFT,18,50);
  // Mode segmented row: Auto / On / Circulate
  { const char*mn[3]={"Auto","On","Circulate"};
    for(int i=0;i<3;i++){ lv_obj_t*bn=lv_btn_create(gFanSheet); lv_obj_set_size(bn,144,52); lv_obj_align(bn,LV_ALIGN_TOP_LEFT,18+i*148,84);
      lv_obj_set_style_bg_color(bn,lv_color_hex(COL_RAISED),0); lv_obj_set_style_shadow_width(bn,0,0); lv_obj_set_style_radius(bn,9,0);
      lv_obj_add_event_cb(bn,fanModeEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i);
      lv_obj_t*l=lv_label_create(bn); lv_label_set_text(l,mn[i]); lv_obj_center(l); gFanModeBtn[i]=bn; } }
  // Circulate section: header + minutes-per-hour stepper + Low/Med/High speed
  gFanCircHdr=lv_label_create(gFanSheet); lv_label_set_text(gFanCircHdr,"Circulate: run the blower each hour");
  lv_obj_set_style_text_font(gFanCircHdr,&lv_font_montserrat_16,0); lv_obj_align(gFanCircHdr,LV_ALIGN_TOP_LEFT,18,152);
  { lv_obj_t*nl=lv_label_create(gFanSheet); lv_label_set_text(nl,"Minutes / hour"); lv_obj_set_style_text_color(nl,lv_color_hex(COL_MUTED),0);
    lv_obj_set_style_text_font(nl,&lv_font_montserrat_20,0); lv_obj_align(nl,LV_ALIGN_TOP_LEFT,18,192);
    lv_obj_t*minus=lv_btn_create(gFanSheet); lv_obj_set_size(minus,46,44); lv_obj_align(minus,LV_ALIGN_TOP_RIGHT,-150,180);
    lv_obj_set_style_bg_color(minus,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(minus,fanMinStep,LV_EVENT_CLICKED,(void*)(intptr_t)-1);
    { lv_obj_t*l=lv_label_create(minus); lv_label_set_text(l,"-"); lv_obj_center(l); }
    gFanMinLbl=lv_label_create(gFanSheet); lv_obj_set_style_text_font(gFanMinLbl,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(gFanMinLbl,lv_color_hex(COL_INK),0);
    lv_obj_set_width(gFanMinLbl,92); lv_obj_set_style_text_align(gFanMinLbl,LV_TEXT_ALIGN_CENTER,0); lv_obj_align(gFanMinLbl,LV_ALIGN_TOP_RIGHT,-52,192);
    lv_obj_t*plus=lv_btn_create(gFanSheet); lv_obj_set_size(plus,46,44); lv_obj_align(plus,LV_ALIGN_TOP_RIGHT,-4,180);
    lv_obj_set_style_bg_color(plus,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(plus,fanMinStep,LV_EVENT_CLICKED,(void*)(intptr_t)1);
    { lv_obj_t*l=lv_label_create(plus); lv_label_set_text(l,"+"); lv_obj_center(l); } }
  { lv_obj_t*sp=lv_label_create(gFanSheet); lv_label_set_text(sp,"Speed"); lv_obj_set_style_text_color(sp,lv_color_hex(COL_MUTED),0);
    lv_obj_set_style_text_font(sp,&lv_font_montserrat_20,0); lv_obj_align(sp,LV_ALIGN_TOP_LEFT,18,252);
    const char*sn[3]={"Low","Med","High"};
    for(int i=0;i<3;i++){ lv_obj_t*bn=lv_btn_create(gFanSheet); lv_obj_set_size(bn,116,46); lv_obj_align(bn,LV_ALIGN_TOP_LEFT,110+i*118,246);
      lv_obj_set_style_bg_color(bn,lv_color_hex(COL_RAISED),0); lv_obj_set_style_shadow_width(bn,0,0); lv_obj_set_style_radius(bn,9,0);
      lv_obj_add_event_cb(bn,fanSpdEvt,LV_EVENT_CLICKED,(void*)(intptr_t)i);
      lv_obj_t*l=lv_label_create(bn); lv_label_set_text(l,sn[i]); lv_obj_center(l); gFanSpdBtn[i]=bn; } }
  { lv_obj_t*note=lv_label_create(gFanSheet); lv_label_set_text(note,"Quiet low-speed circulation evens out the rooms.");
    lv_obj_set_style_text_font(note,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(note,lv_color_hex(COL_TEXT3),0);
    lv_obj_align(note,LV_ALIGN_TOP_LEFT,18,306); }
  lv_obj_t*done=lv_btn_create(gFanSheet); lv_obj_set_size(done,190,46); lv_obj_align(done,LV_ALIGN_BOTTOM_MID,0,-16);
  lv_obj_set_style_bg_color(done,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(done,fanClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(done); lv_label_set_text(l,"Done"); lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); lv_obj_center(l); }
  lv_obj_t*x=lv_btn_create(gFanSheet); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-6,8);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,fanClose,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  fanRefresh(); lv_obj_add_flag(gFanSheet,LV_OBJ_FLAG_HIDDEN); }

// ---- Settings category sub-sheets (Settings IA reorg) ----------------------
// The Settings tab is now a list of category cards; each drills into one of
// these centered card sheets (same grammar as the Fan/Vacation sheets: COL_CARD
// body, COL_CRYO hairline, a top-right X that hide-reveals the Settings tab as
// the one-level back-nav). The card controls' handlers live here with the sheets
// they belong to. Openers are view-only (no lock gate) so a locked panel still
// DISPLAYS them; the ACTIONS inside stay lock-aware exactly as they were before.
lv_obj_t *gNetSheet=nullptr,*wNetFacts=nullptr;
lv_obj_t *gDispSheet=nullptr,*gClkLbl=nullptr;   // gClkLbl: Display sheet 12/24h label
lv_obj_t *gSecSheet=nullptr,*wSecState=nullptr;
lv_obj_t *gSysSheet=nullptr;
#ifdef SLYTHERM_WG
lv_obj_t *wSetVpn=nullptr;   // #148 VPN status word — now inside the Networking sheet
#endif

static void sheetClose(lv_event_t*e){ lv_obj_t* sheet=(lv_obj_t*)lv_event_get_user_data(e); if(sheet) lv_obj_add_flag(sheet,LV_OBJ_FLAG_HIDDEN); }
// Build a hidden centered card sheet with a title, optional subtitle, and the
// top-right X close — the shared shell every category sub-sheet is built on.
static lv_obj_t* sheetShell(lv_obj_t*scr,int w,int h,const char*title,const char*sub){
  lv_obj_t*sh=lv_obj_create(scr); lv_obj_set_size(sh,w,h); lv_obj_center(sh);
  lv_obj_set_style_bg_color(sh,lv_color_hex(COL_CARD),0); lv_obj_set_style_border_color(sh,lv_color_hex(COL_CRYO),0);
  lv_obj_set_style_border_width(sh,2,0); lv_obj_set_style_radius(sh,14,0); lv_obj_set_style_pad_all(sh,0,0); lv_obj_clear_flag(sh,LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t*t=lv_label_create(sh); lv_label_set_text(t,title); lv_obj_set_style_text_font(t,&lv_font_montserrat_28,0);
  lv_obj_set_style_text_color(t,lv_color_hex(COL_INK),0); lv_obj_align(t,LV_ALIGN_TOP_LEFT,18,14);
  if(sub){ lv_obj_t*s=lv_label_create(sh); lv_label_set_text(s,sub); lv_obj_set_style_text_font(s,&lv_font_montserrat_16,0);
    lv_obj_set_style_text_color(s,lv_color_hex(COL_TEXT3),0); lv_obj_align(s,LV_ALIGN_TOP_LEFT,18,50); }
  lv_obj_t*x=lv_btn_create(sh); lv_obj_set_size(x,46,42); lv_obj_align(x,LV_ALIGN_TOP_RIGHT,-6,8);
  lv_obj_set_style_bg_color(x,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(x,sheetClose,LV_EVENT_CLICKED,sh);
  { lv_obj_t*l=lv_label_create(x); lv_label_set_text(l,LV_SYMBOL_CLOSE); lv_obj_center(l); }
  lv_obj_add_flag(sh,LV_OBJ_FLAG_HIDDEN); return sh; }

// -- category-control handlers (moved here from the old flat Settings tab) --
void clkEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } uiToggleClock24();   // Display: 12/24h (#77)
  if(gClkLbl) setTxt(gClkLbl, uiClock24()?"24-hour":"12-hour"); }
void setPinEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } kpadOpen(KpMode::Set,"Set a 4-digit PIN"); }   // Security
void lockEvt(lv_event_t*){ L(); bool set=gM->userPinSet(); if(set) gM->lockNow(nowS()); U(); }
void unlockEvt(lv_event_t*){ kpadOpen(KpMode::Unlock,"Enter PIN to unlock"); }
void sysOtaStub(lv_event_t*){ telnet_log::logf("[ui] OTA/firmware screen not built yet (#65)"); }   // honest labeled stub
void sysRestore(lv_event_t*){ uiClearReducedMode(); }   // #80: clear safe-UI latch + reboot into full UI
#ifdef SLYTHERM_WG
void vpnRetryEvt(lv_event_t*){ remote_vpn::requestRetry(); }   // #148: posts; the radio task executes
#endif

// -- Networking sheet: WiFi setup + Home system + read-only local facts --
void netRefresh(){ if(!wNetFacts) return; char ss[33],ip[20]; int8_t r=0; bool c=false;
  wifi_prov::status(ss,sizeof(ss),ip,sizeof(ip),&r,&c);
  String gw=WiFi.gatewayIP().toString(), sn=WiFi.subnetMask().toString(), mac=WiFi.macAddress();
  char b[220]; snprintf(b,sizeof(b),
    "Network     %s\nIP          %s\nGateway     %s\nSubnet      %s\nSignal      %d dBm\nMAC         %s",
    c?ss:"not connected", c?ip:"--", gw.c_str(), sn.c_str(), (int)r, mac.c_str());
  setTxt(wNetFacts,b);
#ifdef SLYTHERM_WG
  if(wSetVpn){ const auto vs=remote_vpn::state();
    setTxt(wSetVpn, vs==remote_vpn::State::kUp?"VPN connected":
                    vs==remote_vpn::State::kHandshaking?"VPN connecting":
                    vs==remote_vpn::State::kStandby?"VPN standby":"VPN off");
    lv_obj_set_style_text_color(wSetVpn,lv_color_hex(
      vs==remote_vpn::State::kUp?COL_OK:
      vs==remote_vpn::State::kHandshaking?COL_WARN:
      vs==remote_vpn::State::kStandby?COL_MUTED:COL_CRIT),0); }
#endif
}
void openNet(lv_event_t*){ if(!gNetSheet) return; netRefresh(); lv_obj_clear_flag(gNetSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gNetSheet); }
void buildNetSheet(lv_obj_t*scr){ gNetSheet=sheetShell(scr,540,420,"Networking","This panel's connection");
  lv_obj_t*wb=lv_btn_create(gNetSheet); lv_obj_set_size(wb,238,52); lv_obj_align(wb,LV_ALIGN_TOP_LEFT,18,88);
  lv_obj_set_style_bg_color(wb,lv_color_hex(COL_CRYO),0); lv_obj_add_event_cb(wb,openWifi,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(wb); lv_label_set_text(l,LV_SYMBOL_WIFI "  WiFi setup"); lv_obj_set_style_text_color(l,lv_color_hex(0x06202B),0); lv_obj_center(l); }
  lv_obj_t*hb=lv_btn_create(gNetSheet); lv_obj_set_size(hb,238,52); lv_obj_align(hb,LV_ALIGN_TOP_RIGHT,-18,88);
  lv_obj_set_style_bg_color(hb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(hb,openServer,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(hb); lv_label_set_text(l,LV_SYMBOL_HOME "  Home system"); lv_obj_center(l); }
  wNetFacts=lv_label_create(gNetSheet); lv_obj_set_style_text_font(wNetFacts,&lv_font_montserrat_16,0);
  lv_obj_set_style_text_color(wNetFacts,lv_color_hex(COL_MUTED),0); lv_obj_align(wNetFacts,LV_ALIGN_TOP_LEFT,18,156); lv_label_set_text(wNetFacts,"");
#ifdef SLYTHERM_WG
  lv_obj_t*vb=lv_btn_create(gNetSheet); lv_obj_set_size(vb,190,46); lv_obj_align(vb,LV_ALIGN_BOTTOM_LEFT,18,-14);
  lv_obj_set_style_bg_color(vb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(vb,vpnRetryEvt,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(vb); lv_label_set_text(l,LV_SYMBOL_REFRESH "  Retry VPN"); lv_obj_center(l); }
  wSetVpn=lv_label_create(gNetSheet); lv_obj_set_style_text_font(wSetVpn,&lv_font_montserrat_16,0);
  lv_obj_align(wSetVpn,LV_ALIGN_BOTTOM_LEFT,222,-28); lv_label_set_text(wSetVpn,"");
#endif
}

// -- Display sheet: clock 12/24h (brightness/theme reserved for later) --
void openDisplay(lv_event_t*){ if(!gDispSheet) return; if(gClkLbl) setTxt(gClkLbl,uiClock24()?"24-hour":"12-hour");
  lv_obj_clear_flag(gDispSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gDispSheet); }
void buildDisplaySheet(lv_obj_t*scr){ gDispSheet=sheetShell(scr,470,300,"Display","Clock and appearance");
  lv_obj_t*cl=lv_label_create(gDispSheet); lv_label_set_text(cl,"Clock"); lv_obj_set_style_text_color(cl,lv_color_hex(COL_MUTED),0);
  lv_obj_set_style_text_font(cl,&lv_font_montserrat_20,0); lv_obj_align(cl,LV_ALIGN_TOP_LEFT,18,104);
  lv_obj_t*cb=lv_btn_create(gDispSheet); lv_obj_set_size(cb,180,54); lv_obj_align(cb,LV_ALIGN_TOP_RIGHT,-18,96);
  lv_obj_set_style_bg_color(cb,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(cb,clkEvt,LV_EVENT_CLICKED,nullptr);
  gClkLbl=lv_label_create(cb); lv_label_set_text(gClkLbl,"12-hour"); lv_obj_center(gClkLbl);
  lv_obj_t*note=lv_label_create(gDispSheet); lv_label_set_text(note,"Brightness and theme coming soon.");
  lv_obj_set_style_text_font(note,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(note,lv_color_hex(COL_TEXT3),0);
  lv_obj_align(note,LV_ALIGN_TOP_LEFT,18,170); }

// -- Security sheet: Set PIN / Lock / Unlock --
void secRefresh(){ if(!wSecState) return; bool unlocked=false,pin=false; L(); unlocked=gM->lockState()==LockState::kUnlocked; pin=gM->userPinSet(); U();
  char b[64]; snprintf(b,sizeof(b),"Lock: %s    PIN: %s",unlocked?"unlocked":"LOCKED",pin?"set":"none");
  setTxt(wSecState,b); lv_obj_set_style_text_color(wSecState,lv_color_hex(unlocked?COL_OK:COL_WARN),0); }
void openSecurity(lv_event_t*){ if(!gSecSheet) return; secRefresh(); lv_obj_clear_flag(gSecSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gSecSheet); }
void buildSecuritySheet(lv_obj_t*scr){ gSecSheet=sheetShell(scr,480,340,"Security","Screen lock and PIN");
  wSecState=lv_label_create(gSecSheet); lv_obj_set_style_text_font(wSecState,&lv_font_montserrat_20,0);
  lv_obj_align(wSecState,LV_ALIGN_TOP_LEFT,18,96); lv_label_set_text(wSecState,"");
  struct B{const char*t; lv_event_cb_t cb;} bs[3]={{"Set PIN",setPinEvt},{"Lock",lockEvt},{"Unlock",unlockEvt}};
  for(int i=0;i<3;i++){ lv_obj_t*b=lv_btn_create(gSecSheet); lv_obj_set_size(b,140,56); lv_obj_align(b,LV_ALIGN_TOP_LEFT,18+i*148,150);
    lv_obj_set_style_bg_color(b,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(b,bs[i].cb,LV_EVENT_CLICKED,nullptr);
    lv_obj_t*l=lv_label_create(b); lv_label_set_text(l,bs[i].t); lv_obj_center(l); }
  lv_obj_t*note=lv_label_create(gSecSheet); lv_label_set_text(note,"Lock makes the panel read-only until the PIN is entered.");
  lv_obj_set_style_text_font(note,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(note,lv_color_hex(COL_TEXT3),0);
  lv_obj_set_width(note,444); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP); lv_obj_align(note,LV_ALIGN_TOP_LEFT,18,224); }

// -- System sheet: firmware id + OTA stub (#65) + restore-full-screen --
void openSystem(lv_event_t*){ if(!gSysSheet) return; lv_obj_clear_flag(gSysSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gSysSheet); }
void buildSystemSheet(lv_obj_t*scr){ gSysSheet=sheetShell(scr,480,340,"System","Firmware and recovery");
  lv_obj_t*fw=lv_label_create(gSysSheet); lv_label_set_text(fw,"Firmware " SLYTHERM_FW_BUILD);
  lv_obj_set_style_text_font(fw,&lv_font_montserrat_20,0); lv_obj_set_style_text_color(fw,lv_color_hex(COL_INK),0); lv_obj_align(fw,LV_ALIGN_TOP_LEFT,18,96);
  lv_obj_t*ob=lv_btn_create(gSysSheet); lv_obj_set_size(ob,300,50); lv_obj_align(ob,LV_ALIGN_TOP_LEFT,18,140);
  lv_obj_set_style_bg_color(ob,lv_color_hex(COL_RAISED),0); lv_obj_add_event_cb(ob,sysOtaStub,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(ob); lv_label_set_text(l,LV_SYMBOL_DOWNLOAD "  Firmware update  (soon)"); lv_obj_set_style_text_color(l,lv_color_hex(COL_TEXT3),0); lv_obj_center(l); }
  lv_obj_t*rb=lv_btn_create(gSysSheet); lv_obj_set_size(rb,300,50); lv_obj_align(rb,LV_ALIGN_TOP_LEFT,18,204);
  lv_obj_set_style_bg_color(rb,lv_color_hex(COL_RAISED),0); lv_obj_set_style_border_color(rb,lv_color_hex(COL_WARN),0); lv_obj_set_style_border_width(rb,1,0);
  lv_obj_add_event_cb(rb,sysRestore,LV_EVENT_CLICKED,nullptr);
  { lv_obj_t*l=lv_label_create(rb); lv_label_set_text(l,LV_SYMBOL_REFRESH "  Restore full screen"); lv_obj_center(l); }
  lv_obj_t*note=lv_label_create(gSysSheet); lv_label_set_text(note,"Restore reboots into the full UI (clears the safe-mode latch).");
  lv_obj_set_style_text_font(note,&lv_font_montserrat_16,0); lv_obj_set_style_text_color(note,lv_color_hex(COL_TEXT3),0);
  lv_obj_set_width(note,444); lv_label_set_long_mode(note,LV_LABEL_LONG_WRAP); lv_obj_align(note,LV_ALIGN_TOP_LEFT,18,266); }

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
static void showHold(){ if(!gHoldSheet) return; lv_obj_clear_flag(gHoldSheet,LV_OBJ_FLAG_HIDDEN); lv_obj_move_foreground(gHoldSheet); }
void holdEvt(lv_event_t*){ if(uiLocked()){ promptUnlock(); return; } showHold(); }
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

// ---- WiFi setup: state machine (Settings -> WiFi setup) --------------------
void wifiGoto(WifiState s);  // fwd


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

// Drive the display to a named screen for automated documentation capture.
// VIEW-ONLY: switches tabs and opens sheets, never actuates a control (no
// mode/setpoint change — those go over MQTT during capture), and BYPASSES the
// screen lock (opening a sheet shows nothing a locked panel shouldn't reveal),
// so a scripted capture works on a locked bench panel. Vocabulary:
//   bare "0".."5"            — tab by index (back-compat)
//   "tab:home|presets|sensors|system|settings|diag"
//   "sheet:fan|networking|display|security|system"  (Settings-group sheets)
//   "sheet:wifi|home|hold|vacation"
// Unknown/empty -> leave the current screen (just snapshot what's up).
static int tabIndex(const char* n){
  if(!strcmp(n,"home"))return 0;     if(!strcmp(n,"presets"))return 1;
  if(!strcmp(n,"sensors"))return 2;  if(!strcmp(n,"system"))return 3;
  if(!strcmp(n,"settings"))return 4; if(!strcmp(n,"diag"))return 5; return -1; }
static void navToTab(int i){ if(gTabview && i>=0 && i<6) lv_tabview_set_act(gTabview,(uint32_t)i,LV_ANIM_OFF); }
static void navScreen(const char* cmd){
  if(!cmd || !cmd[0]) return;
  bool digits=true; for(const char*p=cmd;*p;++p) if(*p<'0'||*p>'9'){ digits=false; break; }
  if(digits){ navToTab(atoi(cmd)); return; }
  if(!strncmp(cmd,"tab:",4)){ navToTab(tabIndex(cmd+4)); return; }
  if(!strncmp(cmd,"sheet:",6)){ const char* n=cmd+6;
    // Settings-group sheets: show the Settings tab behind them first.
    if(!strcmp(n,"fan")){        navToTab(4); showFan(); }
    else if(!strcmp(n,"networking")){ navToTab(4); openNet(nullptr); }
    else if(!strcmp(n,"display")){    navToTab(4); openDisplay(nullptr); }
    else if(!strcmp(n,"security")){   navToTab(4); openSecurity(nullptr); }
    else if(!strcmp(n,"system")){     navToTab(4); openSystem(nullptr); }
    else if(!strcmp(n,"wifi")){       openWifi(nullptr); }
    else if(!strcmp(n,"home")){       openServer(nullptr); }
    else if(!strcmp(n,"hold")){       navToTab(0); showHold(); }
    else if(!strcmp(n,"vacation")){   navToTab(1); showVac(); }
    return; }
  // unrecognized token -> no navigation
}

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
  while(millis()-t0<250){ if(c.available()){ char ch=c.read(); if(ch=='\n'||ch=='\r') break; cmd+=ch; if(cmd.length()>=24) break; } }  // cap fits "sheet:networking"
  if(cmd.length()>0){ navScreen(cmd.c_str()); lv_refr_now(NULL); }   // drive to the requested tab/sheet, then snapshot below
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

}  // namespace slytherm_ui
