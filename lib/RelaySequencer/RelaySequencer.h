// RelaySequencer.h — Case B relay sequencing (issue #35 logic half;
// docs/03 §7, docs/04 §2 relay rows).
//
// Pure sequencing between DemandArbiter's DemandSet and the GPIO glue. It
// computes DESIRED {Y1, Y2, O/B, G} states; the glue owns the pins, the
// hardware interlocks (watchdog coil-cut, float switch) and the sense-side
// checks. Rules enforced here:
//   * O/B changes ONLY with the compressor proven idle: own Y output off AND
//     the injected gate confirms the min-off guard (kCompressorMinOffS) has
//     been served — same injected-gate pattern as HpRelayShaper. A demand that
//     needs the opposite valve position drops Y first and waits.
//   * O/B energized = HEATING (kObEnergizedIsHeat, Gree B convention).
//   * G follows fan demand and is FORCED on with any Y. Blower-proven is a
//     SENSE-side interlock: this module raises requiresBlowerProof with Y and
//     the caller drops Y if proof is absent (docs/04 §2 coil-freeze row).
//   * Y2 only ever with Y1.
//   * Successive output transitions are spaced >= kRelayMinTransitionMs so
//     the glue never chatters contacts. A deferred change costs at most one
//     spacing window; the UNBOUNDED fail path is goSilent()/the hardware
//     watchdog, which are never spaced.
//   * All outputs OFF at construction and after goSilent() (docs/04 §1b).
//
// Boundary — defrost: the D-wire sense input is passed through as
// defrostActive for observation only; the ODU owns its own defrost cycle and
// these outputs are not altered by it. Defrost TEMPERING (W/gas) is routed by
// DualFuelArbiter onto DemandArbiter's defrost channel and reaches the furnace
// via the gas actuator path — never through this module, which owns only the
// 24 V compressor-side contacts.
//
// Pure C++17, no Arduino; time injected (uint32_t nowS / nowMs, monotonic).

#pragma once
#include <cstdint>
#include "DemandArbiter.h"
#include "DettsonConfig.h"

namespace dettson {

// Desired logic-level relay states. true = relay energized (contacts closed).
// ob: energized = HEATING position per kObEnergizedIsHeat — the glue must
// apply that constant if the installed valve convention ever differs.
struct RelayOutputs {
  bool y1 = false;
  bool y2 = false;
  bool ob = false;
  bool g  = false;
  bool requiresBlowerProof = false;  // raised with Y1; sense-side interlock cue
  bool defrostActive       = false;  // D-wire sense passthrough, not an output
};

// Minimal idle gate so the sequencer stays decoupled from CompressorGuard's
// concrete API (HpRelayShaper pattern); firmware adapts the guard to this.
// Must return true ONLY when the compressor has been off >= the min-off guard
// (kCompressorMinOffS) — docs/04 §2: O/B changes only with compressor idle.
class CompressorIdleGate {
 public:
  virtual ~CompressorIdleGate() = default;
  virtual bool compressorIdle(uint32_t nowS) = 0;
};

// Idle O/B pre-positioning from the system mode, so the valve flips unloaded
// before a call rather than in front of one. kHold = leave it where it is.
enum class ObPreposition : uint8_t { kHold, kHeat, kCool };

class RelaySequencer {
 public:
  struct Inputs {
    DemandSet demand;                                  // from DemandArbiter (sole emitter)
    ObPreposition obPreposition = ObPreposition::kHold;
    bool stage2       = false;  // upstream staging decision; honored only with Y1
    bool defrostSense = false;  // D-wire opto input
  };

  struct Config {
    uint32_t minTransitionMs = kRelayMinTransitionMs;
  };

  explicit RelaySequencer(CompressorIdleGate& gate) : RelaySequencer(gate, Config{}) {}
  RelaySequencer(CompressorIdleGate& gate, const Config& cfg) : gate_(gate), cfg_(cfg) {}

  // Recomputes desired states; call at the control-loop rate. Multiple relays
  // may change together in one transition; transitions closer than
  // minTransitionMs to the previous one are deferred (previous states held;
  // flags still track). nowMs wrap-safe via modular subtraction.
  const RelayOutputs& update(const Inputs& in, uint32_t nowS, uint32_t nowMs);

  // Immediate unconditional all-off (the firmware-side fail-to-no-demand
  // drop, docs/04 §1b) — never deferred by transition spacing.
  void goSilent();

  const RelayOutputs& outputs() const { return out_; }

 private:
  CompressorIdleGate& gate_;
  Config   cfg_;
  RelayOutputs out_;
  bool     spaced_        = false;  // lastChangeMs_ valid
  uint32_t lastChangeMs_  = 0;
};

}  // namespace dettson
