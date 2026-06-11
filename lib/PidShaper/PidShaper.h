// PidShaper.h — gas-heat PID, the ONLY surviving PID in the design (docs/05:
// PidController "survives only as the GasShaper input"; HP paths are
// staged/slew-limited, never PID-driven).
//
// Output is clamped to [0..100] BEFORE GasShaper — the shaper, not the PID,
// owns the 40% floor snap. Per-mode gain sets are selectable at runtime;
// changing mode resets the integrator and derivative history (docs/05).
// During defrost tempering the controller is frozen: output held, no
// integration (docs/05 DEFROST_TEMPER — tempering demand itself is the fixed
// kDefrostTemperHeatPct, never this PID). An invalid-input flag forces
// output 0 and clears state (docs/04 §2: invalid input -> demand 0).
//
// Anti-windup is conditional integration: the integral only accepts updates
// that do not push the unsaturated output further past a clamp.
//
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic seconds).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

class PidShaper {
 public:
  struct Gains {
    float kp = 25.0f;  // %/degC
    float ki = 0.02f;  // %/(degC*s)
    float kd = 0.0f;   // %/(degC/s), applied to measurement (no setpoint kick)
  };

  // Gain-set slots; indices are owned by the caller (e.g. HEAT vs
  // EMERGENCY_HEAT). All slots default to Gains{} until setGains().
  static constexpr uint8_t kMaxModes = 4;

  struct Config {
    Gains gains[kMaxModes] = {};
    float outMinPct = 0.0f;
    float outMaxPct = 100.0f;
  };

  PidShaper() {}
  explicit PidShaper(const Config& cfg) : cfg_(cfg) {}

  void setGains(uint8_t mode, const Gains& g);
  void selectMode(uint8_t mode);  // change -> integrator + derivative reset
  uint8_t mode() const { return mode_; }

  // inputValid=false -> returns 0 and resets state (no output on invalid
  // input, no derivative/integral kick on recovery).
  // freeze=true (defrost tempering) -> holds the last output, no integration.
  float update(float setpointC, float measuredC, bool inputValid, bool freeze,
               uint32_t nowS);

  float output() const { return out_; }
  void reset();  // full state reset; output back to 0 (boot = no demand)

 private:
  static uint8_t clampMode(uint8_t m) { return m < kMaxModes ? m : kMaxModes - 1; }

  Config   cfg_;
  uint8_t  mode_     = 0;
  float    integ_    = 0.0f;
  float    out_      = 0.0f;
  float    lastMeas_ = 0.0f;
  uint32_t lastS_    = 0;
  bool     hasLast_  = false;
};

}  // namespace dettson
