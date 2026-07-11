// CopLearner.h — record-only COP-proxy field learning (issue #143, docs/13 §5).
//
// Accumulates, per 3 °C OAT bucket, the heat pump's HEAT runtime and the
// indoor-outdoor delta integrated over that runtime. The ratio —
// delta-degree-DAYS per runtime-HOUR — is a hardware-free proxy for delivered
// capacity: at a given OAT, a machine that needs fewer runtime hours per
// degree-day is delivering more heat per hour. Field studies (docs/13 §5:
// ACEEE cold-climate monitoring, NRCan ccASHP assessment) show installed
// performance deviates from nameplate (defrost alone adds 5-15%), which is
// why the #143 economic switchover's seed COP curve needs field correction.
//
// RECORD-ONLY (docs/13 §5: "telemetry first, optimization after a season").
// Nothing here feeds DualFuelConfig::copCurve. Telemetry surfaces are the
// periodic [copx] telnet line and the retained slytherm/state/cop_proxy JSON.
//
// >>> CORRECTION SEAM (deliberately not implemented) <<<
// After a full heating season of buckets, the closing step is: derive a
// per-bucket relative capacity from ddPerRunHour (buckets with comparable
// duty), normalize against the bucket containing the +8.3 °C rating point,
// scale by nameplate COP there, and write the corrected points back through
// DualFuelArbiter::setConfig(). That lands as a follow-up with its own review
// — a learning bug must never be able to steer switchover this season.
//
// Pure C++17, no Arduino. Time is injected as uint32_t nowS (monotonic).

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "DettsonConfig.h"

namespace dettson {

class CopLearner {
 public:
  // Feed once per control tick. Accumulates only while ALL of: the HP is
  // serving a heat demand, both temperatures valid, and indoor > outdoor
  // (heating regime — a shoulder-season inversion sample would corrupt the
  // proxy). dt is capped at kCopTickMaxGapS so a stalled loop or clock jump
  // can't dump phantom hours, and the first active tick after any idle/invalid
  // period is an edge (dt attributed only between consecutive active ticks).
  void tick(bool hpHeatActive, float indoorC, bool indoorValid, float oatC,
            bool oatValid, uint32_t nowS);

  struct BucketView {
    float    oatLoC = 0.0f;       // bucket lower edge; spans kCopBucketWidthC
    uint32_t runtimeS = 0;
    float    degreeHours = 0.0f;  // integral of (indoor - oat) dt, in degC*h
    float    ddPerRunHour = 0.0f; // (degreeHours/24) / (runtimeS/3600); 0 if empty
  };
  BucketView bucket(size_t i) const;             // i < kCopBucketCount
  uint32_t totalRuntimeS() const { return totalRuntimeS_; }

  // Compact NVS blob (~140 bytes). restore() rejects a version mismatch and
  // returns false (fresh season, not corrupted math).
  struct PersistBlob {
    uint32_t version = kBlobVersion;
    uint32_t runtimeS[kCopBucketCount] = {};
    float    degreeHours[kCopBucketCount] = {};
  };
  static constexpr uint32_t kBlobVersion = 1;
  void save(PersistBlob* out) const;
  bool restore(const PersistBlob* in);

  // Retained slytherm/state/cop_proxy payload (also the [copx] telnet line):
  //   {"bucketC":3,"buckets":[{"oat":-12,"runS":8130,"degH":61.2,"ddph":1.13},...]}
  // "oat" is the bucket lower edge; only non-empty buckets are listed.
  std::string proxyJson() const;

 private:
  static size_t bucketFor(float oatC);

  // degreeSeconds in double: a season is ~1e8 degC*s and 1 Hz increments are
  // ~30 — float would stop accumulating around week two.
  double   degreeSeconds_[kCopBucketCount] = {};
  uint32_t runtimeS_[kCopBucketCount] = {};
  uint32_t totalRuntimeS_ = 0;
  uint32_t lastTickS_ = 0;
  bool     prevActive_ = false;  // active = hpHeat + both temps valid + regime
};

}  // namespace dettson
