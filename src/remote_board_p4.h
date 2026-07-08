// P4 board glue: ST7701 MIPI-DSI panel + GT911 touch (issue #97).
// Board = GUITION JC4880P443C_I_W (4.3" 480x800 portrait). NOT the bare
// JC-ESP32P4-M3-DEV carrier -- confirmed via the vendor's AliExpress listing
// (item 1005009618259341) + a cross-checked ESP32-MiniWebRadio driver
// (schreibfaul1/ESP32-MiniWebRadio, lib/tftLib/tft_dsi.cpp) for this exact SKU.
#pragma once

namespace remote_board {

// Powers the DSI PHY, resets + initializes the ST7701 panel, brings up the
// GT911 touch controller, and starts LVGL (single internal-RAM line buffer,
// matching the Controller's validated wall-UI recipe). Call once from setup().
void begin();

// Services LVGL's tick/timer handler and polls touch. Call every loop().
void loop();

// Init-result flags, for the caller's periodic heartbeat print -- avoids
// depending on catching begin()'s one-shot boot-time serial output.
bool panelOk();
bool touchOk();

}  // namespace remote_board
