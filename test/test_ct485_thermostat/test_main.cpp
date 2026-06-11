// Unit tests for Ct485Thermostat — the CT-485 TX stack (issues #14/#15/#16/#21):
// scripted coordinator conversations (AutoNet join happy path, already-addressed
// fast path, coordinator re-enumeration), token grant/queue/no-babble, EXACT
// demand frame bytes under both provisional offset variants, refresh timing +
// starvation alarm, NAK2 pairing stop, response-timeout comms loss, and the
// goSilent flush. This is the most safety-critical module in the project.
#include <unity.h>

#include <cmath>
#include <cstring>

#include "Ct485Core.h"
#include "Ct485Frame.h"
#include "Ct485Thermostat.h"

using namespace ct485;

void setUp() {}
void tearDown() {}

static const uint8_t kMac[8]     = {0x00, 0x12, 0x34, 0xDE, 0xAD, 0xBE, 0xEF, 0x01};
static const uint8_t kSession[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
static const uint8_t kOtherMac[8] = {0x00, 0x12, 0x34, 0x00, 0x00, 0x00, 0x00, 0x99};

static Ct485Thermostat::Config baseCfg(OffsetVariant v = OffsetVariant::kVarA) {
  Ct485Thermostat::Config c;
  c.offsetVariant = v;
  std::memcpy(c.mac, kMac, 8);
  std::memcpy(c.sessionId, kSession, 8);
  return c;
}

// ---------- scripted coordinator frames ----------

static Frame coDiscovery(uint8_t filter = 0x00) {
  Frame f;
  f.dst = kAddrBroadcast;
  f.src = kAddrCoordinator;
  f.srcNodeType = static_cast<uint8_t>(NodeType::kCoordinator);
  f.msgType = static_cast<uint8_t>(MsgType::kNodeDiscovery);
  f.payload[0] = filter;
  f.payloadLen = 1;
  return f;
}

static Frame coSetAddress(const uint8_t* mac, uint8_t addr = kAddrThermostat,
                          uint8_t subnet = kSubnetV2) {
  Frame f;
  f.dst = kAddrBroadcast;
  f.src = kAddrCoordinator;
  f.msgType = static_cast<uint8_t>(MsgType::kSetAddress);
  f.payload[0] = addr;
  f.payload[1] = subnet;
  std::memcpy(f.payload + 2, mac, 8);
  std::memcpy(f.payload + 10, kSession, 8);
  f.payloadLen = 18;
  return f;
}

static Frame coR2R(uint8_t dst = kAddrThermostat, uint8_t pktNum = kPktNumDataflowBit) {
  Frame f;
  f.dst = dst;
  f.src = kAddrCoordinator;
  f.msgType = static_cast<uint8_t>(MsgType::kR2R);
  f.packetNum = pktNum;
  f.payload[0] = 0x00;  // R2R_CODE
  f.payloadLen = 1;
  return f;
}

static Frame coToken(uint8_t dst = kAddrThermostat) {
  Frame f;
  f.dst = dst;
  f.src = kAddrCoordinator;
  f.msgType = static_cast<uint8_t>(MsgType::kTokenOffer);
  return f;
}

static Frame coCtrlResp(uint8_t code, uint8_t cmd, uint8_t dst = kAddrThermostat) {
  Frame f;
  f.dst = dst;
  f.src = 0x02;  // furnace node
  f.srcNodeType = static_cast<uint8_t>(NodeType::kGasFurnace);
  f.sendParamHi = cmd;
  f.msgType = static_cast<uint8_t>(MsgType::kSetControlCmd) | kResponseFlag;
  f.payload[0] = code;
  f.payloadLen = 1;
  return f;
}

static Frame coVersion(uint8_t ver, uint8_t rev) {
  Frame f;
  f.dst = kAddrBroadcast;
  f.src = kAddrCoordinator;
  f.msgType = static_cast<uint8_t>(MsgType::kVersionAnnounce);
  f.payload[0] = ver;
  f.payload[1] = rev;
  f.payload[2] = 0x01;  // FFD
  f.payloadLen = 3;
  return f;
}

// ---------- helpers ----------

static int drain(Ct485Thermostat& t) {
  Frame f;
  int n = 0;
  while (t.popTx(f)) n++;
  return n;
}

// Full happy-path join; leaves the stat addressed at node 1 / subnet V2.
static void join(Ct485Thermostat& t, uint32_t& now) {
  t.resume(now);
  t.onFrame(coDiscovery(), now);
  now += kAnetSlotLoMs;
  t.tick(now);
  Frame f;
  TEST_ASSERT_TRUE(t.popTx(f));  // 0xF9
  t.onFrame(coSetAddress(kMac), now);
  TEST_ASSERT_TRUE(t.popTx(f));  // 0xFA
  TEST_ASSERT_TRUE(t.addressed());
  TEST_ASSERT_EQUAL(0, drain(t));
}

// Grant via R2R and pop exactly one TX frame.
static Frame grant1(Ct485Thermostat& t, uint32_t now) {
  t.onFrame(coR2R(t.nodeAddress()), now);
  Frame f;
  TEST_ASSERT_TRUE_MESSAGE(t.popTx(f), "grant produced no TX");
  TEST_ASSERT_EQUAL_MESSAGE(0, drain(t), "grant produced >1 TX frame");
  return f;
}

static void ackOutstanding(Ct485Thermostat& t, uint8_t cmd, uint32_t now) {
  t.onFrame(coCtrlResp(kAck1, cmd), now);
}

// ---------- boot / silence ----------

static void test_boot_silent_no_demands_no_tx() {
  Ct485Thermostat t(baseCfg());
  TEST_ASSERT_TRUE(t.silent());
  uint32_t now = 1000;
  // Demand API refused while silent.
  TEST_ASSERT_FALSE(t.setDemand(DemandChannel::kHeat, 50.0f, now));
  TEST_ASSERT_FALSE(t.setSystemSwitch(SystemSwitch::kHeat, now));
  // Discovery while silent is ignored entirely — no late 0xF9 after resume.
  t.onFrame(coDiscovery(), now);
  now += kAnetSlotHiMs + 1000;
  t.tick(now);
  TEST_ASSERT_EQUAL(0, drain(t));
  t.resume(now);
  t.tick(now + kAnetSlotHiMs);
  TEST_ASSERT_EQUAL(0, drain(t));
  TEST_ASSERT_EQUAL(static_cast<int>(JoinState::kUnaddressed),
                    static_cast<int>(t.joinState()));
}

static void test_boot_silent_assume_addressed_ignores_grants() {
  auto cfg = baseCfg();
  cfg.assumeAddressed = true;
  Ct485Thermostat t(cfg);
  TEST_ASSERT_TRUE(t.addressed());  // address known, but still silent
  t.onFrame(coR2R(), 1000);
  t.tick(1000);
  TEST_ASSERT_EQUAL(0, drain(t));
}

// ---------- AutoNet join ----------

static void test_join_happy_path_exact_frames() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 5000;
  t.resume(now);
  t.onFrame(coDiscovery(), now);
  TEST_ASSERT_EQUAL(0, drain(t));  // reply waits for the slot
  t.tick(now + kAnetSlotLoMs - 1);
  TEST_ASSERT_EQUAL(0, drain(t));
  t.tick(now + kAnetSlotLoMs);
  Frame f;
  TEST_ASSERT_TRUE(t.popTx(f));
  // 0xF9: nodeType + MAC + sessionId, from the unaddressed node.
  TEST_ASSERT_EQUAL_UINT8(kAddrCoordinator, f.dst);
  TEST_ASSERT_EQUAL_UINT8(0x00, f.src);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kNodeDiscovery) | kResponseFlag,
                          f.msgType);
  TEST_ASSERT_EQUAL_UINT8(17, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(NodeType::kThermostat), f.payload[0]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMac, f.payload + 1, 8);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kSession, f.payload + 9, 8);
  TEST_ASSERT_EQUAL(0, drain(t));

  t.onFrame(coSetAddress(kMac), now + kAnetSlotLoMs + 500);
  TEST_ASSERT_TRUE(t.popTx(f));
  // 0xFA echoes the assignment, already from the new address.
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetAddress) | kResponseFlag,
                          f.msgType);
  TEST_ASSERT_EQUAL_UINT8(kAddrThermostat, f.src);
  TEST_ASSERT_EQUAL_UINT8(kSubnetV2, f.subnet);
  TEST_ASSERT_EQUAL_UINT8(18, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8(kAddrThermostat, f.payload[0]);
  TEST_ASSERT_EQUAL_UINT8(kSubnetV2, f.payload[1]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMac, f.payload + 2, 8);
  TEST_ASSERT_TRUE(t.addressed());
  TEST_ASSERT_EQUAL_UINT8(kAddrThermostat, t.nodeAddress());
  TEST_ASSERT_EQUAL_UINT8(kSubnetV2, t.nodeSubnet());
}

