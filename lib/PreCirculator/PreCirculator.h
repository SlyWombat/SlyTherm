// PreCirculator.h — blower-first pre-circulation (issue #142, docs/13 §3+§8).
//
// When #141's crossing prediction says a heat/cool call is imminent (the
// glue projects the fused-temp slope through RecoveryEstimator::crossingBias
// with THIS module's lead as the horizon), run the blower LOW for the lead
// window ahead of the call: low continuous circulation destratifies the
// space and gives SensorFusion a truthful whole-space reading BEFORE the
// arbiter commits to a stage (docs/13 §3). The fan level is the lowest
// field-confirmed CT-485 speed (FAN_DEMAND 0x66, Low = 25 % / wire 0x32,
// docs/02 §5a) — an ECM at low speed costs single-digit watts.
//
// Season policy (docs/13 §8, the #144 literature verdict): heat-side
// pre-runs are ON by default; COOL-side pre-runs are OFF by default — in
// the humid cooling season air moved over the coil between cycles
// re-evaporates the previous cycle's held condensate (~2 lb; effective
// SHR -> 1.0 at part load), so a pre-run before a cool call is a latent
// penalty paid 1:1 for its sensible "gain". Enabling cool-side pre-runs is
// an explicit owner decision for dry conditions only.
//
// Post-heat blower tail: deliberately NOT implemented. Verified 2026-07-11
// (#142): the furnace control owns fan-off dissipation internally — the OEM
// stat's installer menus expose furnace-side AC/HP ON/OFF fan delays
// (5-120 s / 5-240 s, R02P032 System menu) and the heat-side blower profile
// belongs to the IFC entirely (docs/02 §5a: FAN_DEMAND is not used during
// heat/cool; airflow is mapped to fire rate internally). A bus-side tail
// would stack on the equipment's own delay. Cooling fan-off delay stays
// 0 s per §8.
//
// Authority rules (docs/04 §3): the output is a fan-channel REQUEST the
// glue merges into the normal DemandRequest — it flows through
// SafetySupervisor::filterRequest and DemandArbiter like every other
// demand; this module has NO demand authority. A real call always
// overrides the pre-run (the equipment's own airflow takes over), and the
// pre-run cancels the moment the prediction evaporates.
//
// Duty accounting (docs/13 §3 / issue #53): pre-run seconds are tallied
// per wall-clock hour and exposed via dutyCreditS() so the fan-circulate
// duty window shrinks by what the pre-runs already ran — pre-circulation
// counts toward the circulate duty, never on top of it.
//
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic seconds).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

class PreCirculator {
 public:
  struct Config {
    bool     heatEnabled = kBlowerFirstHeatEnabledDefault;  // §3: on
    bool     coolEnabled = kBlowerFirstCoolEnabledDefault;  // §8: off (latent penalty)
    float    fanPct      = kBlowerFirstFanPct;              // CT-485 fan Low
    uint32_t leadS       = kBlowerFirstLeadS;               // 1-3 min pre-run lead
    uint32_t maxRunS     = kBlowerFirstMaxRunS;             // hovering-prediction cap
  };

  struct Inputs {
    // Crossing predicted within the lead window (glue: crossingBias with
    // horizon = config().leadS on the heat/cool approach respectively).
    bool heatPredicted = false;
    bool coolPredicted = false;
    // Any real thermal call or nonzero equipment request — ALWAYS overrides:
    // once the stage engages, the IFC owns airflow (docs/02 §5a).
    bool callActive    = false;
  };

  PreCirculator() {}
  explicit PreCirculator(const Config& cfg) : cfg_(cfg) {}

  // Control-tick update. Returns the fan REQUEST percent: cfg.fanPct while
  // a pre-run is active, else 0. The caller merges it into the normal
  // DemandRequest.fanPct (never lowering an explicit fan-on).
  float update(const Inputs& in, uint32_t nowS);

  bool active() const { return state_ == State::kPreRun; }

  // Pre-run seconds accumulated in the CURRENT wall-clock hour bucket
  // (nowS / 3600); 0 once the hour rolls. The glue subtracts this from the
  // circulate mode's minutes-per-hour window (#53 duty ledger seam — when
  // the full #53 engine lands, this feeds the same per-hour ledger, see
  // #128 "predictive pre-runs count toward the same per-hour ledger").
  uint32_t dutyCreditS(uint32_t nowS) const {
    return (nowS / 3600u == bucketHour_) ? bucketCreditS_ : 0u;
  }

  const Config& config() const { return cfg_; }
  void setConfig(const Config& cfg) { cfg_ = cfg; }

 private:
  // kIdle  — no pre-run; arms when an enabled side predicts and no call runs.
  // kPreRun— fan low; ends on call engage (handoff), prediction loss
  //          (cancel), or the maxRunS cap (kSpent).
  // kSpent — cap reached with the prediction still hovering: stay quiet
  //          until the prediction drops or a call happens, so a stuck
  //          near-crossing hover can't run the fan indefinitely.
  enum class State : uint8_t { kIdle, kPreRun, kSpent };

  Config   cfg_;
  State    state_         = State::kIdle;
  uint32_t runStartS_     = 0;
  uint32_t lastUpdateS_   = 0;  // credit accumulation anchor while running
  uint32_t bucketHour_    = 0;
  uint32_t bucketCreditS_ = 0;
};

}  // namespace dettson
