// SafetySupervisor.h — invariant registry, watchdog pet-gating, boot gate,
// comms-loss deadman, reset-loop accounting, and the alarm registry
// (docs/04-safety.md §2-§3, docs/01-architecture.md §5).
//
// Pure logic only: the firmware safety task reports health facts each cycle
// and wires the outputs to the real hardware:
//
//   sup.resetLoop().restore(...persisted...);          // once, from NVS
//   sup.resetLoop().recordBoot(nowS, abnormalReset);   // once, reset-reason reg
//   each safety cycle:
//     sup.updateBootGate(bootFacts, nowS);             // until bootGateOpen()
//     sup.update(healthFacts, nowS);
//     if (sup.petExternalWdt()) petHardwareWdt();
//     arbiter.set(sup.filterRequest(req), nowS);       // DemandArbiter's
//                                                      //  force-zero path
//
// Pet-gating policy (docs/04 §1c/§3: pet ONLY when the safety invariants
// hold; docs/04 §2 rows: HA/MQTT loss must NOT cause no-heat, an invalid
// sensor MUST):
//   MANDATORY (false -> stop petting -> external WDT forces no-demand):
//     sensorValid, controlLoopTicking, demandStateSane.
//   ADVISORY (false -> alarm only, petting continues):
//     mqttAlive, setpointFresh (stale setpoints fall back to the dual-bounded
//     local profile elsewhere — they are never a reason to drop heat).
//   busAlive is special: its failsafe is GOING QUIET, not a chip reset — a
//     deadman trip raises the demand-drop line + a critical alarm but keeps
//     petting (WDT starvation would reset-loop the chip without reviving the
//     bus; our silence already drops the calls equipment-side, docs/04 §1).
//   Boot exception: while the boot gate is closed, demands are blocked, so
//     sensorValid-false does not starve the watchdog — otherwise the chip
//     could never survive its own sensor bring-up. Once the gate opens, a
//     sensor loss stops petting per the table above.
//
// Demand-drop line: demandPermitted()/filterRequest() zero every channel when
// the boot gate is closed, the reset-loop latch is set, the deadman tripped,
// or any mandatory fact is false. Recovery from any drop still honors the
// compressor timers downstream — CompressorGuard owns restarts; nothing here
// bypasses it (docs/04 §1 prime directive).
//
// Reset-loop accounting complements CompressorGuard's internal lockout with a
// SYSTEM-wide latched no-demand (docs/04 §2 reset-loop row) sharing the same
// DettsonConfig constants. The latch does NOT stop petting: a latched chip
// must stay up to publish the alarm — starving the WDT would just keep the
// loop going.
//
// Alarm registry: bounded (kMaxAlarmEntries, oldest dropped + flag), severity
// + ack semantics. Entry text/code shapes match ui::Alarm (UiModel.h:
// char[40] + uint16 code — keep kAlarmTextLen == ui::kAlarmTextLen) and the
// HaMqtt health contract (binary_sensor problem <- anyActive(); last_error
// <- lastErrorText()). syncAlarmsToUi() is the no-dependency adapter.
//
// Pure C++17, no Arduino; time injected as uint32_t nowS (monotonic seconds).

#pragma once
#include <cstddef>
#include <cstdint>

#include "DemandArbiter.h"  // DemandRequest (filterRequest force-zero shape)
#include "DettsonConfig.h"

