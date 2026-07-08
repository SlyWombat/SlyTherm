// remote_net_guard.cpp — see remote_net_guard.h.

#include "remote_net_guard.h"

#include <Arduino.h>
#include <lvgl.h>

#include <atomic>

#include "NetGuard.h"
#include "ui/ui_shared.h"   // palette + fonts (render side only)

namespace remote_net_guard {
namespace {

using slytherm_ui::setTxt;

// Bench builds may override the 5-min sustain for testing.
#ifndef SLYTHERM_NETGUARD_SUSTAIN_MS
#define SLYTHERM_NETGUARD_SUSTAIN_MS (5u * 60u * 1000u)
#endif

net_guard::Guard gGuard(SLYTHERM_NETGUARD_SUSTAIN_MS);  // loop-task owned

// Loop -> UI handoff.
std::atomic<uint8_t> gState{static_cast<uint8_t>(net_guard::State::kHealthy)};
std::atomic<uint32_t> gAttempts{0};

// UI-task-only widgets.
lv_obj_t* gPanel = nullptr;
lv_obj_t* gTitle = nullptr;
lv_obj_t* gDetail = nullptr;
lv_obj_t* gHint = nullptr;

void onReboot(lv_event_t*) { ESP.restart(); }

void buildPanel() {
  gPanel = lv_obj_create(lv_layer_top());  // floats above every screen
  lv_obj_set_size(gPanel, 800, 480);
  lv_obj_set_pos(gPanel, 0, 0);
  lv_obj_set_style_bg_color(gPanel, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(gPanel, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(gPanel, 0, 0);
  lv_obj_set_style_radius(gPanel, 0, 0);
  lv_obj_clear_flag(gPanel, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* warn = lv_label_create(gPanel);
  lv_label_set_text(warn, LV_SYMBOL_WARNING);
  lv_obj_set_style_text_font(warn, &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(warn, lv_color_hex(COL_WARN), 0);
  lv_obj_align(warn, LV_ALIGN_TOP_MID, 0, 70);

  gTitle = lv_label_create(gPanel);
  lv_obj_set_style_text_font(gTitle, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(gTitle, lv_color_hex(COL_INK), 0);
  lv_obj_align(gTitle, LV_ALIGN_TOP_MID, 0, 150);

  gDetail = lv_label_create(gPanel);
  lv_obj_set_style_text_font(gDetail, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gDetail, lv_color_hex(COL_MUTED), 0);
  lv_obj_align(gDetail, LV_ALIGN_TOP_MID, 0, 200);

  gHint = lv_label_create(gPanel);
  lv_obj_set_style_text_font(gHint, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(gHint, lv_color_hex(COL_WARN), 0);
  lv_obj_align(gHint, LV_ALIGN_TOP_MID, 0, 236);

  lv_obj_t* b = lv_btn_create(gPanel);
  lv_obj_set_size(b, 220, 60);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -60);
  lv_obj_set_style_bg_color(b, lv_color_hex(COL_RAISED), 0);
  lv_obj_add_event_cb(b, onReboot, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* bl = lv_label_create(b);
  lv_label_set_text(bl, LV_SYMBOL_REFRESH "  Reboot");
  lv_obj_center(bl);

  lv_obj_add_flag(gPanel, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

void feed(bool brokerUp, bool controllerUp, uint32_t attempts) {
  const net_guard::State s = gGuard.update(brokerUp, controllerUp, attempts, millis());
  gState.store(static_cast<uint8_t>(s));
  gAttempts.store(gGuard.outageAttempts());
}

void service() {
  const auto s = static_cast<net_guard::State>(gState.load());
  const bool block = s == net_guard::State::kNetworkUnavailable ||
                     s == net_guard::State::kControllerOffline;
  if (!block) {
    if (gPanel != nullptr && !lv_obj_has_flag(gPanel, LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_add_flag(gPanel, LV_OBJ_FLAG_HIDDEN);   // recovery: back to normal UI
      Serial.println("[guard] link recovered - panel cleared");
    }
    return;
  }
  if (gPanel == nullptr) buildPanel();

  const bool net = s == net_guard::State::kNetworkUnavailable;
  setTxt(gTitle, net ? "Network Unavailable" : "Controller Offline");
  char d[48];
  snprintf(d, sizeof(d), "Retrying (attempt %lu)",
           static_cast<unsigned long>(gAttempts.load()));
  setTxt(gDetail, d);
  setTxt(gHint, net ? "" : "Check Controller Power");

  if (lv_obj_has_flag(gPanel, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_clear_flag(gPanel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(gPanel);
    Serial.printf("[guard] BLOCKING: %s\n", net ? "network unavailable" : "controller offline");
  }
}

}  // namespace remote_net_guard
