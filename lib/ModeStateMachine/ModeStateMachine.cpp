#include "ModeStateMachine.h"

#include <cmath>

namespace dettson {

namespace {
uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
  return nowS >= thenS ? nowS - thenS : 0;
}
}  // namespace

ModeStateMachine::ModeStateMachine() : ModeStateMachine(Config{}) {}

ModeStateMachine::ModeStateMachine(const Config& cfg) : cfg_(cfg) {
  if (cfg_.minSetpointDeltaC < kMinSetpointDeltaFloorC) {
    cfg_.minSetpointDeltaC = kMinSetpointDeltaFloorC;
  }
  if (coolSpC_ < heatSpC_ + cfg_.minSetpointDeltaC) {
    coolSpC_ = heatSpC_ + cfg_.minSetpointDeltaC;
  }
}

void ModeStateMachine::setMode(UserMode mode, uint32_t nowS) {
  if (mode == mode_) return;
  endActiveCall(nowS);
  resetTriggers();
  mode_ = mode;
}

ModeStateMachine::SetpointResult ModeStateMachine::setHeatSetpoint(float c) {
  SetpointResult r;
  r.heatC = heatSpC_;
  r.coolC = coolSpC_;
  if (std::isnan(c) || c < cfg_.setpointMinC || c > cfg_.setpointMaxC) return r;
  r.accepted = true;

  float heat = c;
  float cool = coolSpC_;
  if (cool < heat + cfg_.minSetpointDeltaC) {
    cool = heat + cfg_.minSetpointDeltaC;  // push the other setpoint up
    if (cool > cfg_.setpointMaxC) {        // pushed off the rail: pull the write back
      cool = cfg_.setpointMaxC;
      heat = cool - cfg_.minSetpointDeltaC;
    }
    r.coolChanged = (cool != coolSpC_);
  }
  r.heatChanged = (heat != heatSpC_);
  heatSpC_ = heat;
  coolSpC_ = cool;
  r.heatC = heat;
  r.coolC = cool;
  return r;
}

ModeStateMachine::SetpointResult ModeStateMachine::setCoolSetpoint(float c) {
  SetpointResult r;
  r.heatC = heatSpC_;
  r.coolC = coolSpC_;
  if (std::isnan(c) || c < cfg_.setpointMinC || c > cfg_.setpointMaxC) return r;
  r.accepted = true;

  float cool = c;
  float heat = heatSpC_;
  if (cool < heat + cfg_.minSetpointDeltaC) {
    heat = cool - cfg_.minSetpointDeltaC;  // push the other setpoint down
    if (heat < cfg_.setpointMinC) {
      heat = cfg_.setpointMinC;
      cool = heat + cfg_.minSetpointDeltaC;
    }
    r.heatChanged = (heat != heatSpC_);
  }
  r.coolChanged = (cool != coolSpC_);
  heatSpC_ = heat;
  coolSpC_ = cool;
  r.heatC = heat;
  r.coolC = cool;
  return r;
}

float ModeStateMachine::setMinSetpointDelta(float deltaC) {
  float d = deltaC;
  if (std::isnan(d) || d < kMinSetpointDeltaFloorC) d = kMinSetpointDeltaFloorC;
  cfg_.minSetpointDeltaC = d;
  if (coolSpC_ < heatSpC_ + d) {
    coolSpC_ = heatSpC_ + d;
    if (coolSpC_ > cfg_.setpointMaxC) {
      coolSpC_ = cfg_.setpointMaxC;
      heatSpC_ = coolSpC_ - d;
    }
  }
  return d;
}

void ModeStateMachine::endActiveCall(uint32_t nowS) {
  if (call_.type == CallType::kHeat) {
    hadHeatEnd_ = true;
    heatEndS_ = nowS;
    lastEndedCall_ = CallType::kHeat;
  } else if (call_.type == CallType::kCool) {
    hadCoolEnd_ = true;
    coolEndS_ = nowS;
    lastEndedCall_ = CallType::kCool;
  }
  call_ = Call{};
}

void ModeStateMachine::startCall(CallType t, float tempC, bool gasOnly) {
  call_.type = t;
  call_.gasOnly = gasOnly;
  call_.errorC = (t == CallType::kHeat) ? (heatSpC_ - tempC)
               : (t == CallType::kCool) ? (tempC - coolSpC_) : 0.0f;
}

void ModeStateMachine::resetTriggers() {
  heatTrigArmed_ = coolTrigArmed_ = false;
}