static uint32_t gRandLo, gRandHi, gRandReturn;
static uint32_t scriptedRandom(uint32_t lo, uint32_t hi) {
  gRandLo = lo;
  gRandHi = hi;
  return gRandReturn;
}

static void test_join_slot_uses_injected_random() {
  auto cfg = baseCfg();
  cfg.randomMs = scriptedRandom;
  gRandReturn = 12345;
  Ct485Thermostat t(cfg);
  uint32_t now = 1000;
  t.resume(now);
  t.onFrame(coDiscovery(), now);
  TEST_ASSERT_EQUAL_UINT32(kAnetSlotLoMs, gRandLo);  // documented 6-30 s window
  TEST_ASSERT_EQUAL_UINT32(kAnetSlotHiMs, gRandHi);
  t.tick(now + 12344);
  TEST_ASSERT_EQUAL(0, drain(t));
  t.tick(now + 12345);
  TEST_ASSERT_EQUAL(1, drain(t));
}

static void test_join_slot_clamps_injected_random() {
  auto cfg = baseCfg();
  cfg.randomMs = scriptedRandom;
  gRandReturn = 100;  // out of window -> clamped to the 6 s floor
  Ct485Thermostat t(cfg);
  uint32_t now = 1000;
  t.resume(now);
  t.onFrame(coDiscovery(), now);
  t.tick(now + kAnetSlotLoMs - 1);
  TEST_ASSERT_EQUAL(0, drain(t));
  t.tick(now + kAnetSlotLoMs);
  TEST_ASSERT_EQUAL(1, drain(t));
}

