// FanCirculate.h — pure helpers for the fan "circulate" duty (issue #128).
//
// The Controller's control task drives fan_mode == circulate by running the
// blower for the first N minutes of each wall-clock hour at a fixed speed
// (docs/06 duty-cycled FAN_DEMAND, docs/13 §3). #128 promotes the minutes and
// speed from compile-time constants to runtime config the on-panel Fan sheet
// (and HA) can set; these helpers hold the clamp rules and the duty decision so
// they are host-testable and shared by the driver and the config glue.
//
// Pure C++17, NO Arduino.h — testable on the host, same rule as lib/PreCirculator.

#pragma once
#include <cstdint>

namespace dettson {
namespace fan {

// Field-confirmed CT-485 fan speeds (docs/02 §5a): the demand pipeline speaks
// PERCENT (setFanDemand takes pct, the wire byte is pct*2 downstream — Low
// 25 %=0x32, Med 50 %=0x64, High 75 %=0x96). The Fan sheet's Low/Med/High map
// onto exactly these three.
constexpr float kSpeedLowPct  = 25.0f;
constexpr float kSpeedMedPct  = 50.0f;
constexpr float kSpeedHighPct = 75.0f;

// The circulate window can never exceed the hour it duty-cycles.
constexpr uint32_t kCirculateMinPerHourMax = 60u;

// Clamp a requested minutes-per-hour to [0, 60]. 0 is legal (== never
// circulates this hour; effectively idle until the mode changes).
uint32_t clampCirculateMinPerHour(long minPerHour);

// Snap an arbitrary speed percent onto the nearest field-confirmed speed
// (Low/Med/High). A panel tap already sends one of the three; an MQTT writer
// might send anything, so snap defensively rather than pass a raw pct to the
// bus.
float snapCirculatePct(float pct);

// The fan pct the circulate driver should request at wall-clock second `nowS`.
// Runs at `pct` for the first `minPerHour*60` seconds of each hour, MINUS the
// pre-circulation credit already spent this hour (#53/#142: pre-run seconds
// count toward the circulate duty, never on top of it), else 0. Reproduces the
// former inline decision in main_thermostat.cpp exactly.
float circulateRequestPct(uint32_t nowS, uint32_t minPerHour, float pct,
                          uint32_t creditS);

}  // namespace fan
}  // namespace dettson
