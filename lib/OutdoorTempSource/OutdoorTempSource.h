// OutdoorTempSource.h — outdoor temperature rung ladder (issue #34).
//
// Rungs in priority order: CT-485 bus sensor -> wired outdoor DS18B20 ->
// HA weather (docs/04 §4, docs/05 module layout). Each rung carries its own
// value + timestamp; staleness demotes to the next rung; all rungs stale ->
// reading invalid, and the CALLER (DualFuelArbiter) applies the fail-cold
// policy: gas allowed, compressor locked out.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic).

#pragma once
#include <cstdint>
#include "DettsonConfig.h"

namespace dettson {

// Values 0..2 are ladder priority order; kNone reports "no live rung".
enum class OatRung : uint8_t {
  kBus       = 0,
  kWired     = 1,
  kHaWeather = 2,
  kNone      = 3,
};

// String for the slytherm/state/outdoor_source MQTT topic (docs/06).
const char* oatRungName(OatRung rung);

struct OatSourceConfig {
  uint32_t staleS         = kOatStaleS;             // age >= this -> rung dead
  float    disagreeAlarmC = kOatRungDisagreeAlarmC; // adjacent live rungs
  // Plausibility gate. Rejects the DS18B20 sentinels (+85 power-on,
  // -127 disconnect) without special-casing them (docs/04 §4).
  float    rangeMinC      = -50.0f;
  float    rangeMaxC      = 55.0f;
};

struct OatReading {
  bool    valid         = false;
  float   valueC        = 0.0f;
  OatRung rung          = OatRung::kNone;
  bool    disagreeAlarm = false;  // adjacent live rungs differ > threshold
};

class OutdoorTempSource {
 public:
  OutdoorTempSource() = default;
  explicit OutdoorTempSource(const OatSourceConfig& cfg) : cfg_(cfg) {}

  // Feed one rung. Returns false (sample dropped, rung unchanged) on
  // NaN/out-of-range value or rung == kNone.
  bool submit(OatRung rung, float valueC, uint32_t nowS);

  // Evaluate the ladder at nowS. Highest-priority live rung wins; a
  // disagreement with the next live rung raises the alarm flag but the
  // higher-priority rung is kept (docs/04 §4).
  OatReading read(uint32_t nowS) const;

  const OatSourceConfig& config() const { return cfg_; }

 private:
  struct Sample {
    float    valueC = 0.0f;
    uint32_t atS    = 0;
    bool     has    = false;
  };

  bool live(const Sample& s, uint32_t nowS) const {
    return s.has && (nowS - s.atS) < cfg_.staleS;
  }

  Sample          rungs_[3];
  OatSourceConfig cfg_;
};

}  // namespace dettson
