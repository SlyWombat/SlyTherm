// UiModel.h — view-model decoupling the touchscreen UI from control logic.
//
// The UI renders a DisplayState snapshot (filled by the control task) and
// issues user actions through UiCommands. Commands NEVER write control state:
// they update the local display echo and enqueue UiIntents that the control
// task consumes and re-validates. A crashed/garbage UI can at worst enqueue
// intents that get rejected — demand authority stays in the control task
// (docs/04-safety.md §1c UI failure isolation).
//
// Pure C++17, no Arduino/LVGL dependencies. Time is injected where needed.
// This module survives the #38 ESPHome-vs-PlatformIO decision; only the
// LVGL binding (ui/lvgl/) changes.

#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include "DettsonConfig.h"

namespace dettson {
namespace ui {

// ---------- Enums ----------
enum class UserMode : uint8_t { kOff, kHeat, kCool, kAuto, kEmergencyHeat };
enum class HvacAction : uint8_t { kIdle, kHeating, kCooling, kFanOnly, kDefrosting };
enum class Preset : uint8_t { kHome, kAway, kSleep };
enum class OutdoorSource : uint8_t { kNone, kBus, kWiredDs18b20, kHaWeather };
enum class SetpointSide : uint8_t { kHeat, kCool };

// ---------- Screen lock (issue #45) ----------
// SAFETY RULE (docs/04 §1c + §3 alarming): the lock blocks CHANGE INTENTS
// only. It never gates DisplayState — alarms, current temp, action, and
// every status group stay visible and keep re-rendering at every lock level.
// Alarm *acknowledgement* is a change intent and stays locked.
enum class LockState : uint8_t {
  kUnlocked,         // change intents allowed (installer pages still gated)
  kUserLocked,       // user PIN or installer code unlocks
  kInstallerLocked,  // installer lockout: ONLY the installer code unlocks
};
enum class LockLevel : uint8_t {
  kSettingsOnly,          // default: locked = mode/settings/ack blocked, setpoints free
  kSettingsAndSetpoints,  // locked = setpoint/preset changes blocked too
};
enum class PinContext : uint8_t {
  kNone,               // no entry in progress
  kUnlock,             // unlocking the screen lock
  kInstallerSettings,  // gaining access to installer settings pages
};
enum class PinResult : uint8_t {
  kIdle,      // no entry in progress (digit ignored)
  kPending,   // digit accepted, more needed
  kAccepted,  // PIN matched
  kRejected,  // PIN wrong, attempts remain
  kBackoff,   // attempts exhausted — entry blocked until the backoff expires
};
constexpr size_t kPinLen = 4;

// Salted FNV-1a over the PIN digits. This is tamper resistance against
// casual users reading an NVS dump — NOT cryptography (a 4-digit space is
// trivially brute-forceable offline). The salt only keeps the stored word
// from being a recognizable constant across devices.
uint32_t pinHash(uint32_t salt, const uint8_t digits[kPinLen]);

// Active equipment bitmask (multiple may be lit, e.g. defrost tempering).
constexpr uint8_t kEquipNone   = 0;
constexpr uint8_t kEquipGas    = 1u << 0;
constexpr uint8_t kEquipHpHeat = 1u << 1;
constexpr uint8_t kEquipHpCool = 1u << 2;
constexpr uint8_t kEquipAux    = 1u << 3;
constexpr uint8_t kEquipFan    = 1u << 4;

// ---------- Capacities (fixed; no heap) ----------
constexpr size_t kMaxSensorRows  = 8;
constexpr size_t kSensorNameLen  = 16;
constexpr size_t kMaxAlarms      = 8;
constexpr size_t kAlarmTextLen   = 40;
constexpr size_t kIntentQueueCap = 8;

struct SensorRow {
  char     name[kSensorNameLen] = {};
  float    tempC         = 0.0f;
  bool     occupied      = false;
  uint32_t ageS          = 0;
  bool     participating = false;
  bool     healthy       = false;
};

struct Alarm {
  char     text[kAlarmTextLen] = {};
  uint16_t code = 0;
};

// ---------- Dirty groups: one bit per render group ----------
enum DirtyGroup : uint16_t {
  kDirtyTemp      = 1u << 0,   // fused temp + validity
  kDirtySetpoints = 1u << 1,
  kDirtyMode      = 1u << 2,
  kDirtyAction    = 1u << 3,
  kDirtyEquipment = 1u << 4,
  kDirtySensors   = 1u << 5,
  kDirtyOutdoor   = 1u << 6,
  kDirtyLockout   = 1u << 7,
  kDirtyGasMod    = 1u << 8,
  kDirtyAlarms    = 1u << 9,
  kDirtyHealth    = 1u << 10,  // wifi/mqtt/bus + degraded flag
  kDirtyLock      = 1u << 11,  // lock state / PIN entry progress / backoff
  kDirtyAll       = 0x0FFF,
};

// ---------- DisplayState: everything the screens render ----------
struct DisplayState {
  float fusedTempC     = 0.0f;
  bool  fusedTempValid = false;

