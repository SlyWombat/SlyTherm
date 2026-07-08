// remote_net_guard.h — #109 degraded-UX glue: runs lib/NetGuard's state
// machine on the link signals and raises/clears the full-screen blocking
// panel ("Network Unavailable — Retrying (attempt N)" / "Controller Offline
// — Check Controller Power", each with [Reboot]).
//
// Thread split: feed() runs on the loop task (owns the link modules);
// service() runs on the UI TASK ONLY (owns LVGL) and renders the panel on
// lv_layer_top() — floating above whatever screen the shared UI shows, so
// no shared slytherm_ui/service() changes are needed. State crosses the
// tasks through atomics.
#pragma once

#include <cstdint>

namespace remote_net_guard {

// Loop task, ~1 Hz: current link signals + the cumulative retry counters.
void feed(bool brokerUp, bool controllerUp, uint32_t attempts);

// UI task, once per service iteration (after slytherm_ui::service()).
void service();

}  // namespace remote_net_guard
