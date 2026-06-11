// DemandArbiter.h — THE single demand emission point (docs/04 §3).
//
// Every demand the system ever emits (bus message or relay state) passes
// through set(); no other module may touch the actuator layer. Invariants
// (docs/04 §2 demand-conflict + gas/compressor rows) are re-checked on every
// call:
//   * heat-family (gasHeat, hpHeat, defrostTemper) and cool simultaneously
//     nonzero -> all channels forced 0 + invariant alarm LATCHED (manual clear).
//   * gasHeat and hpHeat simultaneously nonzero -> same trip. The ONLY
//     sanctioned overlap is defrostTemper alongside hp operation (docs/04 §2).
//     gasHeat + defrostTemper together is two gas channels at once — an
//     upstream logic bug — and trips as well.
//   * changeover sequencing: a channel may go nonzero only after the opposite
//     family has been emitted-zero for its injected min-dwell (docs/04 §2:
//     dwell >= compressor min-off). Dwell holds suppress, they do not latch.
//
// Boot state is all-zero, and both dwell timers start from boot: prior
// activity is unknown, so assume the worst (docs/04 §3 boot validation).
// Invalid (non-finite/negative) input -> all channels 0 + input alarm flag.
//
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic seconds).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

struct DemandRequest {
  float gasHeatPct       = 0.0f;
  float hpHeatPct        = 0.0f;
  float coolPct          = 0.0f;
  float fanPct           = 0.0f;
  float defrostTemperPct = 0.0f;
};

// Consumed by the actuator layer (Ct485Thermostat or RelayOutputs).
struct DemandSet {
  float gasHeatPct       = 0.0f;
  float hpHeatPct        = 0.0f;
  float coolPct          = 0.0f;
  float fanPct           = 0.0f;
  float defrostTemperPct = 0.0f;

  bool anyNonzero() const {
    return gasHeatPct > 0.0f || hpHeatPct > 0.0f || coolPct > 0.0f ||
           fanPct > 0.0f || defrostTemperPct > 0.0f;
  }
};

class DemandArbiter {
 public:
  struct Config {
    uint32_t heatToCoolDwellS = kCompressorMinOffS;  // cool may start this long after heat-family last nonzero
    uint32_t coolToHeatDwellS = kCompressorMinOffS;  // heat-family may start this long after cool last nonzero
  };

  // bootNowS: current injected time at construction; both families are
  // treated as active-at-boot so each direction waits its full dwell.
  explicit DemandArbiter(uint32_t bootNowS) : DemandArbiter(bootNowS, Config{}) {}
  DemandArbiter(uint32_t bootNowS, const Config& cfg);

  // Revalidates everything and returns the emitted set. Fan has no opposite
  // channel: it passes through dwell holds but is zeroed by any trip.
  DemandSet set(const DemandRequest& req, uint32_t nowS);

  const DemandSet& current() const { return out_; }

  bool invariantAlarm() const { return invariantAlarm_; }  // latched
  void clearInvariantAlarm() { invariantAlarm_ = false; }  // deliberate manual clear (docs/04 §2)
  bool inputAlarm() const { return inputAlarm_; }          // reflects the most recent set()
  bool heatHeldByDwell() const { return heatHeld_; }
  bool coolHeldByDwell() const { return coolHeld_; }

 private:
  Config    cfg_;
  DemandSet out_;
  bool      invariantAlarm_ = false;
  bool      inputAlarm_     = false;
  bool      heatHeld_       = false;
  bool      coolHeld_       = false;
  uint32_t  lastHeatActiveS_;  // last time the heat family was EMITTED nonzero
  uint32_t  lastCoolActiveS_;
};

}  // namespace dettson