  float heatSetpointC = kFallbackHeatSetpointC;
  float coolSetpointC = kFallbackCoolSetpointC;

  UserMode   mode   = UserMode::kOff;     // boot = no demand (docs/04 §3 boot validation)
  HvacAction action = HvacAction::kIdle;
  uint8_t    activeEquipment = kEquipNone;

  SensorRow sensors[kMaxSensorRows] = {};
  uint8_t   sensorCount = 0;

  float         outdoorTempC  = 0.0f;
  bool          outdoorValid  = false;
  OutdoorSource outdoorSource = OutdoorSource::kNone;

  uint32_t compressorLockoutRemainS = 0;
  float    gasModulationPct         = 0.0f;

  Alarm   alarms[kMaxAlarms] = {};
  uint8_t alarmCount    = 0;
  bool    alarmsDropped = false;   // bounded list overflowed; oldest discarded

  bool wifiOk = false;
  bool mqttOk = false;
  bool busOk  = false;
  bool degradedMode = false;       // DS18B20-only degraded mode banner
};

// ---------- Intents: the only thing the UI hands to control ----------
enum class IntentType : uint8_t { kSetSetpoints, kSetMode, kSetPreset, kAckAlarms };

struct UiIntent {
  IntentType type   = IntentType::kSetSetpoints;
  float      heatC  = 0.0f;   // kSetSetpoints: both values, post clamp+push
  float      coolC  = 0.0f;
  UserMode   mode   = UserMode::kOff;
  Preset     preset = Preset::kHome;
};

// ---------- Deadband clamp+push ----------
// DUPLICATED from ModeStateMachine on purpose: UiModel must not depend on the
// control lib (UI isolation, docs/04 §1c). Keep the two in sync. Rule per
// docs/05 defaults table: cool >= heat + minDelta; the moved setpoint wins
// and pushes the other Ecobee-style; minDelta never below the hard floor.
struct Setpoints {
  float heatC;
  float coolC;
};

inline Setpoints applyDeadbandClampPush(Setpoints sp, SetpointSide moved, float minDeltaC) {
  if (!(minDeltaC >= kMinSetpointDeltaFloorC)) minDeltaC = kMinSetpointDeltaFloorC;
  if (sp.coolC - sp.heatC < minDeltaC) {
    if (moved == SetpointSide::kHeat) sp.coolC = sp.heatC + minDeltaC;
    else                              sp.heatC = sp.coolC - minDeltaC;
  }
  return sp;
}

// ---------- UiCommands: the interface screens call ----------
class UiCommands {
 public:
  virtual ~UiCommands() = default;
  virtual void adjustSetpoint(SetpointSide side, float deltaC) = 0;
  virtual void setMode(UserMode mode) = 0;
  virtual void setPreset(Preset preset) = 0;
  virtual void ackAlarms() = 0;  // acknowledgement only — visibility is never gated
};

// ---------- Config ----------
struct UiModelConfig {
  float minSetpointDeltaC = kMinSetpointDeltaC;  // clamped to kMinSetpointDeltaFloorC at use
  // UI affordance limits only — NOT safety limits; the control task re-validates
  // every intent against its own bounds.
  float setpointMinC = 10.0f;
  float setpointMaxC = 35.0f;
  float setpointStepC = 0.5f;  // suggested increment for arc/buttons (binding hint)

