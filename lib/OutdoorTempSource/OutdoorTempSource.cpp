#include "OutdoorTempSource.h"

#include <cmath>

namespace dettson {

const char* oatRungName(OatRung rung) {
  switch (rung) {
    case OatRung::kBus:       return "bus";
    case OatRung::kWired:     return "wired";
    case OatRung::kHaWeather: return "ha";
    default:                  return "none";
  }
}

bool OutdoorTempSource::submit(OatRung rung, float valueC, uint32_t nowS) {
  if (rung == OatRung::kNone) return false;
  if (std::isnan(valueC) || valueC < cfg_.rangeMinC || valueC > cfg_.rangeMaxC) {
    return false;  // invalid sample dropped; rung will age out if not refed
  }
  Sample& s = rungs_[static_cast<uint8_t>(rung)];
  s.valueC = valueC;
  s.atS = nowS;
  s.has = true;
  return true;
}

OatReading OutdoorTempSource::read(uint32_t nowS) const {
  OatReading out;
  int prevLive = -1;
  for (int i = 0; i < 3; ++i) {
    if (!live(rungs_[i], nowS)) continue;
    if (!out.valid) {
      out.valid = true;
      out.valueC = rungs_[i].valueC;
      out.rung = static_cast<OatRung>(i);
    } else {
      // Adjacent-in-ladder live pair: cross-check, alarm only — the
      // higher-priority rung is kept (docs/04 §4).
      float d = rungs_[prevLive].valueC - rungs_[i].valueC;
      if (d < 0.0f) d = -d;
      if (d > cfg_.disagreeAlarmC) out.disagreeAlarm = true;
    }
    prevLive = i;
  }
  return out;
}

}  // namespace dettson
