// CopLearner.cpp — see CopLearner.h. Pure C++17, no Arduino.
#include "CopLearner.h"

#include <cmath>
#include <cstdio>

namespace dettson {

size_t CopLearner::bucketFor(float oatC) {
  const float rel = (oatC - kCopBucketMinOatC) / kCopBucketWidthC;
  if (rel <= 0.0f) return 0;
  const size_t i = static_cast<size_t>(rel);
  return i >= kCopBucketCount ? kCopBucketCount - 1 : i;
}

void CopLearner::tick(bool hpHeatActive, float indoorC, bool indoorValid,
                      float oatC, bool oatValid, uint32_t nowS) {
  const bool active = hpHeatActive && indoorValid && oatValid &&
                      std::isfinite(indoorC) && std::isfinite(oatC) &&
                      indoorC > oatC;  // heating regime only
  if (active && prevActive_) {
    uint32_t dt = nowS - lastTickS_;
    if (dt > kCopTickMaxGapS) dt = kCopTickMaxGapS;  // stall/clock-jump cap
    if (dt > 0) {
      const size_t b = bucketFor(oatC);
      runtimeS_[b] += dt;
      totalRuntimeS_ += dt;
      degreeSeconds_[b] +=
          static_cast<double>(indoorC - oatC) * static_cast<double>(dt);
    }
  }
  prevActive_ = active;
  lastTickS_ = nowS;
}

CopLearner::BucketView CopLearner::bucket(size_t i) const {
  BucketView v;
  if (i >= kCopBucketCount) return v;
  v.oatLoC = kCopBucketMinOatC + kCopBucketWidthC * static_cast<float>(i);
  v.runtimeS = runtimeS_[i];
  v.degreeHours = static_cast<float>(degreeSeconds_[i] / 3600.0);
  if (v.runtimeS > 0) {
    // degree-days / runtime-hours = (degSec/86400) / (runS/3600).
    v.ddPerRunHour = static_cast<float>(
        degreeSeconds_[i] / 86400.0 / (static_cast<double>(v.runtimeS) / 3600.0));
  }
  return v;
}

void CopLearner::save(PersistBlob* out) const {
  *out = PersistBlob{};
  for (size_t i = 0; i < kCopBucketCount; ++i) {
    out->runtimeS[i] = runtimeS_[i];
    out->degreeHours[i] = static_cast<float>(degreeSeconds_[i] / 3600.0);
  }
}

bool CopLearner::restore(const PersistBlob* in) {
  if (in == nullptr || in->version != kBlobVersion) return false;
  totalRuntimeS_ = 0;
  for (size_t i = 0; i < kCopBucketCount; ++i) {
    // Plausibility gate on the persisted values (docs/04 §4 spirit): a
    // corrupt blob starts the season fresh instead of poisoning the record.
    if (!std::isfinite(in->degreeHours[i]) || in->degreeHours[i] < 0.0f) {
      for (size_t j = 0; j < kCopBucketCount; ++j) {
        runtimeS_[j] = 0;
        degreeSeconds_[j] = 0.0;
      }
      totalRuntimeS_ = 0;
      return false;
    }
    runtimeS_[i] = in->runtimeS[i];
    degreeSeconds_[i] = static_cast<double>(in->degreeHours[i]) * 3600.0;
    totalRuntimeS_ += in->runtimeS[i];
  }
  prevActive_ = false;  // a reboot is always an accumulation edge
  return true;
}

std::string CopLearner::proxyJson() const {
  std::string js = "{\"bucketC\":";
  char b[96];
  std::snprintf(b, sizeof(b), "%.0f", static_cast<double>(kCopBucketWidthC));
  js += b;
  js += ",\"buckets\":[";
  bool first = true;
  for (size_t i = 0; i < kCopBucketCount; ++i) {
    const BucketView v = bucket(i);
    if (v.runtimeS == 0) continue;
    std::snprintf(b, sizeof(b),
                  "%s{\"oat\":%.0f,\"runS\":%lu,\"degH\":%.1f,\"ddph\":%.3f}",
                  first ? "" : ",", static_cast<double>(v.oatLoC),
                  static_cast<unsigned long>(v.runtimeS),
                  static_cast<double>(v.degreeHours),
                  static_cast<double>(v.ddPerRunHour));
    js += b;
    first = false;
  }
  js += "]}";
  return js;
}

}  // namespace dettson