// Single-mode (HEAT/COOL/EM-HEAT) call: plain enter/exit hysteresis —
// enter at setpoint -/+ hysteresisC, exit at setpoint. Changeover gating
// does not apply (an explicit user mode switch already ended the opposite
// call; the compressor guard downstream still owns min-off on restart).
Call ModeStateMachine::evalSimple(CallType want, float spC, float tempC,
                                  uint32_t nowS, bool gasOnly) {
  const bool heating = (want == CallType::kHeat);
  if (call_.type == want) {
    const bool exit = heating ? (tempC >= spC) : (tempC <= spC);
    if (exit) {
      endActiveCall(nowS);  // timestamped: AUTO dwell counts ends from any mode
    } else {
      call_.errorC = heating ? (spC - tempC) : (tempC - spC);
    }
  } else if (call_.type == CallType::kNone) {
    const bool enter = heating ? (tempC <= spC - cfg_.hysteresisC)
                               : (tempC >= spC + cfg_.hysteresisC);
    if (enter) startCall(want, tempC, gasOnly);
  }
  return call_;
}

Call ModeStateMachine::evalAuto(float tempC, uint32_t nowS,
                                bool compressorSwapOk) {
  if (call_.type == CallType::kHeat) {
    if (tempC >= heatSpC_) {
      endActiveCall(nowS);
    } else {
      call_.errorC = heatSpC_ - tempC;
      return call_;
    }
  } else if (call_.type == CallType::kCool) {
    if (tempC <= coolSpC_) {
      endActiveCall(nowS);
    } else {
      call_.errorC = tempC - coolSpC_;
      return call_;
    }
  }

  const bool heatTrig = tempC <= heatSpC_ - cfg_.hysteresisC;
  const bool coolTrig = tempC >= coolSpC_ + cfg_.hysteresisC;

  // Sustain bookkeeping: arm when a trigger first turns true, clear when it
  // drops (a blip must restart the sustain clock).
  if (heatTrig && !heatTrigArmed_) { heatTrigArmed_ = true; heatTrigStartS_ = nowS; }
  if (!heatTrig) heatTrigArmed_ = false;
  if (coolTrig && !coolTrigArmed_) { coolTrigArmed_ = true; coolTrigStartS_ = nowS; }
  if (!coolTrig) coolTrigArmed_ = false;

  if (heatTrig && coolTrig) return call_;  // contradictory: hold no-call (deadband should prevent)

  if (heatTrig) {
    const bool changeover = (lastEndedCall_ == CallType::kCool);
    if (!changeover ||
        (elapsedS(nowS, heatTrigStartS_) >= cfg_.changeoverSustainS &&
         hadCoolEnd_ && elapsedS(nowS, coolEndS_) >= cfg_.changeoverDwellS &&
         compressorSwapOk)) {
      startCall(CallType::kHeat, tempC, false);
    }
  } else if (coolTrig) {
    const bool changeover = (lastEndedCall_ == CallType::kHeat);
    if (!changeover ||
        (elapsedS(nowS, coolTrigStartS_) >= cfg_.changeoverSustainS &&
         hadHeatEnd_ && elapsedS(nowS, heatEndS_) >= cfg_.changeoverDwellS &&
         compressorSwapOk)) {
      startCall(CallType::kCool, tempC, false);
    }
  }
  return call_;
}

Call ModeStateMachine::update(float tempC, bool tempValid, uint32_t nowS,
                              bool compressorSwapOk) {
  if (!tempValid || std::isnan(tempC)) {
    // Fail to no-demand (docs/04 §2 sensor row). End time is recorded so the
    // AUTO dwell clock still runs; triggers reset so a recovery can't carry
    // stale sustain credit.
    endActiveCall(nowS);
    resetTriggers();
    inputFaultAlarm_ = true;
    return call_;
  }
  inputFaultAlarm_ = false;

  switch (mode_) {
    case UserMode::kOff:
      endActiveCall(nowS);
      resetTriggers();
      return call_;
    case UserMode::kHeat:
      return evalSimple(CallType::kHeat, heatSpC_, tempC, nowS, false);
    case UserMode::kEmergencyHeat:
      return evalSimple(CallType::kHeat, heatSpC_, tempC, nowS, true);
    case UserMode::kCool:
      return evalSimple(CallType::kCool, coolSpC_, tempC, nowS, false);
    case UserMode::kAuto:
      return evalAuto(tempC, nowS, compressorSwapOk);
  }
  return call_;
}

}  // namespace dettson
