// SafetySupervisor.cpp — see header for policy rationale (docs/04 §2-§3).
#include "SafetySupervisor.h"

#include <cstring>

namespace dettson {
namespace safety {

namespace {
void copyText(char (&dst)[kAlarmTextLen], const char* src) {
  if (src == nullptr) src = "";
  std::strncpy(dst, src, kAlarmTextLen - 1);
  dst[kAlarmTextLen - 1] = '\0';
}
}  // namespace

// ---------- AlarmRegistry ----------

int AlarmRegistry::findIdx(uint16_t code) const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (entries_[i].code == code) return i;
  }
  return -1;
}

const AlarmEntry* AlarmRegistry::find(uint16_t code) const {
  int i = findIdx(code);
  return i >= 0 ? &entries_[i] : nullptr;
}

void AlarmRegistry::removeAt(uint8_t i) {
  for (uint8_t j = i; j + 1 < count_; ++j) entries_[j] = entries_[j + 1];
  --count_;
  entries_[count_] = AlarmEntry{};
}

void AlarmRegistry::raise(uint16_t code, Severity sev, const char* text,
                          uint32_t nowS, bool autoClear) {
  copyText(lastError_, text);
  int i = findIdx(code);
  if (i >= 0) {
    AlarmEntry& e = entries_[i];
    if (!e.active) {  // re-raise after clear re-alerts
      e.active    = true;
      e.acked     = false;
      e.raisedAtS = nowS;
    }
    e.severity  = sev;
    e.autoClear = autoClear;  // keep the recoverability flag current
    copyText(e.text, text);
    return;
  }
  if (count_ == kMaxAlarmEntries) {  // bounded: drop oldest + flag
    removeAt(0);
    overflowed_ = true;
  }
  AlarmEntry& e = entries_[count_++];
  e.code      = code;
  e.severity  = sev;
  e.raisedAtS = nowS;
  e.active    = true;
  e.acked     = false;
  e.autoClear = autoClear;
  copyText(e.text, text);
}

void AlarmRegistry::clearCondition(uint16_t code) {
  int i = findIdx(code);
  if (i < 0) return;
  // Auto-recoverable (issue #72) or already acked -> drop the moment the
  // condition resolves, so a healthy live state shows no stale alarm.
  if (entries_[i].acked || entries_[i].autoClear) {
    removeAt(static_cast<uint8_t>(i));
  } else {
    entries_[i].active = false;  // stays visible until acked
  }
}

bool AlarmRegistry::ack(uint16_t code) {
  int i = findIdx(code);
  if (i < 0) return false;
  if (entries_[i].active) {
    entries_[i].acked = true;
  } else {
    removeAt(static_cast<uint8_t>(i));
  }
  return true;
}

void AlarmRegistry::ackAll() {
  for (int i = count_ - 1; i >= 0; --i) {
    if (entries_[i].active) {
      entries_[i].acked = true;
    } else {
      removeAt(static_cast<uint8_t>(i));
    }
  }
}

bool AlarmRegistry::anyActive() const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (entries_[i].active) return true;
  }
  return false;
}

bool AlarmRegistry::anyActiveCritical() const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (entries_[i].active && entries_[i].severity == Severity::kCritical) return true;
  }
  return false;
}

bool AlarmRegistry::anyUnacked() const {
  for (uint8_t i = 0; i < count_; ++i) {
    if (!entries_[i].acked) return true;
  }
  return false;
}

// ---------- ResetLoopAccountant ----------

ResetLoopAccountant::ResetLoopAccountant() : ResetLoopAccountant(Config{}) {}

void ResetLoopAccountant::restore(const uint32_t* abnormalBootTimesS, size_t n,
                                  bool latched) {
  count_ = 0;
  if (abnormalBootTimesS != nullptr) {
    if (n > kMaxBootHistory) {  // keep the newest entries
      abnormalBootTimesS += n - kMaxBootHistory;
      n = kMaxBootHistory;
    }
    for (size_t i = 0; i < n; ++i) timesS_[count_++] = abnormalBootTimesS[i];
  }
  latched_ = latched;
}

void ResetLoopAccountant::recordBoot(uint32_t nowS, bool abnormalReset) {
  if (!abnormalReset) return;  // normal power-on never counts (docs/05 table)
  if (count_ == kMaxBootHistory) {
    for (size_t i = 0; i + 1 < kMaxBootHistory; ++i) timesS_[i] = timesS_[i + 1];
    --count_;
  }
  timesS_[count_++] = nowS;
  if (abnormalBootsInWindow(nowS) >= cfg_.lockoutCount) latched_ = true;
}

uint8_t ResetLoopAccountant::abnormalBootsInWindow(uint32_t nowS) const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < count_; ++i) {
    if (elapsedS(nowS, timesS_[i]) <= cfg_.windowS) ++n;
  }
  return n;
}

void ResetLoopAccountant::manualClear() {
  latched_ = false;
  count_   = 0;
}

