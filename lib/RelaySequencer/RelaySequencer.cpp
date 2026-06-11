#include "RelaySequencer.h"

namespace dettson {

namespace {
bool coilsDiffer(const RelayOutputs& a, const RelayOutputs& b) {
  // Only actual contact states count as a transition; flags are free.
  return a.y1 != b.y1 || a.y2 != b.y2 || a.ob != b.ob || a.g != b.g;
}
}  // namespace

const RelayOutputs& RelaySequencer::update(const Inputs& in, uint32_t nowS,
                                           uint32_t nowMs) {
  const bool wantHpHeat = in.demand.hpHeatPct > 0.0f;
  const bool wantCool   = in.demand.coolPct > 0.0f;
  // DemandArbiter guarantees mutual exclusion; both nonzero here is an
  // upstream fault -> no compressor demand (docs/04 §2 demand-conflict row).
  const bool wantY = (wantHpHeat != wantCool);

  // Desired valve position: demand wins; mode pre-positions when idle.
  // Energized = HEATING (kObEnergizedIsHeat).
  bool obDesired = out_.ob;
  if (wantY) {
    obDesired = wantHpHeat;
  } else if (in.obPreposition == ObPreposition::kHeat) {
    obDesired = true;
  } else if (in.obPreposition == ObPreposition::kCool) {
    obDesired = false;
  }

  RelayOutputs next = out_;
  if (obDesired != out_.ob) {
    // docs/04 §2 O/B row: flip only with the compressor proven idle. Y drops
    // first; the flip then waits for the injected min-off gate; Y (re)closes
    // on a later update once the valve is in position.
    next.y1 = false;
    if (!out_.y1 && gate_.compressorIdle(nowS)) next.ob = obDesired;
  } else {
    next.y1 = wantY;
  }
  next.y2 = next.y1 && in.stage2;
  next.g  = in.demand.fanPct > 0.0f || next.y1;

  if (coilsDiffer(next, out_)) {
    if (spaced_ && (nowMs - lastChangeMs_) < cfg_.minTransitionMs) {
      // Defer: hold previous contact states this cycle (recomputed next call).
      out_.requiresBlowerProof = out_.y1;
      out_.defrostActive = in.defrostSense;
      return out_;
    }
    lastChangeMs_ = nowMs;
    spaced_ = true;
  }

  next.requiresBlowerProof = next.y1;
  next.defrostActive = in.defrostSense;
  out_ = next;
  return out_;
}

void RelaySequencer::goSilent() {
  // Immediate, unconditional, never spaced (docs/04 §1b). Spacing history is
  // kept so a re-energize right after still respects the chatter guard.
  out_ = RelayOutputs{};
}

}  // namespace dettson
