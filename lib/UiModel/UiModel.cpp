#include "UiModel.h"

#include <cstddef>
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

// Backwards time (RTC step) = zero elapsed — conservative for relock/backoff.
uint32_t elapsedS(uint32_t nowS, uint32_t thenS) {
  return nowS >= thenS ? nowS - thenS : 0;
}

}  // namespace

uint32_t pinHash(uint32_t salt, const uint8_t digits[kPinLen]) {
  // FNV-1a, salt bytes first then the digits. See header: casual tamper
  // resistance only, not crypto.
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < 4; ++i) {
    h = (h ^ static_cast<uint8_t>(salt >> (8 * i))) * 16777619u;
  }
  for (size_t i = 0; i < kPinLen; ++i) h = (h ^ digits[i]) * 16777619u;
  return h;
}

UiModel::UiModel(const UiModelConfig& cfg) : cfg_(cfg) {
  if (!(cfg_.minSetpointDeltaC >= kMinSetpointDeltaFloorC)) {
    cfg_.minSetpointDeltaC = kMinSetpointDeltaFloorC;
  }
  if (cfg_.pinMaxAttempts == 0) cfg_.pinMaxAttempts = 1;
  attemptsLeft_ = cfg_.pinMaxAttempts;
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
  if (!setpointChangeAllowed()) {  // lock blocks the change intent only
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);  // let the screen show "locked" feedback
    return;
  }
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
  if (!settingsChangeAllowed()) {  // mode is a settings-class intent
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);
    return;
  }
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
  if (!setpointChangeAllowed()) {  // presets resolve to setpoints — same class
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);
    return;
  }
  // No local echo: preset resolution (setpoint pair) lives in control/HA.
  UiIntent intent;
  intent.type = IntentType::kSetPreset;
  intent.preset = preset;
  enqueue(intent);
}

void UiModel::requestHold(HoldType t) {
  if (!setpointChangeAllowed()) {  // a hold is a comfort-class change
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);
    return;
  }
  UiIntent intent;
  intent.type = IntentType::kSetHold;
  intent.hold = t;
  enqueue(intent);
}
void UiModel::resumeSchedule() {
  if (!setpointChangeAllowed()) {
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);
    return;
  }
  UiIntent intent;
  intent.type = IntentType::kClearHold;
  enqueue(intent);
}
void UiModel::ackAlarms() {
  // Issue #45: alarm VISIBILITY is exempt from every lock level, but the ack
  // is a change intent and stays locked.
  if (!settingsChangeAllowed()) {
    ++lockBlockedCommands_;
    setDirty(kDirtyLock);
    return;
  }
  UiIntent intent;
  intent.type = IntentType::kAckAlarms;
  enqueue(intent);
}

// ---------- screen lock (issue #45) ----------

void UiModel::setUserPin(const uint8_t digits[kPinLen], uint32_t salt) {
  userSalt_ = salt;
  userHash_ = pinHash(salt, digits);
  userPinSet_ = true;
  setDirty(kDirtyLock);
}

void UiModel::setInstallerPin(const uint8_t digits[kPinLen], uint32_t salt) {
  installerSalt_ = salt;
  installerHash_ = pinHash(salt, digits);
  installerPinSet_ = true;
  setDirty(kDirtyLock);
}

void UiModel::clearUserPin() {
  userPinSet_ = false;
  userSalt_ = 0;
  userHash_ = 0;
  // Release a USER lock and any in-flight entry/backoff; an installer
  // lockout is independent and stays (docs/06 recovery clears the user PIN only).
  if (lockState_ == LockState::kUserLocked) lockState_ = LockState::kUnlocked;
  wipePinEntry();
  backoffActive_ = false;
  attemptsLeft_ = cfg_.pinMaxAttempts;
  setDirty(kDirtyLock);
}

void UiModel::setLockLevel(LockLevel level) {
  if (lockLevel_ == level) return;
  lockLevel_ = level;
  setDirty(kDirtyLock);
}

bool UiModel::lockNow(uint32_t nowS) {
  if (!userPinSet_ || lockState_ == LockState::kInstallerLocked) return false;
  lockState_ = LockState::kUserLocked;
  wipePinEntry();
  touchActivity(nowS);
  setDirty(kDirtyLock);
  return true;
}

bool UiModel::setInstallerLockout(bool on, uint32_t nowS) {
  if (on) {
    if (!installerPinSet_) return false;  // no code = no way back in
    installerLockout_ = true;
    lockState_ = LockState::kInstallerLocked;
    installerAccess_ = false;
  } else {
    installerLockout_ = false;
    // Whoever is disabling it is present and unlocked; auto-relock will
    // re-engage the user lock later if a user PIN is set.
    if (lockState_ == LockState::kInstallerLocked) lockState_ = LockState::kUnlocked;
  }
  wipePinEntry();
  touchActivity(nowS);
  setDirty(kDirtyLock);
  return true;
}

void UiModel::beginPinEntry(PinContext ctx, uint32_t nowS) {
  wipePinEntry();
  pinCtx_ = ctx;
  touchActivity(nowS);
  setDirty(kDirtyLock);
}