namespace dettson {
namespace safety {

// ---------- Severity & alarm codes ----------
enum class Severity : uint8_t { kAdvisory = 0, kCritical = 1 };

// Codes fit ui::Alarm::code (uint16). 0x53xx = SafetySupervisor namespace.
constexpr uint16_t kAlarmSensorInvalid      = 0x5301;
constexpr uint16_t kAlarmControlLoopStalled = 0x5302;
constexpr uint16_t kAlarmDemandStateInsane  = 0x5303;
constexpr uint16_t kAlarmBusDeadman         = 0x5304;
constexpr uint16_t kAlarmSetpointStale      = 0x5305;
constexpr uint16_t kAlarmMqttDown           = 0x5306;
constexpr uint16_t kAlarmBootGraceExceeded  = 0x5307;
constexpr uint16_t kAlarmResetLoopLatched   = 0x5308;

// ---------- Alarm registry ----------
constexpr size_t kAlarmTextLen    = 40;  // MUST equal ui::kAlarmTextLen
constexpr size_t kMaxAlarmEntries = 8;   // MUST equal ui::kMaxAlarms

struct AlarmEntry {
  uint16_t code      = 0;
  Severity severity  = Severity::kAdvisory;
  char     text[kAlarmTextLen] = {};
  uint32_t raisedAtS = 0;
  bool     active    = false;  // condition currently present
  bool     acked     = false;  // human has acknowledged
  bool     autoClear = false;  // auto-recoverable: drop on clearCondition, no ack (issue #72)
};

// Lifecycle: raise() inserts or reactivates (a re-raise after the condition
// cleared re-alerts: active again, un-acked). clearCondition() marks the
// condition gone — the entry stays visible until ack'd so a transient fault
// is never silently lost. ack() of an inactive entry removes it; of an active
// one keeps it listed (the problem persists) but acknowledged. An entry
// leaves the list only when it is both inactive and acked.
//
// autoClear (issue #72): an alarm raised with autoClear=true is
// auto-recoverable (OAT source returns, MQTT reconnects, a valid room sample
// arrives) — clearCondition() removes it immediately, no ack needed, so a
// resolved condition can't linger as a stale Diag entry. autoClear=false
// (default) keeps the persist-until-ack contract for latched safety events.
class AlarmRegistry {
 public:
  void raise(uint16_t code, Severity sev, const char* text, uint32_t nowS,
             bool autoClear = false);
  void clearCondition(uint16_t code);
  bool ack(uint16_t code);  // false if code not listed
  void ackAll();

  uint8_t count() const { return count_; }
  const AlarmEntry& at(uint8_t i) const { return entries_[i < count_ ? i : 0]; }
  const AlarmEntry* find(uint16_t code) const;

  bool anyActive() const;          // HaMqtt health binary_sensor (problem)
  bool anyActiveCritical() const;
  bool anyUnacked() const;
  // Text of the most recent raise(), retained even after the entry is gone
  // (HaMqtt slytherm/state/last_error). "" if never raised.
  const char* lastErrorText() const { return lastError_; }

  bool overflowed() const { return overflowed_; }  // bounded list dropped oldest
  void clearOverflowed() { overflowed_ = false; }

 private:
  int  findIdx(uint16_t code) const;
  void removeAt(uint8_t i);

  AlarmEntry entries_[kMaxAlarmEntries] = {};
  uint8_t    count_      = 0;
  bool       overflowed_ = false;
  char       lastError_[kAlarmTextLen] = {};
};

// Adapter to any sink with the UiModel alarm shape (clearAlarms() +
// pushAlarm(const char*, uint16_t)) — templated so this lib never depends on
// the UI lib. Pushes every listed entry (active or awaiting ack).
template <typename UiAlarmSink>
void syncAlarmsToUi(const AlarmRegistry& reg, UiAlarmSink& ui) {
  ui.clearAlarms();
  for (uint8_t i = 0; i < reg.count(); ++i) {
    const AlarmEntry& a = reg.at(i);
    ui.pushAlarm(a.text, a.code);
  }
}

// ---------- Reset-loop accounting ----------
// Boot-counter window logic for the system-wide latched NO-DEMAND (docs/04
// §2: >=3 watchdog/brownout resets in 30 min -> latched, manual clear).
// Persistence is the caller's (NVS) via save()/restore(); only ABNORMAL boots
// (watchdog/brownout/panic per the reset-reason register) count — a normal
// power-on never trips it (same rule as CompressorGuard::bootRestore).
class ResetLoopAccountant {
 public:
  static constexpr size_t kMaxBootHistory = 8;  // == CompressorGuard::kMaxBootHistory

  struct Config {
    uint8_t  lockoutCount = kResetLoopLockoutCount;
    uint32_t windowS      = kResetLoopWindowS;
  };

  ResetLoopAccountant();  // default Config (out-of-line: NSDMI-in-default-arg quirk)
  explicit ResetLoopAccountant(const Config& cfg) : cfg_(cfg) {}

  // Call before recordBoot(). Timestamps are in the caller's reboot-surviving
  // timebase (RTC/epoch seconds — same requirement as CompressorGuard).
  void restore(const uint32_t* abnormalBootTimesS, size_t n, bool latched);
  void recordBoot(uint32_t nowS, bool abnormalReset);  // once per boot

  bool    latched() const { return latched_; }
  uint8_t abnormalBootsInWindow(uint32_t nowS) const;

  // Deliberate human action only; clears latch + history. Compressor timers
  // are unaffected (CompressorGuard owns them) — clearing never grants an
  // instant start.
  void manualClear();

