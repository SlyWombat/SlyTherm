// NetGuard.h — the Remote's degraded-UX state machine (issue #109; docs/11
// "Degraded-UX"). Pure C++17, no Arduino deps, host-tested (test/test_net_guard).
//
// Keyed on two signals: broker reachable? (wifi + MQTT session) and
// Controller live? (the slytherm/availability LWT the Remote tracks). A
// blip (<5 min sustained) keeps the last-known UI and retries quietly; only
// a SUSTAINED outage raises a blocking panel:
//   kNetworkUnavailable — WiFi/broker unreachable >=5 min ("Retrying
//     (attempt N)" + [Reboot]; no control surface)
//   kControllerOffline  — broker OK but the Controller's LWT is offline
//     >=5 min (same panel + "Check Controller Power")
// Retries have NO max — the attempt counter is unbounded and resets on
// recovery; cadence/backoff live with the owners of the retries
// (remote_wifi/remote_mqtt), not here. Recovery = the retained echo
// restores state -> back to kHealthy instantly (no re-splash).
#pragma once

#include <cstdint>

namespace net_guard {

enum class State : uint8_t {
  kHealthy,             // all links up
  kBlip,                // something down, but < the sustain threshold
  kNetworkUnavailable,  // wifi/broker down, sustained
  kControllerOffline,   // broker up, Controller LWT offline, sustained
};

class Guard {
 public:
  // sustainMs: how long an outage must persist before blocking UI (5 min
  // per docs/11; injectable for tests/bench builds).
  explicit Guard(uint32_t sustainMs = 5u * 60u * 1000u) : sustainMs_(sustainMs) {}

  // Feed the current link signals + monotonic clock; returns the state.
  // `attempts` is the caller's cumulative reconnect-attempt counter (wifi +
  // broker retries); the guard snapshots it at outage start so the panel
  // shows attempts SINCE the outage began, and it naturally resets on
  // recovery.
  State update(bool brokerUp, bool controllerUp, uint32_t attempts, uint32_t nowMs);

  State state() const { return state_; }
  // Attempts since the current outage began (0 while healthy).
  uint32_t outageAttempts() const { return outageAttempts_; }

 private:
  uint32_t sustainMs_;
  State state_ = State::kHealthy;
  bool outage_ = false;
  bool outageIsNetwork_ = false;  // which panel a sustained outage raises
  uint32_t outageStartMs_ = 0;
  uint32_t attemptsAtStart_ = 0;
  uint32_t outageAttempts_ = 0;
};

}  // namespace net_guard
