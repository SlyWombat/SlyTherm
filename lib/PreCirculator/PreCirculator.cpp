// PreCirculator.cpp — see header for the docs/13 §3+§8 rationale.

#include "PreCirculator.h"

namespace dettson {

float PreCirculator::update(const Inputs& in, uint32_t nowS) {
  const bool predicted = (cfg_.heatEnabled && in.heatPredicted) ||
                         (cfg_.coolEnabled && in.coolPredicted);

  // Roll the per-hour duty bucket (wall-clock hour of the injected time —
  // the same nowS % 3600 frame the circulate window uses).
  const uint32_t hour = nowS / 3600u;
  if (hour != bucketHour_) {
    bucketHour_ = hour;
    bucketCreditS_ = 0;
    // A pre-run slice spanning the roll credits only its in-bucket
    // remainder — the pre-roll part belonged to the closed hour, and a
    // bucket can never hold more credit than the hour has elapsed.
    if (state_ == State::kPreRun && lastUpdateS_ < hour * 3600u) {
      lastUpdateS_ = hour * 3600u;
    }
  }

  switch (state_) {
    case State::kIdle:
      if (predicted && !in.callActive) {
        state_ = State::kPreRun;
        runStartS_ = nowS;
        lastUpdateS_ = nowS;
      }
      break;

    case State::kPreRun:
      // Credit the elapsed slice before deciding transitions, so a pre-run
      // that hands off to a call this tick still counts its runtime. A
      // slice spanning the hour boundary lands whole in the new bucket —
      // at control-tick granularity the error is seconds.
      bucketCreditS_ += nowS - lastUpdateS_;
      lastUpdateS_ = nowS;
      if (in.callActive) {
        state_ = State::kIdle;   // handoff: the engaged stage owns airflow
      } else if (!predicted) {
        state_ = State::kIdle;   // prediction evaporated: cancel
      } else if (nowS - runStartS_ >= cfg_.maxRunS) {
        state_ = State::kSpent;  // hovering prediction: cap, don't run forever
      }
      break;

    case State::kSpent:
      if (in.callActive || !predicted) state_ = State::kIdle;  // re-arm
      break;
  }

  return state_ == State::kPreRun ? cfg_.fanPct : 0.0f;
}

}  // namespace dettson