static void test_join_ignores_setaddress_for_other_mac() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  t.resume(now);
  t.onFrame(coDiscovery(), now);
  t.tick(now + kAnetSlotLoMs);
  TEST_ASSERT_EQUAL(1, drain(t));  // our 0xF9
  t.onFrame(coSetAddress(kOtherMac, 0x02), now + kAnetSlotLoMs + 100);
  TEST_ASSERT_EQUAL(0, drain(t));  // someone else's assignment: silence
  TEST_ASSERT_FALSE(t.addressed());
}

static void test_join_discovery_filter_other_node_type() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  t.resume(now);
  t.onFrame(coDiscovery(static_cast<uint8_t>(NodeType::kGasFurnace)), now);
  t.tick(now + kAnetSlotHiMs);
  TEST_ASSERT_EQUAL(0, drain(t));
  TEST_ASSERT_EQUAL(static_cast<int>(JoinState::kUnaddressed),
                    static_cast<int>(t.joinState()));
}

static void test_already_addressed_fast_path() {
  auto cfg = baseCfg();
  cfg.assumeAddressed = true;
  Ct485Thermostat t(cfg);
  uint32_t now = 1000;
  t.resume(now);
  Frame f = grant1(t, now);  // coordinator polls node 1 directly, no AutoNet
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
  TEST_ASSERT_EQUAL_UINT8(kAddrCoordinator, f.dst);
  TEST_ASSERT_TRUE((f.packetNum & kPktNumDataflowBit) != 0);
  TEST_ASSERT_EQUAL_UINT8(1, f.payloadLen);
  TEST_ASSERT_EQUAL_UINT8(kAck1, f.payload[0]);  // R2R_ACK 0x06
}

