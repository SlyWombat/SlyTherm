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

}  // namespace slytherm_ui
