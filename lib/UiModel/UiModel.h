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
  kDirtyAll       = 0x07FF,
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
enum class IntentType : uint8_t { kSetSetpoints, kSetMode, kSetPreset };

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
};

// ---------- Config ----------
struct UiModelConfig {
  float minSetpointDeltaC = kMinSetpointDeltaC;  // clamped to kMinSetpointDeltaFloorC at use
  // UI affordance limits only — NOT safety limits; the control task re-validates
  // every intent against its own bounds.
  float setpointMinC = 10.0f;
  float setpointMaxC = 35.0f;
  float setpointStepC = 0.5f;  // suggested increment for arc/buttons (binding hint)
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
};

}  // namespace ui
}  // namespace dettson