static void test_coordinator_reenumeration_drops_demands_and_rejoins() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  Frame f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  ackOutstanding(t, f.sendParamHi, now);

  // Coordinator restarts and re-enumerates: drop everything, rejoin.
  now += 5000;
  t.onFrame(coDiscovery(), now);
  TEST_ASSERT_FALSE(t.addressed());
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kHeat));
  TEST_ASSERT_EQUAL_UINT32(1, t.reenumerations());
  t.onFrame(coR2R(), now);  // poll to old address while unaddressed: silence
  TEST_ASSERT_EQUAL(0, drain(t));

  now += kAnetSlotLoMs;
  t.tick(now);
  TEST_ASSERT_EQUAL(1, drain(t));  // fresh 0xF9
  t.onFrame(coSetAddress(kMac), now);
  TEST_ASSERT_EQUAL(1, drain(t));  // 0xFA
  TEST_ASSERT_TRUE(t.addressed());
  // Demand was NOT silently resurrected across the rejoin.
  f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
}

// ---------- token discipline / no-babble ----------

static void test_no_babble_on_foreign_or_unexpected_frames() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  t.onFrame(coR2R(0x02), now);                       // someone else's grant
  t.onFrame(coToken(0x05), now);                     // someone else's token
  t.onFrame(coR2R(kAddrThermostat, 0x00), now);      // R2R without dataflow bit
  Frame status;                                      // unsolicited app message
  status.dst = kAddrThermostat;
  status.src = 0x02;
  status.msgType = static_cast<uint8_t>(MsgType::kGetStatus);
  t.onFrame(status, now);
  Frame resp = coCtrlResp(kAck1, 0x64);              // 0x83 with nothing outstanding
  t.onFrame(resp, now);
  t.tick(now + 100);
  TEST_ASSERT_EQUAL(0, drain(t));  // default to silence, always
  TEST_ASSERT_TRUE(t.unexpectedFrames() >= 3);
}

static void test_token_offer_declined_when_idle() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  t.onFrame(coToken(), now);
  Frame f;
  TEST_ASSERT_TRUE(t.popTx(f));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kTokenOffer) | kResponseFlag,
                          f.msgType);
  TEST_ASSERT_EQUAL_UINT8(kAddrThermostat, f.payload[0]);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(kMac, f.payload + 2, 8);
  TEST_ASSERT_EQUAL(0, drain(t));
}

static void test_demand_queued_until_grant() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 80.0f, now));
  t.tick(now + 10);
  TEST_ASSERT_EQUAL(0, drain(t));  // TX only when granted
  Frame f = grant1(t, now + 20);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  // Token Offer grants work the same way.
  ackOutstanding(t, f.sendParamHi, now + 30);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 90.0f, now + 40));
  t.onFrame(coToken(), now + 50);
  TEST_ASSERT_TRUE(t.popTx(f));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  TEST_ASSERT_EQUAL(0, drain(t));
}

// ---------- demand frame bytes, EXACT, both variants ----------

static void test_demand_frame_bytes_exact_variant_a() {
  Ct485Thermostat t(baseCfg(OffsetVariant::kVarA));
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 80.0f, now));
  Frame f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(0x00, f.dst);  // routed
  TEST_ASSERT_EQUAL_UINT8(0x01, f.src);
  TEST_ASSERT_EQUAL_UINT8(kSubnetV2, f.subnet);
  TEST_ASSERT_EQUAL_UINT8(0x01, f.sendMethod);  // routed-by-priority
  TEST_ASSERT_EQUAL_UINT8(0x64, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(0x00, f.sendParamLo);
  TEST_ASSERT_EQUAL_UINT8(0x01, f.srcNodeType);
  TEST_ASSERT_EQUAL_UINT8(0x03, f.msgType);
  TEST_ASSERT_EQUAL_UINT8(4, f.payloadLen);
  const uint8_t wantA[4] = {0x64, 0x00, 0x10, 0xA0};  // echo LE, timer 60 s, 80%*2
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantA, f.payload, 4);
  // Frame-offset semantics on the wire: [12]=timer, [13]=demand.
  uint8_t raw[kMaxFrame];
  size_t len = encode(f, raw);
  TEST_ASSERT_EQUAL_UINT32(16, len);
  TEST_ASSERT_EQUAL_UINT8(0x10, raw[12]);
  TEST_ASSERT_EQUAL_UINT8(0xA0, raw[13]);
  TEST_ASSERT_TRUE(fletcherOk(raw, f.payloadLen));
}

