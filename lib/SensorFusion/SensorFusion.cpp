// SensorFusion.cpp — see SensorFusion.h and docs/04 §4.

#include "SensorFusion.h"

#include <cmath>

namespace dettson {

namespace {

inline uint32_t ageOf(uint32_t nowS, uint32_t thenS) {
  return nowS >= thenS ? nowS - thenS : 0;  // clock went backwards -> treat as fresh
}

inline uint32_t clampU32(uint32_t v, uint32_t lo, uint32_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

inline float expDecayAlpha(uint32_t dtS, uint32_t tauS) {
  if (dtS == 0) return 0.0f;
  if (tauS == 0) return 1.0f;
  return 1.0f - std::exp(-static_cast<float>(dtS) / static_cast<float>(tauS));
}

}  // namespace

SensorFusion::Slot* SensorFusion::find(uint8_t id) {
  for (auto& s : slots_)
    if (s.used && s.id == id) return &s;
  return nullptr;
}

const SensorFusion::Slot* SensorFusion::find(uint8_t id) const {
  for (const auto& s : slots_)
    if (s.used && s.id == id) return &s;
  return nullptr;
}

bool SensorFusion::registerSensor(uint8_t id, bool isLocal, bool participating,
                                  uint32_t maxAgeS) {
  if (find(id) != nullptr) return false;
  for (auto& s : slots_) {
    if (s.used) continue;
    s = Slot{};
    s.used = true;
    s.id = id;
    s.isLocal = isLocal;
    s.participating = participating;
    s.maxAgeS = clampU32(maxAgeS, kSensorMaxAgeMinS, kSensorMaxAgeMaxS);
    return true;
  }
  return false;  // table full
}

bool SensorFusion::setParticipating(uint8_t id, bool participating) {
  Slot* s = find(id);
  if (!s) return false;
  s->participating = participating;
  return true;
}

bool SensorFusion::setSensorMaxAgeS(uint8_t id, uint32_t maxAgeS) {
  Slot* s = find(id);
  if (!s) return false;
  s->maxAgeS = clampU32(maxAgeS, kSensorMaxAgeMinS, kSensorMaxAgeMaxS);
  return true;
}

bool SensorFusion::setSensorOffsetC(uint8_t id, float offsetC) {
  Slot* s = find(id);
  if (!s || !std::isfinite(offsetC)) return false;
  if (offsetC > kSensorOffsetMaxC) offsetC = kSensorOffsetMaxC;
  if (offsetC < -kSensorOffsetMaxC) offsetC = -kSensorOffsetMaxC;
  // An offset edit shifts this sensor's corrected value instantly — treat it
  // like a participant-set change so the output ramps instead of stepping.
  if (offsetC != s->offsetC && hasOutput_) slewActive_ = true;
  s->offsetC = offsetC;
  return true;
}

bool SensorFusion::update(uint8_t id, float tempC, Occupancy occ, uint32_t nowS) {
  Slot* s = find(id);
  if (!s) return false;
  // Stuck tracking: exact repeats only — any change resets the window.
  if (!s->hasUpdate || !(tempC == s->lastDistinctTempC)) {
    s->lastDistinctTempC = tempC;
    s->lastChangeS = nowS;
    // #153: a new flat run begins — snapshot every peer's current RAW value.
    // Peer movement since this instant is the disagreement evidence for the
    // stuck test. Raw (not corrected) so a later offset edit on either side
    // is invisible to the detector, consistent with the raw repeat tracking.
    s->peerSnapMask = 0;
    for (size_t j = 0; j < kMaxSensors; ++j) {
      const Slot& p = slots_[j];
      if (&p == s || !p.used || !p.hasUpdate) continue;
      s->peerSnapC[j] = p.tempC;
      s->peerSnapMask |= static_cast<uint16_t>(1u << j);
    }
  }
  s->tempC = tempC;
  s->lastUpdateS = nowS;
  s->hasUpdate = true;
  if (occ == Occupancy::kOccupied) {
    s->everOccupied = true;
    s->lastOccS = nowS;
  }
  return true;
}

void SensorFusion::updatePresence(uint8_t id, bool occupied, uint32_t lastSeenS,
                                  bool hasLastSeen, uint32_t nowS) {
  Slot* s = find(id);
  if (!s) return;
  s->presenceReported = true;
  s->occupiedState = occupied;
  if (hasLastSeen) {
    // Monotonic, non-decreasing: an out-of-order (older) retained message must
    // not walk the ledger backwards.
    if (!s->presenceSeen || lastSeenS > s->lastSeenS) s->lastSeenS = lastSeenS;
    s->presenceSeen = true;
  } else if (occupied) {
    // No timestamp but HA says occupied now -> "seen now".
    if (!s->presenceSeen || nowS > s->lastSeenS) s->lastSeenS = nowS;
    s->presenceSeen = true;
  }
  // Vacant with no timestamp: recorded as reporting, but not placed in time.
}

PresenceState SensorFusion::presenceWithin(uint32_t windowS, uint32_t nowS) const {
  PresenceState ps;
  bool haveNewest = false;
  uint32_t newest = 0;
  uint8_t dom = 0xFF;
  for (const auto& s : slots_) {
    if (!s.used || s.isLocal) continue;  // local DS18B20 has no presence
    if (s.presenceReported) ps.anyReporting = true;
    if (!s.presenceSeen) continue;
    ps.everSeen = true;
    if (!haveNewest || s.lastSeenS >= newest) {
      newest = s.lastSeenS;
      dom = s.id;
      haveNewest = true;
    }
  }
  if (haveNewest) {
    ps.lastSeenAgeS = ageOf(nowS, newest);
    ps.present = ps.lastSeenAgeS < windowS;
    ps.dominantId = dom;
  }
  return ps;
}

PresenceState SensorFusion::presence(uint32_t nowS) const {
  return presenceWithin(kPresenceAwayS, nowS);
}

FusedTemp SensorFusion::fusedTemp(uint32_t nowS) {
  uint32_t dt = 0;
  if (hasTime_ && nowS > lastNowS_) dt = nowS - lastNowS_;
  lastNowS_ = nowS;
  hasTime_ = true;

  FusedTemp out;

  // --- Per-sensor health gates (docs/04 §4: all mandatory) ---
  size_t liveIdx[kMaxSensors];
  size_t nLive = 0;
  for (size_t i = 0; i < kMaxSensors; ++i) {
    Slot& s = slots_[i];
    s.live = false;
    s.faults = 0;
    if (!s.used) continue;
    if (!s.isLocal && !s.participating) continue;  // out of profile: ignored, no alarms
    if (!s.hasUpdate) continue;                    // boot quiet: not yet an alarm
    if (ageOf(nowS, s.lastUpdateS) > s.maxAgeS) {
      s.faults |= kAlarmStale;
    } else if (!(s.correctedC() >= kSensorRangeMinC &&
                 s.correctedC() <= kSensorRangeMaxC)) {
      s.faults |= kAlarmRange;  // NaN fails both comparisons -> rejected here
    } else if (const uint32_t flatAgeS = ageOf(nowS, s.lastChangeS);
               flatAgeS > stuckCeilingS_ ||
               (flatAgeS > stuckWindowS_ && peersMovedMajority(s, nowS))) {
      // Stuck-value policy (#153 redesign). These sensors publish at 0.1 C
      // steps, so a bit-identical reading for hours is the EXPECTED signature
      // of a regulated room drifting <0.1 C/h — flatness alone is never a
      // fault (field: three 07-10 overnight dropouts were exactly this).
      // Declared stuck only when the flatness is IMPLAUSIBLE:
      //  (a) flat past the suspect window while a MAJORITY of comparable
      //      peers moved >= stuckPeerDeltaC_ since this sensor's last change
      //      — the wedged-driver signature (the house moved, it didn't); or
      //  (b) flat past the absolute ceiling regardless of peers — the only
      //      trigger for a single-participant install.
      // When all participants are flat and mutually consistent (stable house
      // overnight), nobody's peers moved, so nobody trips (a); and the most-
      // recently-changed participant can never trip (a), so the peer rule
      // alone can never drive fusion to kAlarmAllBad. Stuck tracks raw
      // repeats, so an offset edit can neither trip nor mask the detector
      // (docs/07 G6).
      s.faults |= kAlarmStuck;
    } else {
      s.live = true;
      if (!s.isLocal) liveIdx[nLive++] = i;
    }
  }

  // --- Outlier exclusion vs live-participant median (docs/04 §4) ---
  // Needs >=3 participants: with 2 the median is the midpoint and both sensors
  // are equidistant from it — no way to tell which is wrong, so never kill.
  if (nLive >= 3) {
    float sorted[kMaxSensors];
    for (size_t k = 0; k < nLive; ++k) sorted[k] = slots_[liveIdx[k]].correctedC();
    for (size_t a = 1; a < nLive; ++a) {  // insertion sort, N <= kMaxSensors
      float v = sorted[a];
      size_t b = a;
      while (b > 0 && sorted[b - 1] > v) { sorted[b] = sorted[b - 1]; --b; }
      sorted[b] = v;
    }
    const float median = (nLive % 2 == 1)
                             ? sorted[nLive / 2]
                             : 0.5f * (sorted[nLive / 2 - 1] + sorted[nLive / 2]);
    size_t kept = 0;
    for (size_t k = 0; k < nLive; ++k) {
      Slot& s = slots_[liveIdx[k]];
      if (std::fabs(s.correctedC() - median) > outlierC_) {
        s.faults |= kAlarmOutlier;
        s.live = false;
      } else {
        liveIdx[kept++] = liveIdx[k];
      }
    }
    nLive = kept;
  }

  // --- Occupancy weight ramp (exponential, continuous — no steps) ---
  const float rampAlpha = expDecayAlpha(dt, weightRampTauS_);
  for (auto& s : slots_) {
    if (!s.used || s.isLocal) continue;
    const bool occupied =
        s.everOccupied && ageOf(nowS, s.lastOccS) <= occupancyWindowS_;
    const float target = occupied ? occupiedWeight_ : 1.0f;
    s.weight += (target - s.weight) * rampAlpha;
  }

  // --- Fallback ladder (docs/04 §4) ---
  uint16_t mask = 0;
  float raw = 0.0f;
  bool haveRaw = false;
  const Slot* liveLocal = nullptr;
  for (size_t i = 0; i < kMaxSensors; ++i)
    if (slots_[i].used && slots_[i].isLocal && slots_[i].live && !liveLocal)
      liveLocal = &slots_[i];

  if (nLive >= 1) {
    float wSum = 0.0f, wtSum = 0.0f;
    for (size_t k = 0; k < nLive; ++k) {
      const Slot& s = slots_[liveIdx[k]];
      mask |= static_cast<uint16_t>(1u << liveIdx[k]);
      wSum += s.weight;
      wtSum += s.weight * s.correctedC();
    }
    raw = wtSum / wSum;
    haveRaw = true;
    out.tier = (nLive >= 2) ? SourceTier::kFusedRemotes : SourceTier::kSingleRemote;
    // Local-vs-fusion sanity even while fusion is healthy (docs/04 §4).
    if (liveLocal && std::fabs(liveLocal->correctedC() - raw) > localDisagreeC_)
      out.alarms |= kAlarmLocalDisagree;
  } else if (liveLocal) {
    raw = liveLocal->correctedC();
    mask = static_cast<uint16_t>(1u << (liveLocal - slots_));
    haveRaw = true;
    out.tier = SourceTier::kLocalDegraded;
    out.degraded = true;  // caller: bound setpoints, disable cooling (kDegraded*)
    out.alarms |= kAlarmDegraded;
  }

  for (const auto& s : slots_)
    if (s.used) out.alarms |= s.faults;

  if (!haveRaw) {
    // Coast-on-last-good grace (#153): a JUST-lapsed fusion (field case: the
    // stuck window expiring on flat overnight temps; also heartbeat hiccups
    // at the staleness edge) keeps reporting the last good output for up to
    // coastMaxS_ instead of forcing the caller's safety stop. Tier/degraded
    // carry from the last good evaluation so a coast off the local-degraded
    // rung keeps its cooling lockout. Smoothing state is retained: recovery
    // resumes continuously from the coasted value. Hard-invalid beyond the
    // grace, on an implausible last-good, or at boot (no last-good) — a REAL
    // prolonged failure still fails to no-demand (docs/04 §3).
    if (hasOutput_ && coastMaxS_ > 0 && ageOf(nowS, lastGoodS_) <= coastMaxS_ &&
        coastPlausible(output_)) {
      out.value = output_;
      out.valid = true;
      out.coasting = true;
      out.tier = lastGoodTier_;
      out.degraded = (lastGoodTier_ == SourceTier::kLocalDegraded);
      out.alarms |= kAlarmCoasting;
      if (out.degraded) out.alarms |= kAlarmDegraded;
      return out;
    }
    // All bad -> invalid; caller must go to no-demand (docs/04 §3).
    // Smoothing state resets: nothing meaningful to be continuous with, and
    // recovery re-seeds from truth (compressor timers live in CompressorGuard).
    out.alarms |= kAlarmAllBad;
    out.valid = false;
    out.tier = SourceTier::kNone;
    hasOutput_ = false;
    slewActive_ = false;
    lastMask_ = 0;
    return out;
  }

  if (!hasOutput_) {
    ema_ = raw;
    output_ = raw;
    hasOutput_ = true;
    slewActive_ = false;
    lastMask_ = mask;
  } else {
    if (mask != lastMask_) {
      slewActive_ = true;  // a sensor joined/left: rate-limit until caught up
      lastMask_ = mask;
    }
    ema_ += (raw - ema_) * expDecayAlpha(dt, smoothingTauS_);
    if (slewActive_) {
      const float maxDelta = slewCPerMin_ * static_cast<float>(dt) / 60.0f;
      float clamped = ema_;
      if (clamped > output_ + maxDelta) clamped = output_ + maxDelta;
      if (clamped < output_ - maxDelta) clamped = output_ - maxDelta;
      if (clamped == ema_) slewActive_ = false;
      output_ = clamped;
    } else {
      output_ = ema_;
    }
  }

  out.value = output_;
  out.valid = true;
  lastGoodS_ = nowS;      // #153 coast anchors
  lastGoodTier_ = out.tier;
  return out;
}

bool SensorFusion::peersMovedMajority(const Slot& s, uint32_t nowS) const {
  // Comparable peers: fused-relevant remotes (participating, non-local) that
  // were already reporting when s's flat run began (snapshot exists) and are
  // currently fresh and in corrected range. The local DS18B20 is deliberately
  // NOT movement evidence: unit self-heating can swing it while the rooms sit
  // still, and a false stuck verdict from it would push fusion toward the
  // degraded rung. A peer's own stuck state is irrelevant here — if it moved,
  // it isn't flat; if it's flat, it simply counts as not-moved.
  size_t comparable = 0, moved = 0;
  for (size_t j = 0; j < kMaxSensors; ++j) {
    const Slot& p = slots_[j];
    if (&p == &s || !p.used || p.isLocal || !p.participating || !p.hasUpdate)
      continue;
    if (!(s.peerSnapMask & static_cast<uint16_t>(1u << j))) continue;
    if (ageOf(nowS, p.lastUpdateS) > p.maxAgeS) continue;  // stale: no evidence
    if (!(p.correctedC() >= kSensorRangeMinC && p.correctedC() <= kSensorRangeMaxC))
      continue;
    ++comparable;
    // 1e-3 headroom: deltas of 0.1 C-quantized floats land within ~1e-5 of
    // ideal — far below one quantization step, so this can't misclassify.
    if (std::fabs(p.tempC - s.peerSnapC[j]) + 1e-3f >= stuckPeerDeltaC_) ++moved;
  }
  // Strict majority of >=1 comparable peers. Field evidence for majority
  // (slylog 2026-07-09/10): the basement made lone 0.6 C excursions while the
  // dining sensor sat legitimately bit-flat for 16 h — one drifting room must
  // not condemn a flat-but-healthy sensor.
  return comparable > 0 && moved * 2 > comparable;
}

bool SensorFusion::coastPlausible(float v) {
  return std::isfinite(v) && v >= kSensorRangeMinC && v <= kSensorRangeMaxC;
}

SensorStatus SensorFusion::status(uint8_t id, uint32_t nowS) const {
  SensorStatus st;
  const Slot* s = find(id);
  if (!s) return st;
  st.registered = true;
  st.live = s->live;
  st.participating = s->participating;
  st.hasTemp = s->hasUpdate;
  st.tempC = s->correctedC();
  st.faults = s->faults;
  st.offsetC = s->offsetC;
  st.ageS = s->hasUpdate ? ageOf(nowS, s->lastUpdateS) : UINT32_MAX;
  st.everOccupied = s->everOccupied;
  st.occupied = s->everOccupied && ageOf(nowS, s->lastOccS) <= occupancyWindowS_;
  st.lastOccAgeS = s->everOccupied ? ageOf(nowS, s->lastOccS) : UINT32_MAX;
  st.weight = s->weight;
  return st;
}

uint8_t SensorFusion::dominantParticipant() const {
  const Slot* best = nullptr;
  for (const auto& s : slots_) {
    if (!s.used || s.isLocal || !s.live) continue;
    if (!best || s.weight > best->weight) best = &s;
  }
  return best ? best->id : 0xFF;
}

void SensorFusion::setCoastMaxS(uint32_t s) {
  coastMaxS_ = s > 600 ? 600 : s;  // documented range 0-600; 0 disables (#153)
}
void SensorFusion::setWeightRampTauS(uint32_t tauS) {
  weightRampTauS_ = clampU32(tauS, kWeightRampTauMinS, kWeightRampTauMaxS);
}
void SensorFusion::setSmoothingTauS(uint32_t tauS) {
  smoothingTauS_ = clampU32(tauS, kFusionSmoothingTauMinS, kFusionSmoothingTauMaxS);
}
void SensorFusion::setOccupancyWindowS(uint32_t windowS) {
  occupancyWindowS_ = windowS < 60 ? 60 : windowS;
}
void SensorFusion::setStuckWindowS(uint32_t windowS) {
  stuckWindowS_ = windowS < kStuckWindowMinS ? kStuckWindowMinS : windowS;
}
void SensorFusion::setStuckPeerDeltaC(float c) {
  if (!(c >= 0.2f)) c = 0.2f;  // two quantization steps; NaN lands here too
  if (c > 5.0f) c = 5.0f;
  stuckPeerDeltaC_ = c;
}
void SensorFusion::setStuckCeilingS(uint32_t s) {
  stuckCeilingS_ = s < kStuckWindowMinS ? kStuckWindowMinS : s;
}
void SensorFusion::setSlewCPerMin(float cPerMin) {
  slewCPerMin_ = (cPerMin > 0.01f) ? cPerMin : 0.01f;
}
void SensorFusion::setOccupiedWeight(float w) {
  occupiedWeight_ = (w >= 1.0f) ? w : 1.0f;
}
void SensorFusion::setOutlierC(float c) {
  outlierC_ = (c >= 0.5f) ? c : 0.5f;
}
void SensorFusion::setLocalDisagreeAlarmC(float c) {
  localDisagreeC_ = (c >= 0.5f) ? c : 0.5f;
}

}  // namespace dettson
