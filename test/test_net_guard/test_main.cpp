// NetGuard tests (#109): blips under the sustain threshold never block,
// sustained outages raise the right panel, the attempt counter counts from
// outage start and resets on recovery, a changed outage character restarts
// the sustain clock, and recovery is instant.
#include <unity.h>

#include "NetGuard.h"

using namespace net_guard;

void setUp() {}
void tearDown() {}

constexpr uint32_t kSustain = 300000;  // 5 min, matching production

static void test_healthy_stays_healthy() {
  Guard g(kSustain);
  TEST_ASSERT_EQUAL(static_cast<int>(State::kHealthy),
                    static_cast<int>(g.update(true, true, 0, 1000)));
  TEST_ASSERT_EQUAL_UINT32(0, g.outageAttempts());
}

static void test_blip_under_threshold_never_blocks() {
  Guard g(kSustain);
  g.update(true, true, 0, 0);
  // broker drops for 4m59s — still a blip
  for (uint32_t t = 1000; t < kSustain; t += 30000) {
    TEST_ASSERT_EQUAL(static_cast<int>(State::kBlip),
                      static_cast<int>(g.update(false, false, t / 10000, t)));
  }
  // recovers just before the threshold: instantly healthy
  TEST_ASSERT_EQUAL(static_cast<int>(State::kHealthy),
                    static_cast<int>(g.update(true, true, 30, kSustain - 1)));
  TEST_ASSERT_EQUAL_UINT32(0, g.outageAttempts());
}

static void test_sustained_network_outage_blocks() {
  Guard g(kSustain);
  g.update(true, true, 5, 0);
  g.update(false, false, 5, 1000);           // outage starts, 5 attempts so far
  g.update(false, false, 9, kSustain / 2);   // mid-outage
  const State s = g.update(false, false, 12, kSustain + 1001);
  TEST_ASSERT_EQUAL(static_cast<int>(State::kNetworkUnavailable), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT32(7, g.outageAttempts());  // 12 - 5 at outage start
}

static void test_sustained_controller_offline_blocks() {
  Guard g(kSustain);
  g.update(true, true, 0, 0);
  g.update(true, false, 0, 1000);  // broker fine, Controller LWT offline
  const State s = g.update(true, false, 3, kSustain + 1001);
  TEST_ASSERT_EQUAL(static_cast<int>(State::kControllerOffline), static_cast<int>(s));
  TEST_ASSERT_EQUAL_UINT32(3, g.outageAttempts());
}

static void test_outage_character_change_restarts_clock() {
  Guard g(kSustain);
  g.update(true, true, 0, 0);
  // network outage for 4 min...
  g.update(false, false, 2, 1000);
  g.update(false, false, 4, 240000);
  // ...then WiFi recovers but the Controller stays dark: NEW sustain window
  const State s = g.update(true, false, 6, 250000);
  TEST_ASSERT_EQUAL(static_cast<int>(State::kBlip), static_cast<int>(s));
  // controller-offline must persist its own full 5 min from the change
  TEST_ASSERT_EQUAL(static_cast<int>(State::kBlip),
                    static_cast<int>(g.update(true, false, 8, 250000 + kSustain - 1)));
  TEST_ASSERT_EQUAL(static_cast<int>(State::kControllerOffline),
                    static_cast<int>(g.update(true, false, 8, 250000 + kSustain)));
}

static void test_recovery_is_instant_and_resets_attempts() {
  Guard g(kSustain);
  g.update(false, false, 100, 0);
  g.update(false, false, 140, kSustain + 1);  // blocked, 40 attempts shown
  TEST_ASSERT_EQUAL_UINT32(40, g.outageAttempts());
  TEST_ASSERT_EQUAL(static_cast<int>(State::kHealthy),
                    static_cast<int>(g.update(true, true, 141, kSustain + 2)));
  TEST_ASSERT_EQUAL_UINT32(0, g.outageAttempts());
  // next outage counts from its own start
  g.update(false, true, 141, kSustain + 3);
  TEST_ASSERT_EQUAL_UINT32(0, g.outageAttempts());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_healthy_stays_healthy);
  RUN_TEST(test_blip_under_threshold_never_blocks);
  RUN_TEST(test_sustained_network_outage_blocks);
  RUN_TEST(test_sustained_controller_offline_blocks);
  RUN_TEST(test_outage_character_change_restarts_clock);
  RUN_TEST(test_recovery_is_instant_and_resets_attempts);
  return UNITY_END();
}