static void test_demand_frame_bytes_exact_variant_b() {
  Ct485Thermostat t(baseCfg(OffsetVariant::kVarB));
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 80.0f, now));
  Frame f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(5, f.payloadLen);
  const uint8_t wantB[5] = {0x64, 0x00, 0x00, 0x10, 0xA0};  // [12] zero-filled
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantB, f.payload, 5);
  uint8_t raw[kMaxFrame];
  size_t len = encode(f, raw);
  TEST_ASSERT_EQUAL_UINT32(17, len);
  TEST_ASSERT_EQUAL_UINT8(0x10, raw[13]);  // [13]=timer, [14]=demand
  TEST_ASSERT_EQUAL_UINT8(0xA0, raw[14]);
}

static void test_fan_demand_bytes_both_variants() {
  {
    Ct485Thermostat t(baseCfg(OffsetVariant::kVarA));
    uint32_t now = 1000;
    join(t, now);
    TEST_ASSERT_TRUE(t.setFanDemand(50.0f, 0x01, now));
    Frame f = grant1(t, now);
    TEST_ASSERT_EQUAL_UINT8(0x66, f.sendParamHi);
    TEST_ASSERT_EQUAL_UINT8(5, f.payloadLen);
    const uint8_t want[5] = {0x66, 0x00, 0x10, 0x01, 0x64};  // timer, mode, pct*2
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f.payload, 5);
  }
  {
    Ct485Thermostat t(baseCfg(OffsetVariant::kVarB));
    uint32_t now = 1000;
    join(t, now);
    TEST_ASSERT_TRUE(t.setFanDemand(50.0f, 0x01, now));
    Frame f = grant1(t, now);
    TEST_ASSERT_EQUAL_UINT8(6, f.payloadLen);
    const uint8_t want[6] = {0x66, 0x00, 0x00, 0x10, 0x01, 0x64};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, f.payload, 6);
  }
}

static void test_demand_refused_until_offset_variant_set() {
  Ct485Thermostat t(baseCfg(OffsetVariant::kUnset));  // Phase 2 gate closed
  uint32_t now = 1000;
  join(t, now);  // joining is fine — only demand TX is gated
  TEST_ASSERT_FALSE(t.setDemand(DemandChannel::kHeat, 50.0f, now));
  TEST_ASSERT_FALSE(t.setFanDemand(50.0f, 0, now));
  Frame f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);  // poll-ack only
}

static void test_percent_times_two_encoding_and_clamp() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kCool, 40.0f, now));
  Frame f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(0x65, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(80, f.payload[3]);  // 40% -> 0x50
  ackOutstanding(t, 0x65, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kCool, 150.0f, now));  // clamps to 100
  f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(200, f.payload[3]);
  ackOutstanding(t, 0x65, now);
  // Non-finite input coerces to 0, the safe direction.
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kBackupHeat, std::nanf(""), now));
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kBackupHeat));
}