size_t ResetLoopAccountant::save(uint32_t* outTimesS, size_t cap) const {
  size_t n = count_ < cap ? count_ : cap;
  for (size_t i = 0; i < n; ++i) outTimesS[i] = timesS_[i];
  return n;
}

// ---------- BootGate ----------

void BootGate::update(const BootFacts& f, uint32_t nowS) {
  if (open_) return;  // latched open; runtime health is the facts path's job
  if (f.sensorOk && f.setpointPresent && f.configCrcOk) {
    open_          = true;
    graceExceeded_ = false;
    return;
  }
  if (nowS >= bootS_ && nowS - bootS_ >= graceS_) graceExceeded_ = true;
}

// ---------- SafetySupervisor ----------

SafetySupervisor::SafetySupervisor(uint32_t bootNowS, const Config& cfg)
    : cfg_(cfg),
      resetLoop_(cfg.resetLoop),
      bootGate_(bootNowS, cfg.bootGraceS) {}

void SafetySupervisor::setCondition(bool present, uint16_t code, Severity sev,
                                    const char* text, uint32_t nowS,
                                    bool autoClear) {
  if (present) {
    alarms_.raise(code, sev, text, nowS, autoClear);
  } else {
    alarms_.clearCondition(code);
  }
}

void SafetySupervisor::updateBootGate(const BootFacts& f, uint32_t nowS) {
  bootGate_.update(f, nowS);
  // Auto-clears the instant the boot gate validates (sensors/setpoints arrive).
  setCondition(bootGate_.graceExceeded(), kAlarmBootGraceExceeded,
               Severity::kCritical, "boot validation overdue, demands held",
               nowS, /*autoClear=*/true);
}

void SafetySupervisor::update(const HealthFacts& f, uint32_t nowS) {
  facts_ = f;

  // Comms-loss deadman: trips on continuous silence; the alarm condition
  // clears only when the bus is alive again (not merely below the threshold).
  if (f.busAlive) {
    busSilent_ = false;
    if (deadmanTripped_) {
      deadmanTripped_ = false;
      alarms_.clearCondition(kAlarmBusDeadman);
    }
  } else {
    if (!busSilent_) {
      busSilent_       = true;
      busSilentSinceS_ = nowS;
    }
    if (!deadmanTripped_ && elapsedS(nowS, busSilentSinceS_) >= cfg_.busDeadmanS) {
      deadmanTripped_ = true;
      // autoClear: this is a transient condition (bus silent -> bus alive), like
      // the sensor/MQTT staleness alarms (#72). demandDropRequested() already
      // lifts the moment busAlive returns (deadmanTripped_ = false above); the
      // ALARM must drop with it, not linger as a stale un-acked entry after the
      // bus recovers. Without autoClear=true, clearCondition() above (persist-
      // until-ack contract) leaves it latched — the stale-alarm bug.
      alarms_.raise(kAlarmBusDeadman, Severity::kCritical,
                    "CT-485 silent: deadman demand drop", nowS, /*autoClear=*/true);
    }
  }

  // Auto-recoverable conditions (issue #72) clear the moment their input
  // returns — no manual ack — so Diag never shows a stale sensor/MQTT alarm
  // while the live state is healthy. The invariant/latched faults below keep
  // the persist-until-ack contract (a transient must never be silently lost).
  setCondition(!f.sensorValid, kAlarmSensorInvalid, Severity::kCritical,
               "temp sensor invalid: no demand", nowS, /*autoClear=*/true);
  setCondition(!f.controlLoopTicking, kAlarmControlLoopStalled,
               Severity::kCritical, "control loop stalled", nowS);
  setCondition(!f.demandStateSane, kAlarmDemandStateInsane, Severity::kCritical,
               "demand state insane: forced zero", nowS);
  setCondition(!f.setpointFresh, kAlarmSetpointStale, Severity::kAdvisory,
               "setpoints stale: local fallback", nowS, /*autoClear=*/true);
  setCondition(!f.mqttAlive, kAlarmMqttDown, Severity::kAdvisory,
               "MQTT/HA link down", nowS, /*autoClear=*/true);
  setCondition(resetLoop_.latched(), kAlarmResetLoopLatched, Severity::kCritical,
               "reset loop: latched no-demand", nowS);
}

bool SafetySupervisor::mandatoryOk() const {
  // Boot exception (see header): a not-yet-valid sensor cannot starve the
  // watchdog while the boot gate already blocks all demand.
  return facts_.controlLoopTicking && facts_.demandStateSane &&
         (facts_.sensorValid || !bootGate_.open());
}

bool SafetySupervisor::petExternalWdt() const { return mandatoryOk(); }

bool SafetySupervisor::demandsAllowed() const {
  return bootGate_.open() && !resetLoop_.latched();
}

bool SafetySupervisor::demandDropRequested() const {
  return deadmanTripped_ || !facts_.sensorValid || !facts_.controlLoopTicking ||
         !facts_.demandStateSane;
}

DemandRequest SafetySupervisor::filterRequest(const DemandRequest& req) const {
  return demandPermitted() ? req : DemandRequest{};
}

}  // namespace safety
}  // namespace dettson