  size_t save(uint32_t* outTimesS, size_t cap) const;  // returns count written

 private:
  static uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
    return nowS >= thenS ? nowS - thenS : 0;  // backwards RTC step = 0, conservative
  }

  Config   cfg_;
  bool     latched_ = false;
  uint8_t  count_   = 0;
  uint32_t timesS_[kMaxBootHistory] = {};
};

// ---------- Boot gate ----------
// Holds "demands allowed" false until every injected boot fact validates
// (docs/04 §3 boot validation: boot = no demand until mode/setpoints/sensors
// validated + config CRC ok). Opens once, latched — runtime degradation is
// the HealthFacts path's job, not the gate's.
struct BootFacts {
  bool sensorOk       = false;
  bool setpointPresent = false;
  bool configCrcOk    = false;
};

class BootGate {
 public:
  BootGate(uint32_t bootNowS, uint32_t graceS = kBootValidationGraceS)
      : bootS_(bootNowS), graceS_(graceS) {}

  void update(const BootFacts& f, uint32_t nowS);
  bool open() const { return open_; }
  bool graceExceeded() const { return graceExceeded_; }  // still closed past grace

 private:
  uint32_t bootS_;
  uint32_t graceS_;
  bool     open_          = false;
  bool     graceExceeded_ = false;
};

// ---------- Health facts (invariant registry) ----------
// Defaults are all-false: unreported = unproven = unsafe (docs/04 §3).
struct HealthFacts {
  bool sensorValid        = false;  // mandatory
  bool setpointFresh      = false;  // advisory (fallback profile covers it)
  bool mqttAlive          = false;  // advisory (HA is never a safety layer)
  bool busAlive           = false;  // deadman: drop + alarm, keep petting
  bool controlLoopTicking = false;  // mandatory
  bool demandStateSane    = false;  // mandatory (emitted == commanded, no conflict)
};

// ---------- Supervisor ----------
class SafetySupervisor {
 public:
  struct Config {
    uint32_t busDeadmanS = kBusDeadmanS;
    uint32_t bootGraceS  = kBootValidationGraceS;
    ResetLoopAccountant::Config resetLoop{};
  };

  explicit SafetySupervisor(uint32_t bootNowS) : SafetySupervisor(bootNowS, Config{}) {}
  SafetySupervisor(uint32_t bootNowS, const Config& cfg);

  // Boot-time wiring (see header sketch).
  ResetLoopAccountant&       resetLoop() { return resetLoop_; }
  const ResetLoopAccountant& resetLoop() const { return resetLoop_; }

  // Call each cycle until bootGateOpen(); harmless afterwards.
  void updateBootGate(const BootFacts& f, uint32_t nowS);
  // Call every safety cycle with the current facts.
  void update(const HealthFacts& f, uint32_t nowS);

  // --- outputs the firmware task acts on ---
  bool petExternalWdt() const;
  bool demandsAllowed() const;       // boot gate open AND reset latch clear
  bool demandDropRequested() const;  // runtime: deadman or mandatory fact down
  bool demandPermitted() const { return demandsAllowed() && !demandDropRequested(); }
  // The DemandArbiter integration: feed its set() this instead of the raw
  // request — all-zero is its existing force-zero path; no arbiter changes.
  DemandRequest filterRequest(const DemandRequest& req) const;

  // --- diagnostics ---
  bool bootGateOpen() const { return bootGate_.open(); }
  bool bootGraceExceeded() const { return bootGate_.graceExceeded(); }
  bool busDeadmanTripped() const { return deadmanTripped_; }
  const HealthFacts& facts() const { return facts_; }

  AlarmRegistry&       alarms() { return alarms_; }
  const AlarmRegistry& alarms() const { return alarms_; }
  bool healthProblem() const { return alarms_.anyActive(); }  // HaMqtt health

 private:
  static uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
    return nowS >= thenS ? nowS - thenS : 0;
  }
  void setCondition(bool present, uint16_t code, Severity sev, const char* text,
                    uint32_t nowS, bool autoClear = false);
  bool mandatoryOk() const;

  Config              cfg_;
  ResetLoopAccountant resetLoop_;
  BootGate            bootGate_;
  AlarmRegistry       alarms_;
  HealthFacts         facts_{};  // all-false until first update()

  bool     busSilent_       = false;
  uint32_t busSilentSinceS_ = 0;
  bool     deadmanTripped_  = false;
};

}  // namespace safety
}  // namespace dettson
