// screens.h — LVGL v9 screen skeleton for the Dettson wall thermostat.
//
// NOT COMPILED YET: decision #38 (ESPHome+LVGL vs custom PlatformIO+LVGL) is
// open; this file is written against the plain LVGL v9 C API so it works under
// either binding. See ui/README.md for how it gets wired into a build later.
//
// Target: Guition JC4827W543C — 480x272 IPS, NV3041A over QSPI, GT911 touch.
//
// All control coupling goes through dettson::ui::DisplayState (render) and
// dettson::ui::UiCommands (input) from lib/UiModel — no direct control access.

#pragma once

#include "lvgl.h"

namespace dettson_ui {

// ---- Home: dial with dual-setpoint arc + mode row + status strip ----
struct HomeScreen {
  lv_obj_t* root = nullptr;
  // Dial: background track + two indicator arcs sharing the same sweep.
  lv_obj_t* dial      = nullptr;  // outer container
  lv_obj_t* heatArc   = nullptr;  // knob adjusts heat setpoint
  lv_obj_t* coolArc   = nullptr;  // knob adjusts cool setpoint
  lv_obj_t* tempLabel = nullptr;  // big fused indoor temp (or "--.-" if invalid)
  lv_obj_t* setpointLabel = nullptr;  // "18.0 / 24.0" dual setpoints
  lv_obj_t* actionLabel   = nullptr;  // Idle/Heating/Cooling/Fan/DEFROST
  lv_obj_t* equipLabel    = nullptr;  // gas % / HP / aux glyphs
  lv_obj_t* lockoutLabel  = nullptr;  // compressor lockout countdown (hidden when 0)
  lv_obj_t* outdoorLabel  = nullptr;  // OAT + source rung glyph
  lv_obj_t* modeMatrix    = nullptr;  // OFF / HEAT / COOL / AUTO / EM HEAT
  lv_obj_t* presetMatrix  = nullptr;  // home / away / sleep
  lv_obj_t* alarmBanner   = nullptr;  // newest alarm + degraded-mode banner
  lv_obj_t* wifiIcon = nullptr;
  lv_obj_t* mqttIcon = nullptr;
  lv_obj_t* busIcon  = nullptr;
};

// ---- Sensors: one row per fused participant ----
struct SensorsScreen {
  lv_obj_t* root  = nullptr;
  lv_obj_t* table = nullptr;  // cols: name | temp | occ | age | in-fusion | health
};

// ---- Diagnostics: alarms + link/bus health + equipment detail ----
struct DiagnosticsScreen {
  lv_obj_t* root       = nullptr;
  lv_obj_t* alarmList  = nullptr;
  lv_obj_t* healthTable = nullptr;  // wifi/mqtt/bus, degraded flag, lockout, gas %
};

// ---- Settings: read-mostly; tunables live in HA, this is local fallback ----
struct SettingsScreen {
  lv_obj_t* root = nullptr;
  lv_obj_t* deadbandLabel = nullptr;  // shows active min setpoint delta
  lv_obj_t* infoLabel     = nullptr;  // fw version, node id, decision #38 note
};

struct Screens {
  lv_obj_t* tabview = nullptr;
  HomeScreen        home;
  SensorsScreen     sensors;
  DiagnosticsScreen diagnostics;
  SettingsScreen    settings;
};

// Builds all screens under `parent` (pass lv_screen_active()). Pure widget
// construction: no model access, no event wiring — binding.cpp does that.
void screens_create(Screens& s, lv_obj_t* parent);

}  // namespace dettson_ui