static void test_system_switch_and_setpoint_frames() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setSystemSwitch(SystemSwitch::kHeat, now));
  TEST_ASSERT_TRUE(t.setHeatSetpoint(72, now));
  TEST_ASSERT_TRUE(t.setCoolSetpoint(78, now));
  TEST_ASSERT_EQUAL(0, drain(t));  // FIFO waits for grants

  Frame f = grant1(t, now);  // FIFO order preserved
  TEST_ASSERT_EQUAL_UINT8(0x05, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(3, f.payloadLen);
  const uint8_t wantSwitch[3] = {0x05, 0x00, 0x03};  // value at frame [12]
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantSwitch, f.payload, 3);
  ackOutstanding(t, 0x05, now);

  f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(0x01, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(4, f.payloadLen);
  const uint8_t wantHeatSp[4] = {0x01, 0x00, 0x00, 72};  // temp at frame [13]
  TEST_ASSERT_EQUAL_UINT8_ARRAY(wantHeatSp, f.payload, 4);
  ackOutstanding(t, 0x01, now);

  f = grant1(t, now);
  TEST_ASSERT_EQUAL_UINT8(0x02, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(78, f.payload[3]);
}

// ---------- refresh watchdog ----------

static void test_refresh_timer_byte_decoding() {
  TEST_ASSERT_EQUAL_UINT32(60000, refreshTimerMs(0x10));   // 1 min
  TEST_ASSERT_EQUAL_UINT32(135000, refreshTimerMs(0x24));  // 2 min + 4 * 3.75 s
  TEST_ASSERT_EQUAL_UINT32(0, refreshTimerMs(0x00));
  TEST_ASSERT_EQUAL_UINT32(956250, refreshTimerMs(0xFF));  // 15 min 56.25 s
}

static void test_refresh_reemitted_at_fraction_of_window() {
  Ct485Thermostat t(baseCfg());  // 60 s window x 0.5 -> refresh due at 30 s
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  Frame f = grant1(t, now);
  ackOutstanding(t, 0x64, now + 100);
  TEST_ASSERT_EQUAL_UINT32(now + 30000, t.nextRefreshDueMs(DemandChannel::kHeat));

  t.tick(now + 20000);
  f = grant1(t, now + 20000);  // before due: poll-ack, no demand resend
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);

  t.tick(now + 30000);
  f = grant1(t, now + 30000);  // due: the demand goes out again
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  TEST_ASSERT_EQUAL_UINT8(0x64, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(120, f.payload[3]);  // still 60%
  TEST_ASSERT_EQUAL_UINT32(now + 60000, t.nextRefreshDueMs(DemandChannel::kHeat));
}

static void test_starvation_alarm_goes_silent() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kCool, 50.0f, now));
  grant1(t, now);
  ackOutstanding(t, 0x65, now + 100);
  // No grants from here on: the refresh can never be sent.
  t.tick(now + 59999);
  TEST_ASSERT_FALSE(t.starvationAlarm());
  t.tick(now + 60000);  // full window missed
  TEST_ASSERT_TRUE(t.starvationAlarm());
  TEST_ASSERT_EQUAL(static_cast<int>(DemandChannel::kCool),
                    static_cast<int>(t.starvedChannel()));
  TEST_ASSERT_TRUE(t.silent());  // go-silent trigger, not a retry storm
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kCool));
  t.onFrame(coR2R(), now + 61000);
  TEST_ASSERT_EQUAL(0, drain(t));
}

static void test_starvation_when_demand_never_sent() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  // No grant ever arrives — the system would believe it is heating.
  t.tick(now + 59999);
  TEST_ASSERT_FALSE(t.starvationAlarm());
  t.tick(now + 60000);
  TEST_ASSERT_TRUE(t.starvationAlarm());
  TEST_ASSERT_TRUE(t.silent());
}

static void test_zero_demand_sends_one_cancel_frame() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  grant1(t, now);
  ackOutstanding(t, 0x64, now + 100);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 0.0f, now + 200));
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kHeat));
  Frame f = grant1(t, now + 300);  // one explicit zero frame
  TEST_ASSERT_EQUAL_UINT8(0x64, f.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(0x00, f.payload[3]);
  ackOutstanding(t, 0x64, now + 400);
  f = grant1(t, now + 500);  // then nothing more for this channel
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
  // Inactive channel never starves.
  t.tick(now + 600000);
  TEST_ASSERT_FALSE(t.starvationAlarm());
  TEST_ASSERT_FALSE(t.silent());
}

static void test_zero_demand_never_sent_sends_nothing() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 0.0f, now + 10));  // never hit the wire
  Frame f = grant1(t, now + 20);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);  // nothing to cancel
}

// ---------- ACK/NAK / response timeout ----------

