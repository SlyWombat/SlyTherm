#include "UiModel.h"

#include <cstring>

namespace dettson {
namespace ui {
namespace {

bool feq(float a, float b) { return a == b; }  // exact: state echoes, not math

bool rowEquals(const SensorRow& a, const SensorRow& b) {
  return std::strncmp(a.name, b.name, kSensorNameLen) == 0 &&
         feq(a.tempC, b.tempC) &&
         a.occupied == b.occupied &&
         a.ageS == b.ageS &&
         a.participating == b.participating &&
         a.healthy == b.healthy;
}

void copyBounded(char* dst, size_t dstLen, const char* src) {
  size_t i = 0;
  if (src != nullptr) {
    for (; i + 1 < dstLen && src[i] != '\0'; ++i) dst[i] = src[i];
  }
  dst[i] = '\0';
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

}  // namespace

UiModel::UiModel(const UiModelConfig& cfg) : cfg_(cfg) {
  if (!(cfg_.minSetpointDeltaC >= kMinSetpointDeltaFloorC)) {
    cfg_.minSetpointDeltaC = kMinSetpointDeltaFloorC;
  }
  // state_ default-constructs to the safe boot picture: mode OFF, no action,
  // no equipment, invalid temp, fallback setpoints (docs/04 §3: boot = no demand).
}

// ---------- control-task setters ----------

void UiModel::setFusedTemp(float tempC, bool valid) {
  if (feq(state_.fusedTempC, tempC) && state_.fusedTempValid == valid) return;
  state_.fusedTempC = tempC;
  state_.fusedTempValid = valid;
  setDirty(kDirtyTemp);
}

void UiModel::setSetpoints(float heatC, float coolC) {
  if (feq(state_.heatSetpointC, heatC) && feq(state_.coolSetpointC, coolC)) return;
  state_.heatSetpointC = heatC;
  state_.coolSetpointC = coolC;
  setDirty(kDirtySetpoints);
}

void UiModel::setUserMode(UserMode mode) {
  if (state_.mode == mode) return;
  state_.mode = mode;
  setDirty(kDirtyMode);
}

void UiModel::setHvacAction(HvacAction action) {
  if (state_.action == action) return;
  state_.action = action;
  setDirty(kDirtyAction);
}

void UiModel::setActiveEquipment(uint8_t mask) {
  if (state_.activeEquipment == mask) return;
  state_.activeEquipment = mask;
  setDirty(kDirtyEquipment);
}

void UiModel::setSensorRows(const SensorRow* rows, uint8_t count) {
  if (rows == nullptr) count = 0;
  if (count > kMaxSensorRows) count = kMaxSensorRows;
  bool changed = (count != state_.sensorCount);
  for (uint8_t i = 0; !changed && i < count; ++i) {
    changed = !rowEquals(state_.sensors[i], rows[i]);
  }
  if (!changed) return;
  for (uint8_t i = 0; i < count; ++i) state_.sensors[i] = rows[i];
  state_.sensorCount = count;
  setDirty(kDirtySensors);
}

void UiModel::setOutdoor(float tempC, bool valid, OutdoorSource source) {
  if (feq(state_.outdoorTempC, tempC) && state_.outdoorValid == valid &&
      state_.outdoorSource == source) {
    return;
  }
  state_.outdoorTempC = tempC;
  state_.outdoorValid = valid;
  state_.outdoorSource = source;
  setDirty(kDirtyOutdoor);
}

void UiModel::setCompressorLockoutRemain(uint32_t seconds) {
  if (state_.compressorLockoutRemainS == seconds) return;
  state_.compressorLockoutRemainS = seconds;
  setDirty(kDirtyLockout);
}

void UiModel::setGasModulationPct(float pct) {
  if (feq(state_.gasModulationPct, pct)) return;
  state_.gasModulationPct = pct;
  setDirty(kDirtyGasMod);
}

void UiModel::pushAlarm(const char* text, uint16_t code) {
  if (state_.alarmCount == kMaxAlarms) {
    // Bounded list: drop oldest, keep newest, flag the loss.
    for (size_t i = 1; i < kMaxAlarms; ++i) state_.alarms[i - 1] = state_.alarms[i];
    --state_.alarmCount;
    state_.alarmsDropped = true;
  }
  Alarm& slot = state_.alarms[state_.alarmCount++];
  copyBounded(slot.text, kAlarmTextLen, text);
  slot.code = code;
  setDirty(kDirtyAlarms);
}

void UiModel::clearAlarms() {
  if (state_.alarmCount == 0 && !state_.alarmsDropped) return;
  state_.alarmCount = 0;
  state_.alarmsDropped = false;
  setDirty(kDirtyAlarms);
}

void UiModel::setLinkHealth(bool wifi, bool mqtt, bool bus) {
  if (state_.wifiOk == wifi && state_.mqttOk == mqtt && state_.busOk == bus) return;
  state_.wifiOk = wifi;
  state_.mqttOk = mqtt;
  state_.busOk = bus;
  setDirty(kDirtyHealth);
}

void UiModel::setDegradedMode(bool on) {
  if (state_.degradedMode == on) return;
  state_.degradedMode = on;
  setDirty(kDirtyHealth);
}

void UiModel::setMinSetpointDelta(float deltaC) {
  cfg_.minSetpointDeltaC =
      (deltaC >= kMinSetpointDeltaFloorC) ? deltaC : kMinSetpointDeltaFloorC;
}

// ---------- UiCommands ----------

void UiModel::adjustSetpoint(SetpointSide side, float deltaC) {
  if (!std::isfinite(deltaC)) {  // garbage from a sick UI: reject, never enqueue
    ++rejectedCommands_;
    return;
  }
  const float minDelta = cfg_.minSetpointDeltaC;
  Setpoints sp{state_.heatSetpointC, state_.coolSetpointC};
  if (side == SetpointSide::kHeat) {
    // Pre-clamp the moved setpoint so the pushed one stays inside the UI range.
    sp.heatC = clampf(sp.heatC + deltaC, cfg_.setpointMinC, cfg_.setpointMaxC - minDelta);
  } else {
    sp.coolC = clampf(sp.coolC + deltaC, cfg_.setpointMinC + minDelta, cfg_.setpointMaxC);
  }
  sp = applyDeadbandClampPush(sp, side, minDelta);

  // Local echo for responsiveness; the control task remains authoritative and
  // will re-echo (possibly corrected) values via setSetpoints().
  if (!feq(state_.heatSetpointC, sp.heatC) || !feq(state_.coolSetpointC, sp.coolC)) {
    state_.heatSetpointC = sp.heatC;
    state_.coolSetpointC = sp.coolC;
    setDirty(kDirtySetpoints);
  }

  UiIntent intent;
  intent.type = IntentType::kSetSetpoints;
  intent.heatC = sp.heatC;
  intent.coolC = sp.coolC;
  enqueue(intent);
}

void UiModel::setMode(UserMode mode) {
  if (state_.mode != mode) {
    state_.mode = mode;  // local echo only; control task owns the real mode
    setDirty(kDirtyMode);
  }
  UiIntent intent;
  intent.type = IntentType::kSetMode;
  intent.mode = mode;
  enqueue(intent);
}

void UiModel::setPreset(Preset preset) {
  // No local echo: preset resolution (setpoint pair) lives in control/HA.
  UiIntent intent;
  intent.type = IntentType::kSetPreset;
  intent.preset = preset;
  enqueue(intent);
}

// ---------- intent queue ----------

void UiModel::enqueue(const UiIntent& intent) {
  if (queueCount_ == kIntentQueueCap) {
    queueHead_ = (queueHead_ + 1) % kIntentQueueCap;  // drop oldest
    --queueCount_;
    intentOverflow_ = true;
    ++droppedIntents_;
  }
  queue_[(queueHead_ + queueCount_) % kIntentQueueCap] = intent;
  ++queueCount_;
}

bool UiModel::popIntent(UiIntent& out) {
  if (queueCount_ == 0) return false;
  out = queue_[queueHead_];
  queueHead_ = (queueHead_ + 1) % kIntentQueueCap;
  --queueCount_;
  if (queueCount_ == 0) intentOverflow_ = false;  // drained: overflow episode over
  return true;
}

}  // namespace ui
}  // namespace dettson
