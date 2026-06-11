// screens.cpp — LVGL v9 widget construction (skeleton; NOT in the build, see
// ui/README.md and decision #38). Layout sized for 480x272 landscape.

#include "screens.h"

namespace dettson_ui {
namespace {

// Setpoint arcs are value-scaled by 10 (0.1 C resolution): 10.0-35.0 C -> 100-350.
constexpr int32_t kArcMin = 100;
constexpr int32_t kArcMax = 350;

lv_obj_t* makeStatusIcon(lv_obj_t* parent, const char* symbol) {
  lv_obj_t* icon = lv_label_create(parent);
  lv_label_set_text(icon, symbol);
  lv_obj_set_style_text_color(icon, lv_palette_main(LV_PALETTE_GREY), 0);
  return icon;
}

void buildHome(HomeScreen& h, lv_obj_t* parent) {
  h.root = parent;

  // --- dial: two concentric arcs, heat inner / cool outer, 270-degree sweep ---
  h.dial = lv_obj_create(parent);
  lv_obj_set_size(h.dial, 200, 200);
  lv_obj_align(h.dial, LV_ALIGN_LEFT_MID, 8, -10);
  lv_obj_set_style_bg_opa(h.dial, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(h.dial, 0, 0);

  h.coolArc = lv_arc_create(h.dial);
  lv_obj_set_size(h.coolArc, 196, 196);
  lv_obj_center(h.coolArc);
  lv_arc_set_rotation(h.coolArc, 135);
  lv_arc_set_bg_angles(h.coolArc, 0, 270);
  lv_arc_set_range(h.coolArc, kArcMin, kArcMax);
  lv_obj_set_style_arc_color(h.coolArc, lv_palette_main(LV_PALETTE_BLUE),
                             LV_PART_INDICATOR);

  h.heatArc = lv_arc_create(h.dial);
  lv_obj_set_size(h.heatArc, 156, 156);
  lv_obj_center(h.heatArc);
  lv_arc_set_rotation(h.heatArc, 135);
  lv_arc_set_bg_angles(h.heatArc, 0, 270);
  lv_arc_set_range(h.heatArc, kArcMin, kArcMax);
  lv_obj_set_style_arc_color(h.heatArc, lv_palette_main(LV_PALETTE_ORANGE),
                             LV_PART_INDICATOR);

  h.tempLabel = lv_label_create(h.dial);
  lv_label_set_text(h.tempLabel, "--.-");
  lv_obj_set_style_text_font(h.tempLabel, &lv_font_montserrat_28, 0);
  lv_obj_align(h.tempLabel, LV_ALIGN_CENTER, 0, -12);

  h.setpointLabel = lv_label_create(h.dial);
  lv_label_set_text(h.setpointLabel, "-- / --");
  lv_obj_align(h.setpointLabel, LV_ALIGN_CENTER, 0, 18);

  h.actionLabel = lv_label_create(h.dial);
  lv_label_set_text(h.actionLabel, "");
  lv_obj_align(h.actionLabel, LV_ALIGN_CENTER, 0, 40);

  // --- right column: mode + preset + telemetry ---
  static const char* kModeMap[] = {"Off", "Heat", "Cool", "Auto", "Em.Heat", ""};
  h.modeMatrix = lv_buttonmatrix_create(parent);
  lv_buttonmatrix_set_map(h.modeMatrix, kModeMap);
  lv_buttonmatrix_set_one_checked(h.modeMatrix, true);
  lv_obj_set_size(h.modeMatrix, 250, 46);
  lv_obj_align(h.modeMatrix, LV_ALIGN_TOP_RIGHT, -8, 30);

  static const char* kPresetMap[] = {"Home", "Away", "Sleep", ""};
  h.presetMatrix = lv_buttonmatrix_create(parent);
  lv_buttonmatrix_set_map(h.presetMatrix, kPresetMap);
  lv_obj_set_size(h.presetMatrix, 250, 40);
  lv_obj_align_to(h.presetMatrix, h.modeMatrix, LV_ALIGN_OUT_BOTTOM_MID, 0, 6);

  h.equipLabel = lv_label_create(parent);
  lv_label_set_text(h.equipLabel, "");
  lv_obj_align(h.equipLabel, LV_ALIGN_RIGHT_MID, -12, 28);

  h.outdoorLabel = lv_label_create(parent);
  lv_label_set_text(h.outdoorLabel, "OAT --");
  lv_obj_align(h.outdoorLabel, LV_ALIGN_RIGHT_MID, -12, 50);

  h.lockoutLabel = lv_label_create(parent);
  lv_label_set_text(h.lockoutLabel, "");
  lv_obj_add_flag(h.lockoutLabel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(h.lockoutLabel, LV_ALIGN_RIGHT_MID, -12, 72);

  // --- status strip (top) + alarm banner (bottom) ---
  h.wifiIcon = makeStatusIcon(parent, LV_SYMBOL_WIFI);
  lv_obj_align(h.wifiIcon, LV_ALIGN_TOP_RIGHT, -8, 4);
  h.mqttIcon = makeStatusIcon(parent, LV_SYMBOL_UPLOAD);
  lv_obj_align_to(h.mqttIcon, h.wifiIcon, LV_ALIGN_OUT_LEFT_MID, -10, 0);
  h.busIcon = makeStatusIcon(parent, LV_SYMBOL_USB);  // CT-485 link
  lv_obj_align_to(h.busIcon, h.mqttIcon, LV_ALIGN_OUT_LEFT_MID, -10, 0);

  h.alarmBanner = lv_label_create(parent);
  lv_label_set_text(h.alarmBanner, "");
  lv_obj_set_style_text_color(h.alarmBanner, lv_palette_main(LV_PALETTE_RED), 0);
  lv_obj_add_flag(h.alarmBanner, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(h.alarmBanner, LV_ALIGN_BOTTOM_MID, 0, -2);
}

void buildSensors(SensorsScreen& s, lv_obj_t* parent) {
  s.root = parent;
  s.table = lv_table_create(parent);
  lv_obj_set_size(s.table, LV_PCT(100), LV_PCT(100));
  lv_table_set_column_count(s.table, 6);
  lv_table_set_cell_value(s.table, 0, 0, "Sensor");
  lv_table_set_cell_value(s.table, 0, 1, "Temp");
  lv_table_set_cell_value(s.table, 0, 2, "Occ");
  lv_table_set_cell_value(s.table, 0, 3, "Age");
  lv_table_set_cell_value(s.table, 0, 4, "Fused");
  lv_table_set_cell_value(s.table, 0, 5, "OK");
}

void buildDiagnostics(DiagnosticsScreen& d, lv_obj_t* parent) {
  d.root = parent;
  d.alarmList = lv_list_create(parent);
  lv_obj_set_size(d.alarmList, LV_PCT(55), LV_PCT(100));
  lv_obj_align(d.alarmList, LV_ALIGN_LEFT_MID, 0, 0);

  d.healthTable = lv_table_create(parent);
  lv_obj_set_size(d.healthTable, LV_PCT(43), LV_PCT(100));
  lv_obj_align(d.healthTable, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_table_set_column_count(d.healthTable, 2);
}

void buildSettings(SettingsScreen& st, lv_obj_t* parent) {
  st.root = parent;
  st.deadbandLabel = lv_label_create(parent);
  lv_label_set_text(st.deadbandLabel, "Auto deadband: --");
  lv_obj_align(st.deadbandLabel, LV_ALIGN_TOP_LEFT, 8, 8);

  st.infoLabel = lv_label_create(parent);
  lv_label_set_text(st.infoLabel,
                    "Tunables are HA-editable MQTT numbers\n(docs/05); "
                    "this page is read-only fallback.");
  lv_obj_align(st.infoLabel, LV_ALIGN_TOP_LEFT, 8, 40);
}

}  // namespace

void screens_create(Screens& s, lv_obj_t* parent) {
  s.tabview = lv_tabview_create(parent);
  lv_tabview_set_tab_bar_size(s.tabview, 28);

  buildHome(s.home, lv_tabview_add_tab(s.tabview, "Home"));
  buildSensors(s.sensors, lv_tabview_add_tab(s.tabview, "Sensors"));
  buildDiagnostics(s.diagnostics, lv_tabview_add_tab(s.tabview, "Diag"));
  buildSettings(s.settings, lv_tabview_add_tab(s.tabview, "Setup"));
}

}  // namespace dettson_ui