static void test_ack_clears_outstanding() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  grant1(t, now);
  ackOutstanding(t, 0x64, now + 50);
  TEST_ASSERT_EQUAL_UINT8(kAck1, t.lastResponseCode());
  Frame f = grant1(t, now + 100);  // no resend until refresh is due
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
}

static void test_nak2_pairing_alarm_stops_demands() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  Frame f = grant1(t, now);
  t.onFrame(coCtrlResp(kNak2, f.sendParamHi), now + 50);
  TEST_ASSERT_TRUE(t.pairingAlarm());  // not our equipment: never retry (docs/02 §9)
  TEST_ASSERT_EQUAL_UINT8(kNak2, t.lastResponseCode());
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kHeat));
  f = grant1(t, now + 100);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
  t.tick(now + 600000);  // demands stopped -> no starvation either
  TEST_ASSERT_FALSE(t.starvationAlarm());
}

static void test_response_timeout_retries_then_comms_loss_silence() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  Frame tx1 = grant1(t, now);  // attempt 1
  uint8_t raw1[kMaxFrame];
  size_t len1 = encode(tx1, raw1);

  t.tick(now + kResponseTimeoutMs);  // timeout 1: retry owed, but only on a grant
  TEST_ASSERT_EQUAL(0, drain(t));
  uint32_t g2 = now + kResponseTimeoutMs + 100;
  Frame tx2 = grant1(t, g2);  // attempt 2: verbatim retransmit
  uint8_t raw2[kMaxFrame];
  size_t len2 = encode(tx2, raw2);
  TEST_ASSERT_EQUAL_UINT32(len1, len2);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(raw1, raw2, len1);

  t.tick(g2 + kResponseTimeoutMs);  // timeout 2
  uint32_t g3 = g2 + kResponseTimeoutMs + 100;
  grant1(t, g3);  // attempt 3 (kMsgResendAttempts total)

  t.tick(g3 + kResponseTimeoutMs);  // timeout 3: budget exhausted
  TEST_ASSERT_TRUE(t.commsLossAlarm());
  TEST_ASSERT_TRUE(t.silent());  // comms-loss state = all channels silent
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kHeat));
  t.onFrame(coR2R(), g3 + kResponseTimeoutMs + 100);
  TEST_ASSERT_EQUAL(0, drain(t));
}

static void test_nak1_counts_against_attempt_budget() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  Frame f = grant1(t, now);
  t.onFrame(coCtrlResp(kNak1, f.sendParamHi), now + 50);  // bad CRC: retransmit
  f = grant1(t, now + 100);
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  t.onFrame(coCtrlResp(kNak1, f.sendParamHi), now + 150);
  grant1(t, now + 200);  // attempt 3
  t.onFrame(coCtrlResp(kNak1, f.sendParamHi), now + 250);  // budget exhausted
  TEST_ASSERT_TRUE(t.commsLossAlarm());
  TEST_ASSERT_TRUE(t.silent());
}

// ---------- goSilent / alarm latching ----------

static void test_go_silent_flushes_everything() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  TEST_ASSERT_TRUE(t.setSystemSwitch(SystemSwitch::kHeat, now));
  t.onFrame(coR2R(), now);  // demand frame now sitting in the TX queue
  TEST_ASSERT_EQUAL_UINT32(1, t.txPending());

  t.goSilent();
  Frame f;
  TEST_ASSERT_FALSE(t.popTx(f));  // queued TX flushed
  TEST_ASSERT_TRUE(t.silent());
  TEST_ASSERT_FALSE(t.channelActive(DemandChannel::kHeat));
  TEST_ASSERT_FALSE(t.setDemand(DemandChannel::kHeat, 60.0f, now));  // refused
  t.onFrame(coR2R(), now + 100);  // grants ignored while silent
  t.tick(now + 200);
  TEST_ASSERT_EQUAL(0, drain(t));

  t.resume(now + 300);
  f = grant1(t, now + 400);  // demand + FIFO are gone: poll-ack only
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kR2R), f.msgType);
}

