// ui_port.h — the board-port contract for the shared wall UI (issue #114).
// Everything in src/ui/ except ui_port_*.cpp is portable LVGL code; the port
// owns the display + touch hardware and presents a LOGICAL 800x480 landscape
// LVGL display on any panel (rotating in the driver/port if the glass is
// portrait, as on the Remote's 480x800 ST7701).
//
// Ports: ui_port_s3.cpp — Controller (Waveshare S3 4.3B: LovyanGFX RGB panel,
//        CH422G backlight/reset expander, GT911 raw I2C @0x5D).
//        ui_port_p4.cpp — Remote (Guition P4: ST7701 MIPI-DSI, GT911), lives
//        on feature/remote-p4.
//
// Contract details the portable code relies on:
//  - portInit() fully brings up panel + touch, calls lv_init(), and registers
//    the LVGL display (800x480) and pointer input device. Called exactly once,
//    from slytherm_ui::begin(), on the UI task.
//  - The port's touch read callback must call uiNoteTouch() on each press
//    EDGE (issue #90 sleep-state wake) — see the S3 port's touchCb.
//  - portBacklight(false) must leave touch alive (the #86 night deep-blank
//    wakes on touch with the light off).
#pragma once

namespace slytherm_ui {

// Bring up display + touch + LVGL drivers. Returns true if the panel came up
// (touch health is reported separately — a dead touch is degraded, not fatal).
bool portInit();

// Panel backlight on/off (no PWM contract — both current boards are binary).
void portBacklight(bool on);

// True if the touch controller ACKed at init (Diag/telemetry).
bool portTouchOk();

}  // namespace slytherm_ui
