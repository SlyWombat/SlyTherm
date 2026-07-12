// FanCirculate.cpp — see FanCirculate.h (issue #128).
#include "FanCirculate.h"

#include <cmath>

namespace dettson {
namespace fan {

uint32_t clampCirculateMinPerHour(long minPerHour) {
  if (minPerHour < 0) return 0u;
  if (minPerHour > static_cast<long>(kCirculateMinPerHourMax))
    return kCirculateMinPerHourMax;
  return static_cast<uint32_t>(minPerHour);
}

float snapCirculatePct(float pct) {
  // Nearest of {Low, Med, High} by absolute distance. NaN/junk -> Low (the
  // quietest/cheapest default, matching the compile-time default).
  if (!(pct == pct)) return kSpeedLowPct;  // NaN
  const float dLow  = std::fabs(pct - kSpeedLowPct);
  const float dMed  = std::fabs(pct - kSpeedMedPct);
  const float dHigh = std::fabs(pct - kSpeedHighPct);
  if (dLow <= dMed) return dLow <= dHigh ? kSpeedLowPct : kSpeedHighPct;
  return dMed <= dHigh ? kSpeedMedPct : kSpeedHighPct;
}

float circulateRequestPct(uint32_t nowS, uint32_t minPerHour, float pct,
                          uint32_t creditS) {
  const uint32_t winS = minPerHour * 60u;
  const uint32_t effS = winS > creditS ? winS - creditS : 0u;
  return (nowS % 3600u) < effS ? pct : 0.0f;
}

}  // namespace fan
}  // namespace dettson