  // Screen lock (issue #45; docs/05 defaults table).
  uint32_t autoRelockS    = kUiAutoRelockS;
  uint8_t  pinMaxAttempts = kUiPinMaxAttempts;
  uint32_t pinBackoffS    = kUiPinBackoffS;
};

// ---------- UiModel ----------
class UiModel : public UiCommands {
 public:
  explicit UiModel(const UiModelConfig& cfg = UiModelConfig{});

  // --- render side ---
  const DisplayState& state() const { return state_; }
  uint16_t dirty() const { return dirty_; }
  void clearDirty(uint16_t groups = kDirtyAll) { dirty_ &= static_cast<uint16_t>(~groups); }
  void markAllDirty() { dirty_ = kDirtyAll; ++generation_; }
  uint32_t generation() const { return generation_; }  // bumps on any change

  // --- control-task side: authoritative state echo ---
  void setFusedTemp(float tempC, bool valid);
  void setSetpoints(float heatC, float coolC);
  void setUserMode(UserMode mode);
  void setHvacAction(HvacAction action);
  void setActiveEquipment(uint8_t mask);
  void setSensorRows(const SensorRow* rows, uint8_t count);
  void setOutdoor(float tempC, bool valid, OutdoorSource source);
  void setCompressorLockoutRemain(uint32_t seconds);
  void setGasModulationPct(float pct);
  void pushAlarm(const char* text, uint16_t code);
  void clearAlarms();
  void setLinkHealth(bool wifi, bool mqtt, bool bus);
  void setDegradedMode(bool on);
  void setMinSetpointDelta(float deltaC);  // runtime-tunable, floor-clamped

  // --- UiCommands (screen event handlers call these) ---
  void adjustSetpoint(SetpointSide side, float deltaC) override;
  void setMode(UserMode mode) override;
  void setPreset(Preset preset) override;
  void ackAlarms() override;

  // --- screen lock (issue #45) ---
  // POD, fixed layout, same NVS-blob discipline as CompressorGuard: caller
  // owns storage; crc covers every byte after the crc field. A missing or
  // corrupt blob restores to "no PINs, unlocked" — the lock is a comfort/
  // tamper-resistance feature, never a safety layer, so it fails OPEN
  // (an NVS wipe must not brick the wall UI).
  struct LockPersistBlob {
    uint32_t magic   = 0;
    uint16_t version = 0;
    uint16_t crc     = 0;
    uint8_t  userPinSet      = 0;
    uint8_t  installerPinSet = 0;
    uint8_t  lockLevel       = 0;  // LockLevel
    uint8_t  installerLockout = 0;
    uint32_t userSalt      = 0;
    uint32_t userHash      = 0;
    uint32_t installerSalt = 0;
    uint32_t installerHash = 0;
  };
  static constexpr uint32_t kLockBlobMagic   = 0x554C434Bu;  // "ULCK"
  static constexpr uint16_t kLockBlobVersion = 1;

  // PIN management. Salts are caller-injected (this module has no RNG).
  // Setters are trusted: screens expose them only behind the appropriate
  // unlock; the lock gates intents, not its own configuration API.
  void setUserPin(const uint8_t digits[kPinLen], uint32_t salt);
  void setInstallerPin(const uint8_t digits[kPinLen], uint32_t salt);
  // Forgotten-PIN recovery (HA cmd topic, docs/06): clears the user PIN with
  // no PIN required and releases a user lock. Deliberately does NOT release
  // an installer lockout or touch the installer code.
  void clearUserPin();
  void setLockLevel(LockLevel level);

  bool lockNow(uint32_t nowS);                       // -> kUserLocked; needs a user PIN
  bool setInstallerLockout(bool on, uint32_t nowS);  // on needs an installer code

  // PIN entry flow: begin with a context, feed digits 0-9; the kPinLen-th
  // digit evaluates AND ends the entry (begin again for another attempt).
  // Wrong PINs decrement attempts; exhausted attempts start the backoff
  // timer (entry rejected until it expires, then attempts reset).
  void beginPinEntry(PinContext ctx, uint32_t nowS);
  PinResult enterPinDigit(uint8_t digit, uint32_t nowS);
  void cancelPinEntry();

