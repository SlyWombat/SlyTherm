// binding.cpp — DisplayState -> widgets and widget events -> UiCommands.
// (Skeleton; NOT in the build — decision #38 picks ESPHome or PlatformIO glue.
// Either way this file is the shape of the binding: render only dirty groups,
// route input only through UiCommands. See ui/README.md.)

#include <cstdio>

#include "UiModel.h"   // lib/UiModel — the part that outlives decision #38
#include "screens.h"

namespace dettson_ui {
namespace {

using dettson::ui::DirtyGroup;
using dettson::ui::DisplayState;
using dettson::ui::HvacAction;
using dettson::ui::Preset;
using dettson::ui::SetpointSide;
using dettson::ui::UiCommands;
using dettson::ui::UiModel;
using dettson::ui::UserMode;

constexpr float kArcScale = 10.0f;  // arc value = degrees C * 10

struct Binding {
  Screens*    screens = nullptr;
  UiModel*    model   = nullptr;  // render side reads state()/dirty()
  UiCommands* cmds    = nullptr;  // input side; same object, but ONLY this
                                  // interface — never control internals
};
Binding g_binding;  // single display

// ---------- widget events -> UiCommands (the only write path) ----------

void onArcReleased(lv_event_t* e) {
  auto* b = static_cast<Binding*>(lv_event_get_user_data(e));
  lv_obj_t* arc = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const bool isHeat = (arc == b->screens->home.heatArc);
  const SetpointSide side = isHeat ? SetpointSide::kHeat : SetpointSide::kCool;
  const float current = isHeat ? b->model->state().heatSetpointC
                               : b->model->state().coolSetpointC;
  const float target = static_cast<float>(lv_arc_get_value(arc)) / kArcScale;
  // Commands are deltas: the model applies deadband clamp+push and enqueues
  // a UiIntent; the control task re-validates. No direct state write here.
  b->cmds->adjustSetpoint(side, target - current);
}

void onModeChanged(lv_event_t* e) {
  auto* b = static_cast<Binding*>(lv_event_get_user_data(e));
  lv_obj_t* bm = static_cast<lv_obj_t*>(lv_event_get_target(e));
  static constexpr UserMode kOrder[] = {UserMode::kOff, UserMode::kHeat,
                                        UserMode::kCool, UserMode::kAuto,
                                        UserMode::kEmergencyHeat};
  const uint32_t sel = lv_buttonmatrix_get_selected_button(bm);
  if (sel < 5) b->cmds->setMode(kOrder[sel]);
}

void onPresetChanged(lv_event_t* e) {
  auto* b = static_cast<Binding*>(lv_event_get_user_data(e));
  lv_obj_t* bm = static_cast<lv_obj_t*>(lv_event_get_target(e));
  static constexpr Preset kOrder[] = {Preset::kHome, Preset::kAway, Preset::kSleep};
  const uint32_t sel = lv_buttonmatrix_get_selected_button(bm);
  if (sel < 3) b->cmds->setPreset(kOrder[sel]);
}

// ---------- DisplayState -> widgets, per dirty group ----------

const char* actionText(HvacAction a) {
  switch (a) {
    case HvacAction::kHeating:    return "Heating";
    case HvacAction::kCooling:    return "Cooling";
    case HvacAction::kFanOnly:    return "Fan";
    case HvacAction::kDefrosting: return "DEFROST";
    default:                      return "Idle";
  }
}

void renderHome(const HomeScreen& h, const DisplayState& s, uint16_t dirty) {
  if (dirty & dettson::ui::kDirtyTemp) {
    if (s.fusedTempValid) {
      lv_label_set_text_fmt(h.tempLabel, "%.1f°", static_cast<double>(s.fusedTempC));
    } else {
      lv_label_set_text(h.tempLabel, "--.-");  // invalid input renders as absent
    }
  }
  if (dirty & dettson::ui::kDirtySetpoints) {
    lv_arc_set_value(h.heatArc, static_cast<int32_t>(s.heatSetpointC * kArcScale));
    lv_arc_set_value(h.coolArc, static_cast<int32_t>(s.coolSetpointC * kArcScale));
    lv_label_set_text_fmt(h.setpointLabel, "%.1f / %.1f",
                          static_cast<double>(s.heatSetpointC),
                          static_cast<double>(s.coolSetpointC));
  }
  if (dirty & dettson::ui::kDirtyMode) {
    lv_buttonmatrix_set_selected_button(h.modeMatrix,
                                        static_cast<uint32_t>(s.mode));
  }
  if (dirty & dettson::ui::kDirtyAction) {
    lv_label_set_text(h.actionLabel, actionText(s.action));
  }
  if (dirty & (dettson::ui::kDirtyEquipment | dettson::ui::kDirtyGasMod)) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%s%s%s%s",
                  (s.activeEquipment & dettson::ui::kEquipGas) ? "GAS " : "",
                  (s.activeEquipment & (dettson::ui::kEquipHpHeat |
                                        dettson::ui::kEquipHpCool)) ? "HP " : "",
                  (s.activeEquipment & dettson::ui::kEquipAux) ? "AUX " : "",
                  (s.activeEquipment & dettson::ui::kEquipFan) ? "FAN" : "");
    lv_label_set_text(h.equipLabel, buf);
    if (s.activeEquipment & dettson::ui::kEquipGas) {
      lv_label_set_text_fmt(h.equipLabel, "GAS %d%%",
                            static_cast<int>(s.gasModulationPct));
    }
  }
  if (dirty & dettson::ui::kDirtyOutdoor) {
    if (s.outdoorValid) {
      static const char* kRung[] = {"?", "bus", "wired", "wx"};
      lv_label_set_text_fmt(h.outdoorLabel, "OAT %.1f° (%s)",
                            static_cast<double>(s.outdoorTempC),
                            kRung[static_cast<uint8_t>(s.outdoorSource)]);
    } else {
      lv_label_set_text(h.outdoorLabel, "OAT --");
    }
  }
  if (dirty & dettson::ui::kDirtyLockout) {
    if (s.compressorLockoutRemainS > 0) {
      lv_label_set_text_fmt(h.lockoutLabel, "Compressor wait %us",
                            static_cast<unsigned>(s.compressorLockoutRemainS));
      lv_obj_remove_flag(h.lockoutLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(h.lockoutLabel, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (dirty & dettson::ui::kDirtyAlarms) {
    if (s.alarmCount > 0) {
      lv_label_set_text_fmt(h.alarmBanner, "%s%s",
                            s.alarms[s.alarmCount - 1].text,
                            s.alarmsDropped ? " (+more)" : "");
      lv_obj_remove_flag(h.alarmBanner, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(h.alarmBanner, LV_OBJ_FLAG_HIDDEN);
    }
  }
  if (dirty & dettson::ui::kDirtyHealth) {
    lv_obj_set_style_text_color(
        h.wifiIcon,
        lv_palette_main(s.wifiOk ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    lv_obj_set_style_text_color(
        h.mqttIcon,
        lv_palette_main(s.mqttOk ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    lv_obj_set_style_text_color(
        h.busIcon,
        lv_palette_main(s.busOk ? LV_PALETTE_GREEN : LV_PALETTE_RED), 0);
    if (s.degradedMode) {
      lv_label_set_text(h.alarmBanner, "DEGRADED MODE (fallback sensor)");
      lv_obj_remove_flag(h.alarmBanner, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void renderSensors(const SensorsScreen& sc, const DisplayState& s) {
  lv_table_set_row_count(sc.table, s.sensorCount + 1);
  for (uint8_t i = 0; i < s.sensorCount; ++i) {
    const auto& r = s.sensors[i];
    const uint32_t row = i + 1;
    char buf[16];
    lv_table_set_cell_value(sc.table, row, 0, r.name);
    std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(r.tempC));
    lv_table_set_cell_value(sc.table, row, 1, buf);
    lv_table_set_cell_value(sc.table, row, 2, r.occupied ? "yes" : "");
    std::snprintf(buf, sizeof(buf), "%us", static_cast<unsigned>(r.ageS));
    lv_table_set_cell_value(sc.table, row, 3, buf);
    lv_table_set_cell_value(sc.table, row, 4, r.participating ? "yes" : "no");
    lv_table_set_cell_value(sc.table, row, 5, r.healthy ? LV_SYMBOL_OK
                                                        : LV_SYMBOL_WARNING);
  }
}

void renderDiagnostics(const DiagnosticsScreen& d, const DisplayState& s) {
  lv_obj_clean(d.alarmList);
  for (uint8_t i = 0; i < s.alarmCount; ++i) {
    lv_list_add_text(d.alarmList, s.alarms[i].text);
  }
  if (s.alarmsDropped) lv_list_add_text(d.alarmList, "(older alarms dropped)");
}

}  // namespace

// Wire events once after screens_create(). `model` serves both roles but the
// event handlers hold it only as UiCommands — input cannot bypass the intent
// queue (docs/04 §1c: UI has no demand authority).
void binding_attach(Screens& screens, UiModel& model) {
  g_binding.screens = &screens;
  g_binding.model = &model;
  g_binding.cmds = &model;

  lv_obj_add_event_cb(screens.home.heatArc, onArcReleased, LV_EVENT_RELEASED,
                      &g_binding);
  lv_obj_add_event_cb(screens.home.coolArc, onArcReleased, LV_EVENT_RELEASED,
                      &g_binding);
  lv_obj_add_event_cb(screens.home.modeMatrix, onModeChanged,
                      LV_EVENT_VALUE_CHANGED, &g_binding);
  lv_obj_add_event_cb(screens.home.presetMatrix, onPresetChanged,
                      LV_EVENT_VALUE_CHANGED, &g_binding);
}

// Call from the UI task loop (~10 Hz is plenty): renders ONLY dirty groups.
void binding_render() {
  UiModel& m = *g_binding.model;
  const uint16_t dirty = m.dirty();
  if (dirty == 0) return;
  const DisplayState& s = m.state();

  renderHome(g_binding.screens->home, s, dirty);
  if (dirty & dettson::ui::kDirtySensors) {
    renderSensors(g_binding.screens->sensors, s);
  }
  if (dirty & dettson::ui::kDirtyAlarms) {
    renderDiagnostics(g_binding.screens->diagnostics, s);
  }
  m.clearDirty(dirty);  // clear exactly what was rendered
}

}  // namespace dettson_ui
