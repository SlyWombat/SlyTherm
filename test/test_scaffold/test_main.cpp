// Scaffold smoke test: the shared headers compile and basic invariants hold.
#include <unity.h>
#include "Ct485Core.h"
#include "DettsonConfig.h"

void setUp() {}
void tearDown() {}

static void test_frame_geometry() {
  TEST_ASSERT_EQUAL_UINT32(252, ct485::kMaxFrame);
  ct485::Frame f;
  f.payloadLen = 4;
  TEST_ASSERT_EQUAL_UINT32(16, f.totalLen());
  f.msgType = 0x82;
  TEST_ASSERT_TRUE(f.isResponse());
  TEST_ASSERT_EQUAL_UINT8(0x02, f.baseMsgType());
}

static void test_defaults_sane() {
  TEST_ASSERT_TRUE(dettson::kGasFloorPct == 40.0f);
  // Floor = the ODU's own ~3-min (180 s) internal restart delay: SlyTherm's
  // auto min-off must never sit BELOW the hardware's own protection (docs/04).
  TEST_ASSERT_TRUE(dettson::kCompressorMinOffS >= 180);
  TEST_ASSERT_TRUE(dettson::kMinSetpointDeltaC > dettson::kMinSetpointDeltaFloorC);
  TEST_ASSERT_TRUE(dettson::kFallbackCoolSetpointC > dettson::kFallbackHeatSetpointC);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_frame_geometry);
  RUN_TEST(test_defaults_sane);
  return UNITY_END();
}
