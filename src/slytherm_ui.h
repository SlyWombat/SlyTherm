// slytherm_ui.h — LVGL wall-UI binding (issue #37). Renders the shared
// dettson::ui::UiModel and routes touch into its UiCommands. Owns the LVGL
// stack; runs entirely in the UI task. All UiModel access is serialized by the
// caller-provided mutex (the control task on core 1 also touches the model).
//
// Display/touch recipe is the validated stage-1 stack (LovyanGFX RGB panel,
// GT911 raw I2C @0x5D, pclk_idle_high=1). Colours come from docs/09.
#pragma once

#include "UiModel.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace slytherm_ui {

// Bring up the panel + touch + LVGL and build the screens. Call once from the
// UI task before service(). `model` and `mux` outlive the UI.
//
// reducedUi (issue #80): when the boot detected a reset-loop latch, build a
// MINIMAL safe screen (plain temp + mode + alarms, no chart/gradient/LISTEN/
// nav) instead of re-running the full UI that may have crashed. The panel stays
// usable; a "Restore full screen" button clears the NVS flag and reboots.
// firstRun (issue #82): no saved Wi-Fi -> boot a full-screen Welcome onboarding
// (big logo + "Let's Get Started" -> WiFi setup) instead of the empty Home;
// once connected the UI transitions to Home on its own.
void begin(dettson::ui::UiModel* model, SemaphoreHandle_t mux,
           bool reducedUi = false, bool firstRun = false);

// One service iteration (call ~5 ms): LVGL tick + timer, idle/ambient state,
// and render the latest model snapshot. Runs on the UI task only.
void service();

// #156: hand the retained slytherm/graph/system payload (JSON) to the System-tab
// trend chart. MQTT-task safe — parses ints into a buffer; service()/render (UI
// task) applies it to LVGL. Called from the control/MQTT task.
void ingestGraphSeries(const char* json);

}  // namespace slytherm_ui