static void test_alarms_latched_across_resume_until_cleared() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 60.0f, now));
  grant1(t, now);
  ackOutstanding(t, 0x64, now + 50);
  t.tick(now + 60000);  // starve
  TEST_ASSERT_TRUE(t.starvationAlarm());
  t.resume(now + 61000);
  TEST_ASSERT_FALSE(t.silent());
  TEST_ASSERT_TRUE(t.starvationAlarm());  // resume() never clears alarms
  t.clearAlarms();
  TEST_ASSERT_FALSE(t.starvationAlarm());
}

// ---------- version announcement / coordinator identity ----------

static void test_version_announce_and_version_bit_mirroring() {
  Ct485Thermostat t(baseCfg());
  uint32_t now = 1000;
  join(t, now);
  t.onFrame(coVersion(2, 7), now);
  TEST_ASSERT_EQUAL_UINT8(2, t.busVersion());
  TEST_ASSERT_EQUAL_UINT8(7, t.busRevision());
  TEST_ASSERT_EQUAL(0, drain(t));  // broadcast: no reply

  // CT v1.0 coordinator (version bit set): our TX mirrors the bit.
  t.onFrame(coR2R(kAddrThermostat,
                  static_cast<uint8_t>(kPktNumDataflowBit | kPktNumVersionBit)),
            now + 100);
  Frame f;
  TEST_ASSERT_TRUE(t.popTx(f));
  TEST_ASSERT_EQUAL_UINT8(kPktNumDataflowBit | kPktNumVersionBit, f.packetNum);
  TEST_ASSERT_TRUE(t.setDemand(DemandChannel::kHeat, 50.0f, now + 200));
  t.onFrame(coR2R(kAddrThermostat,
                  static_cast<uint8_t>(kPktNumDataflowBit | kPktNumVersionBit)),
            now + 300);
  TEST_ASSERT_TRUE(t.popTx(f));
  TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(MsgType::kSetControlCmd), f.msgType);
  TEST_ASSERT_EQUAL_UINT8(kPktNumVersionBit, f.packetNum);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_boot_silent_no_demands_no_tx);
  RUN_TEST(test_boot_silent_assume_addressed_ignores_grants);
  RUN_TEST(test_join_happy_path_exact_frames);
  RUN_TEST(test_join_slot_uses_injected_random);
  RUN_TEST(test_join_slot_clamps_injected_random);
  RUN_TEST(test_join_ignores_setaddress_for_other_mac);
  RUN_TEST(test_join_discovery_filter_other_node_type);
  RUN_TEST(test_already_addressed_fast_path);
  RUN_TEST(test_coordinator_reenumeration_drops_demands_and_rejoins);
  RUN_TEST(test_no_babble_on_foreign_or_unexpected_frames);
  RUN_TEST(test_token_offer_declined_when_idle);
  RUN_TEST(test_demand_queued_until_grant);
  RUN_TEST(test_demand_frame_bytes_exact_variant_a);
  RUN_TEST(test_demand_frame_bytes_exact_variant_b);
  RUN_TEST(test_fan_demand_bytes_both_variants);
  RUN_TEST(test_demand_refused_until_offset_variant_set);
  RUN_TEST(test_percent_times_two_encoding_and_clamp);
  RUN_TEST(test_system_switch_and_setpoint_frames);
  RUN_TEST(test_refresh_timer_byte_decoding);
  RUN_TEST(test_refresh_reemitted_at_fraction_of_window);
  RUN_TEST(test_starvation_alarm_goes_silent);
  RUN_TEST(test_starvation_when_demand_never_sent);
  RUN_TEST(test_zero_demand_sends_one_cancel_frame);
  RUN_TEST(test_zero_demand_never_sent_sends_nothing);
  RUN_TEST(test_ack_clears_outstanding);
  RUN_TEST(test_nak2_pairing_alarm_stops_demands);
  RUN_TEST(test_response_timeout_retries_then_comms_loss_silence);
  RUN_TEST(test_nak1_counts_against_attempt_budget);
  RUN_TEST(test_go_silent_flushes_everything);
  RUN_TEST(test_alarms_latched_across_resume_until_cleared);
  RUN_TEST(test_version_announce_and_version_bit_mirroring);
  return UNITY_END();
}