PinResult UiModel::enterPinDigit(uint8_t digit, uint32_t nowS) {
  touchActivity(nowS);
  if (pinCtx_ == PinContext::kNone) return PinResult::kIdle;
  if (inBackoff(nowS)) return PinResult::kBackoff;
  if (digit > 9) return PinResult::kPending;  // not a keypad digit: ignore
  pinBuf_[pinCount_++] = digit;
  setDirty(kDirtyLock);  // entry-progress dots
  if (pinCount_ < kPinLen) return PinResult::kPending;

  const bool userMatch =
      userPinSet_ && pinHash(userSalt_, pinBuf_) == userHash_;
  const bool installerMatch =
      installerPinSet_ && pinHash(installerSalt_, pinBuf_) == installerHash_;
  const PinContext ctx = pinCtx_;
  bool ok;
  if (ctx == PinContext::kInstallerSettings ||
      lockState_ == LockState::kInstallerLocked) {
    ok = installerMatch;  // installer-only gates: the user PIN never opens them
  } else {
    ok = userMatch || installerMatch;  // installer code is a master key
  }
  wipePinEntry();  // never keep entered digits around (also ends the context)

  if (ok) {
    attemptsLeft_ = cfg_.pinMaxAttempts;
    backoffActive_ = false;
    if (ctx == PinContext::kInstallerSettings) installerAccess_ = true;
    // The accepted code also unlocks the screen (only the installer code can
    // have matched while installer-locked).
    lockState_ = LockState::kUnlocked;
    return PinResult::kAccepted;
  }
  if (attemptsLeft_ > 0) --attemptsLeft_;
  if (attemptsLeft_ == 0) {
    backoffActive_ = true;
    backoffEndS_ = nowS + cfg_.pinBackoffS;
    return PinResult::kBackoff;
  }
  return PinResult::kRejected;
}

void UiModel::cancelPinEntry() {
  wipePinEntry();
  setDirty(kDirtyLock);
}

void UiModel::tick(uint32_t nowS) {
  if (backoffActive_ && nowS >= backoffEndS_) {
    backoffActive_ = false;
    attemptsLeft_ = cfg_.pinMaxAttempts;
    setDirty(kDirtyLock);
  }
  if (elapsedS(nowS, lastActivityS_) >= cfg_.autoRelockS) {
    if (installerAccess_) {
      installerAccess_ = false;
      setDirty(kDirtyLock);
    }
    if (pinCtx_ != PinContext::kNone) cancelPinEntry();
    relock();
  }
}

uint32_t UiModel::backoffRemainS(uint32_t nowS) const {
  if (!backoffActive_) return 0;
  return nowS >= backoffEndS_ ? 0 : backoffEndS_ - nowS;
}

bool UiModel::inBackoff(uint32_t nowS) const {
  return backoffActive_ && nowS < backoffEndS_;
}

void UiModel::wipePinEntry() {
  pinCtx_ = PinContext::kNone;
  pinCount_ = 0;
  std::memset(pinBuf_, 0, sizeof(pinBuf_));
}

void UiModel::relock() {
  if (lockState_ != LockState::kUnlocked) return;
  if (installerLockout_) lockState_ = LockState::kInstallerLocked;
  else if (userPinSet_) lockState_ = LockState::kUserLocked;
  else return;
  setDirty(kDirtyLock);
}

uint16_t UiModel::lockBlobCrc(const LockPersistBlob& b) {
  // Fletcher-16 over everything after the crc field (same scheme as
  // CompressorGuard::PersistBlob).
  const uint8_t* p = reinterpret_cast<const uint8_t*>(&b);
  const size_t start = offsetof(LockPersistBlob, crc) + sizeof(b.crc);
  uint16_t sum1 = 0, sum2 = 0;
  for (size_t i = start; i < sizeof(LockPersistBlob); ++i) {
    sum1 = static_cast<uint16_t>((sum1 + p[i]) % 255);
    sum2 = static_cast<uint16_t>((sum2 + sum1) % 255);
  }
  return static_cast<uint16_t>((sum2 << 8) | sum1);
}

void UiModel::saveLock(LockPersistBlob* out) const {
  if (out == nullptr) return;
  *out = LockPersistBlob{};
  out->magic = kLockBlobMagic;
  out->version = kLockBlobVersion;
  out->userPinSet = userPinSet_ ? 1 : 0;
  out->installerPinSet = installerPinSet_ ? 1 : 0;
  out->lockLevel = static_cast<uint8_t>(lockLevel_);
  out->installerLockout = installerLockout_ ? 1 : 0;
  out->userSalt = userSalt_;
  out->userHash = userHash_;
  out->installerSalt = installerSalt_;
  out->installerHash = installerHash_;
  out->crc = lockBlobCrc(*out);
}

bool UiModel::restoreLock(const LockPersistBlob* blob, uint32_t nowS) {
  touchActivity(nowS);
  if (blob == nullptr || blob->magic != kLockBlobMagic ||
      blob->version != kLockBlobVersion || blob->crc != lockBlobCrc(*blob) ||
      blob->lockLevel > static_cast<uint8_t>(LockLevel::kSettingsAndSetpoints)) {
    // Fail OPEN (see header): lock is comfort/tamper-resistance, not safety.
    setDirty(kDirtyLock);
    return false;
  }
  userPinSet_ = blob->userPinSet != 0;
  installerPinSet_ = blob->installerPinSet != 0;
  lockLevel_ = static_cast<LockLevel>(blob->lockLevel);
  installerLockout_ = blob->installerLockout != 0 && installerPinSet_;
  userSalt_ = blob->userSalt;
  userHash_ = blob->userHash;
  installerSalt_ = blob->installerSalt;
  installerHash_ = blob->installerHash;
  // Boot LOCKED: a reboot is not a lock bypass.
  if (installerLockout_) lockState_ = LockState::kInstallerLocked;
  else if (userPinSet_) lockState_ = LockState::kUserLocked;
  else lockState_ = LockState::kUnlocked;
  setDirty(kDirtyLock);
  return true;
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
