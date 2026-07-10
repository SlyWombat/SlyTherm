// slytherm_ui.cpp — wall-UI orchestrator. See slytherm_ui.h for the public
// API (unchanged by the #114 split) and ui/ui_shared.h for the module map:
// the screens live in src/ui/ui_{main,overlays,modes}.cpp and the board
// display/touch/backlight port in src/ui/ui_port_*.cpp. This file only wires
// begin() (port bring-up + screen build) and service() (tick/render loop).
// Palette per docs/09; ambient idle screen per docs/09 §8.

#include <Arduino.h>
#include <lvgl.h>

#include "slytherm_ui.h"
#include "ui/ui_shared.h"
#include "ui/ui_port.h"
#include "SleepState.h"   // #90: shared night-window constants (kSleepStartHour/EndHour)
#include "wifi_prov.h"
#include "telnet_log.h"

namespace slytherm_ui {

void begin(UiModel* model, SemaphoreHandle_t mux, bool reducedUi, bool firstRun){
  gM=model; gMux=mux; gReduced=reducedUi;
  portInit();
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
        // #147: onboarding lands on the #92 warm-up splash (pulsing logo +
        // link checklist), not a half-populated Home — the splash's own gate
        // rolls to Home once the connections and temps are live.
        gBootActive=true; gBootStartMs=millis(); lv_scr_load(scrBoot);
        Serial.println("[ui] onboarding connected -> #92 warm-up splash"); }
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
    // touch still reaches the touch controller with the light off and ambWake
    // restores. Outside the window we NEVER blank (ambient stays lit).
    // getLocalTime FAIL -> hour unknown -> fail SAFE: don't blank, and restore
    // if already blanked.
    { int hour=-1; struct tm ti; if(getLocalTime(&ti,0)) hour=ti.tm_hour;
      const bool night=SleepState::inNightWindow(hour,kSleepStartHour,kSleepEndHour);
      if(gAmbient && !gBlanked && night && lv_disp_get_inactive_time(NULL)>kNightBlankIdleMs){
        gBlanked=true; portBacklight(false); }
      else if(gBlanked && !night){ renderAmbient(s); lv_refr_now(NULL); portBacklight(true); gBlanked=false; }  // rolled past 06:00 (or clock lost): repaint ambient, THEN light on (no flash)
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