  // Any user interaction (touch) defers the auto-relock; call tick() ~1 Hz to
  // run the relock/backoff clocks.
  void touchActivity(uint32_t nowS) { lastActivityS_ = nowS; }
  void tick(uint32_t nowS);

  LockState lockState() const { return lockState_; }
  LockLevel lockLevel() const { return lockLevel_; }
  bool userPinSet() const { return userPinSet_; }
  bool installerPinSet() const { return installerPinSet_; }
  bool installerLockout() const { return installerLockout_; }
  bool installerAccess() const { return installerAccess_; }  // installer pages gate
  PinContext pinContext() const { return pinCtx_; }
  uint8_t pinDigitsEntered() const { return pinCount_; }
  uint8_t attemptsRemaining() const { return attemptsLeft_; }
  uint32_t backoffRemainS(uint32_t nowS) const;
  // Intent classes vs lock level: setpoint/preset changes pass at
  // kSettingsOnly; mode/settings/alarm-ack need kUnlocked.
  bool setpointChangeAllowed() const {
    return lockState_ == LockState::kUnlocked || lockLevel_ == LockLevel::kSettingsOnly;
  }
  bool settingsChangeAllowed() const { return lockState_ == LockState::kUnlocked; }
  uint32_t lockBlockedCommandCount() const { return lockBlockedCommands_; }

  void saveLock(LockPersistBlob* out) const;
  // Boot restore: a restored lock comes back LOCKED (installer lockout wins
  // over user lock) — a reboot must not be a lock bypass.
  bool restoreLock(const LockPersistBlob* blob, uint32_t nowS);

  // --- intent queue (control task consumes) ---
  bool popIntent(UiIntent& out);
  size_t pendingIntents() const { return queueCount_; }
  bool intentOverflowed() const { return intentOverflow_; }  // cleared by popIntent draining
  void clearIntentOverflow() { intentOverflow_ = false; }
  uint32_t droppedIntentCount() const { return droppedIntents_; }
  uint32_t rejectedCommandCount() const { return rejectedCommands_; }

 private:
  void setDirty(uint16_t group) { dirty_ |= group; ++generation_; }
  void enqueue(const UiIntent& intent);  // full queue drops OLDEST + flags
  static uint16_t lockBlobCrc(const LockPersistBlob& b);
  bool inBackoff(uint32_t nowS) const;
  void wipePinEntry();
  void relock();  // -> installer lockout target, else user lock, else stays

  UiModelConfig cfg_;
  DisplayState  state_;
  uint16_t      dirty_      = kDirtyAll;  // first render draws everything
  uint32_t      generation_ = 0;

  UiIntent queue_[kIntentQueueCap] = {};
  size_t   queueHead_  = 0;  // index of oldest
  size_t   queueCount_ = 0;
  bool     intentOverflow_   = false;
  uint32_t droppedIntents_   = 0;
  uint32_t rejectedCommands_ = 0;

  // --- screen lock ---
  LockState lockState_ = LockState::kUnlocked;  // no PIN configured = no lock
  LockLevel lockLevel_ = LockLevel::kSettingsOnly;
  bool      userPinSet_       = false;
  bool      installerPinSet_  = false;
  bool      installerLockout_ = false;  // persistent relock target
  uint32_t  userSalt_ = 0, userHash_ = 0;
  uint32_t  installerSalt_ = 0, installerHash_ = 0;
  PinContext pinCtx_   = PinContext::kNone;
  uint8_t    pinBuf_[kPinLen] = {};
  uint8_t    pinCount_ = 0;
  uint8_t    attemptsLeft_;          // set from cfg in ctor
  bool       backoffActive_ = false;
  uint32_t   backoffEndS_   = 0;
  bool       installerAccess_ = false;  // installer pages session; relock expires it
  uint32_t   lastActivityS_   = 0;
  uint32_t   lockBlockedCommands_ = 0;
};

}  // namespace ui
}  // namespace dettson
