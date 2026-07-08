// Unit tests for lib/Ct485Frame (docs/02-protocol-climatetalk.md §2-§3, §8).
#include <unity.h>

#include <cstring>

#include "Ct485Core.h"
#include "Ct485Frame.h"

void setUp() {}
void tearDown() {}

// ---------- helpers ----------

// A plausible Set Control Command (heat demand) frame for round-trip tests.
static ct485::Frame makeDemandFrame() {
  ct485::Frame f;
  f.dst         = 0x02;
  f.src         = ct485::kAddrThermostat;
  f.subnet      = ct485::kSubnetV2;
  f.sendMethod  = 0x01;
  f.sendParamHi = static_cast<uint8_t>(ct485::Command::kHeatDemand);
  f.sendParamLo = 0x00;
  f.srcNodeType = static_cast<uint8_t>(ct485::NodeType::kThermostat);
  f.msgType     = static_cast<uint8_t>(ct485::MsgType::kSetControlCmd);
  f.packetNum   = 0x00;
  f.payloadLen  = 5;
  const uint8_t payload[5] = {0x64, 0x00, 0x10, 0x50, 0x00};
  std::memcpy(f.payload, payload, sizeof(payload));
  return f;
}

// Feed a raw buffer into an accumulator byte-by-byte; gapBefore on first byte.
static bool feedAll(ct485::FrameAccumulator& acc, const uint8_t* buf, size_t len) {
  bool completed = false;
  for (size_t i = 0; i < len; i++) {
    if (acc.feed(buf[i], i == 0)) completed = true;
  }
  return completed;
}

// ---------- Fletcher-16 ----------

// Hand-computed known vector: 10 zero bytes, payloadLen 0.
// s1 stays 0xAA; s2 cycles 170,85,0,... ending at 0xAA -> both checksum
// bytes are 0xAA, and validation lands on s1==0 && s2==0.
static void test_fletcher_known_vector() {
  uint8_t buf[12] = {};
  ct485::fletcher16(buf, 0);
  TEST_ASSERT_EQUAL_HEX8(0xAA, buf[10]);
  TEST_ASSERT_EQUAL_HEX8(0xAA, buf[11]);
  TEST_ASSERT_TRUE(ct485::fletcherOk(buf, 0));
}

static void test_fletcher_detects_single_bit_flip() {
  uint8_t buf[ct485::kMaxFrame] = {};
  ct485::Frame f = makeDemandFrame();
  size_t total = ct485::encode(f, buf);
  TEST_ASSERT_TRUE(ct485::fletcherOk(buf, f.payloadLen));
  for (size_t i = 0; i < total; i++) {
    buf[i] ^= 0x01;
    TEST_ASSERT_FALSE_MESSAGE(ct485::fletcherOk(buf, f.payloadLen),
                              "bit flip not detected");
    buf[i] ^= 0x01;
  }
}

// ---------- encode / decode ----------

static void test_roundtrip_encode_decode() {
  ct485::Frame f = makeDemandFrame();
  uint8_t buf[ct485::kMaxFrame] = {};
  const size_t total = ct485::encode(f, buf);
  TEST_ASSERT_EQUAL_UINT32(f.payloadLen + 12u, total);
  TEST_ASSERT_EQUAL_UINT8(f.payloadLen, buf[ct485::kOffPayloadLen]);

  ct485::Frame out;
  TEST_ASSERT_TRUE(ct485::decode(buf, total, out));
  TEST_ASSERT_EQUAL_UINT8(f.dst, out.dst);
  TEST_ASSERT_EQUAL_UINT8(f.src, out.src);
  TEST_ASSERT_EQUAL_UINT8(f.subnet, out.subnet);
  TEST_ASSERT_EQUAL_UINT8(f.sendMethod, out.sendMethod);
  TEST_ASSERT_EQUAL_UINT8(f.sendParamHi, out.sendParamHi);
  TEST_ASSERT_EQUAL_UINT8(f.sendParamLo, out.sendParamLo);
  TEST_ASSERT_EQUAL_UINT8(f.srcNodeType, out.srcNodeType);
  TEST_ASSERT_EQUAL_UINT8(f.msgType, out.msgType);
  TEST_ASSERT_EQUAL_UINT8(f.packetNum, out.packetNum);
  TEST_ASSERT_EQUAL_UINT8(f.payloadLen, out.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(f.payload, out.payload, f.payloadLen);
}

static void test_roundtrip_zero_and_max_payload() {
  uint8_t buf[ct485::kMaxFrame] = {};
  ct485::Frame out;

  ct485::Frame empty;  // all defaults, payloadLen 0
  empty.msgType = static_cast<uint8_t>(ct485::MsgType::kEcho);
  TEST_ASSERT_EQUAL_UINT32(12, ct485::encode(empty, buf));
  TEST_ASSERT_TRUE(ct485::decode(buf, 12, out));
  TEST_ASSERT_EQUAL_UINT8(0, out.payloadLen);

  ct485::Frame big = makeDemandFrame();
  big.payloadLen = static_cast<uint8_t>(ct485::kMaxPayload);
  for (size_t i = 0; i < ct485::kMaxPayload; i++) {
    big.payload[i] = static_cast<uint8_t>(i);
  }
  TEST_ASSERT_EQUAL_UINT32(ct485::kMaxFrame, ct485::encode(big, buf));
  TEST_ASSERT_TRUE(ct485::decode(buf, ct485::kMaxFrame, out));
  TEST_ASSERT_EQUAL_UINT8(ct485::kMaxPayload, out.payloadLen);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(big.payload, out.payload, ct485::kMaxPayload);
}

static void test_encode_rejects_oversize_payload() {
  ct485::Frame f = makeDemandFrame();
  f.payloadLen = static_cast<uint8_t>(ct485::kMaxPayload + 1);  // 241
  uint8_t buf[ct485::kMaxFrame + 16] = {};
  TEST_ASSERT_EQUAL_UINT32(0, ct485::encode(f, buf));
}

static void test_decode_rejects_corrupt_checksum() {
  ct485::Frame f = makeDemandFrame();
  uint8_t buf[ct485::kMaxFrame] = {};
  const size_t total = ct485::encode(f, buf);

  ct485::Frame out;
  buf[total - 1] ^= 0xFF;  // corrupt checksum byte 2
  TEST_ASSERT_FALSE(ct485::decode(buf, total, out));
  buf[total - 1] ^= 0xFF;

  buf[ct485::kHeaderLen] ^= 0x40;  // corrupt a payload byte
  TEST_ASSERT_FALSE(ct485::decode(buf, total, out));
  buf[ct485::kHeaderLen] ^= 0x40;
  TEST_ASSERT_TRUE(ct485::decode(buf, total, out));  // pristine again
}

static void test_decode_rejects_length_mismatch() {
  ct485::Frame f = makeDemandFrame();
  uint8_t buf[ct485::kMaxFrame] = {};
  const size_t total = ct485::encode(f, buf);

  ct485::Frame out;
  TEST_ASSERT_FALSE(ct485::decode(buf, total - 1, out));  // truncated
  TEST_ASSERT_FALSE(ct485::decode(buf, total + 1, out));  // extra byte
  TEST_ASSERT_FALSE(ct485::decode(buf, 0, out));          // empty
  TEST_ASSERT_FALSE(ct485::decode(buf, 11, out));         // runt (< 12)

  uint8_t bad[ct485::kMaxFrame + 16] = {};
  bad[ct485::kOffPayloadLen] = 0xF1;  // 241 > kMaxPayload, len "matches" 241+12
  TEST_ASSERT_FALSE(ct485::decode(bad, 241u + 12u, out));
}

// ---------- openHAB sample frame (docs/02 §8) ----------

// Parser sanity-test frame from the openHAB ClimateTalk thread. dst/src=0x30
// is an ASCII-'0' adapter artifact in the original capture. HONEST STATUS:
// the published capture is TRUNCATED — buf[9]=0x7C declares a 124-byte
// payload (136-byte total frame) but only these 22 bytes were posted, and the
// checksum bytes are absent. So it CANNOT pass full validation: decode()
// correctly rejects it for length, the accumulator counts it as badLength on
// gap-close, and the Fletcher result is unverifiable for this frame. What it
// DOES exercise: header-offset interpretation (payloadLen at [9] -> expected
// total len+12) and the reject path for torn captures.
static const uint8_t kOpenhabSample[] = {
    0x30, 0x30, 0x7d, 0x07, 0xd4, 0xd1, 0x9f, 0x9a, 0xc6, 0x7c,
    0x10, 0xa2, 0x01, 0xff, 0x02, 0x00, 0x00, 0x00, 0xa5, 0x00,
    0xa0, 0x11};

static void test_openhab_sample_header_and_reject() {
  TEST_ASSERT_EQUAL_UINT32(22, sizeof(kOpenhabSample));
  // Header fields read at the documented offsets.
  TEST_ASSERT_EQUAL_HEX8(0x30, kOpenhabSample[ct485::kOffDst]);  // adapter artifact
  TEST_ASSERT_EQUAL_HEX8(0x30, kOpenhabSample[ct485::kOffSrc]);  // adapter artifact
  TEST_ASSERT_EQUAL_HEX8(0x7C, kOpenhabSample[ct485::kOffPayloadLen]);
  // Declared payload 124 -> full frame would be 136 bytes; capture has 22.
  TEST_ASSERT_EQUAL_UINT32(
      136, kOpenhabSample[ct485::kOffPayloadLen] + ct485::kHeaderLen + ct485::kChecksumLen);

  ct485::Frame out;
  TEST_ASSERT_FALSE(ct485::decode(kOpenhabSample, sizeof(kOpenhabSample), out));

  // Through the accumulator it is a torn frame: badLength on gap-close.
  ct485::FrameAccumulator acc;
  feedAll(acc, kOpenhabSample, sizeof(kOpenhabSample));
  TEST_ASSERT_FALSE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().framesOk);
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badLength);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().badChecksum);
}

// ---------- FrameAccumulator ----------

static void test_accumulator_two_frames_with_gaps() {
  ct485::Frame a = makeDemandFrame();
  ct485::Frame b = makeDemandFrame();
  b.msgType = static_cast<uint8_t>(ct485::MsgType::kGetStatus);
  b.payloadLen = 0;
  uint8_t rawA[ct485::kMaxFrame], rawB[ct485::kMaxFrame];
  const size_t lenA = ct485::encode(a, rawA);
  const size_t lenB = ct485::encode(b, rawB);

  ct485::FrameAccumulator acc;
  // Frame A bytes; nothing completes mid-frame.
  TEST_ASSERT_FALSE(feedAll(acc, rawA, lenA));
  // First byte of B arrives after a gap -> A completes on that feed.
  TEST_ASSERT_TRUE(acc.feed(rawB[0], true));
  TEST_ASSERT_EQUAL_UINT32(lenA, acc.frameLen());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rawA, acc.frame(), lenA);
  ct485::Frame outA;
  TEST_ASSERT_TRUE(ct485::decode(acc.frame(), acc.frameLen(), outA));
  TEST_ASSERT_EQUAL_UINT8(a.msgType, outA.msgType);

  for (size_t i = 1; i < lenB; i++) TEST_ASSERT_FALSE(acc.feed(rawB[i], false));
  // Bus goes idle -> B completes on flush.
  TEST_ASSERT_TRUE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(lenB, acc.frameLen());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rawB, acc.frame(), lenB);

  TEST_ASSERT_EQUAL_UINT32(2, acc.counters().framesOk);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().badChecksum);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().badLength);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().overruns);
}

static void test_accumulator_torn_frame_then_good() {
  ct485::Frame g = makeDemandFrame();
  uint8_t raw[ct485::kMaxFrame];
  const size_t len = ct485::encode(g, raw);

  ct485::FrameAccumulator acc;
  // Torn frame: only the first 8 bytes arrive before the bus goes quiet.
  feedAll(acc, raw, 8);
  // Next frame starts after the gap; torn fragment is rejected, not emitted.
  TEST_ASSERT_FALSE(acc.feed(raw[0], true));
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badLength);
  for (size_t i = 1; i < len; i++) acc.feed(raw[i], false);
  TEST_ASSERT_TRUE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().framesOk);
  TEST_ASSERT_EQUAL_UINT32(len, acc.frameLen());
  ct485::Frame out;
  TEST_ASSERT_TRUE(ct485::decode(acc.frame(), acc.frameLen(), out));
}

static void test_accumulator_bad_checksum_counted() {
  ct485::Frame g = makeDemandFrame();
  uint8_t raw[ct485::kMaxFrame];
  const size_t len = ct485::encode(g, raw);
  raw[ct485::kHeaderLen + 1] ^= 0x08;  // corrupt payload, length stays right

  ct485::FrameAccumulator acc;
  feedAll(acc, raw, len);
  TEST_ASSERT_FALSE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().framesOk);
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badChecksum);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().badLength);
}

static void test_accumulator_overrun_counted_once_then_recovers() {
  ct485::FrameAccumulator acc;
  // 300 gapless bytes: > kMaxFrame (252) -> one overrun, extra bytes dropped.
  for (size_t i = 0; i < 300; i++) acc.feed(0x55, i == 0);
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().overruns);

  // Recovery: a good frame after the next gap still parses.
  ct485::Frame g = makeDemandFrame();
  uint8_t raw[ct485::kMaxFrame];
  const size_t len = ct485::encode(g, raw);
  TEST_ASSERT_FALSE(feedAll(acc, raw, len));  // gap-close of overrun emits nothing
  TEST_ASSERT_TRUE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().framesOk);
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().overruns);  // not double-counted
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().badLength); // overrun != badLength
}

static void test_accumulator_salvages_merged_burst() {
  // Two frames back-to-back with NO gap: gap framing must reject the merged
  // buffer (badLength) but stash the raw bytes for PC-side resync recovery.
  ct485::Frame a = makeDemandFrame();
  ct485::Frame b = makeDemandFrame();
  b.msgType = static_cast<uint8_t>(ct485::MsgType::kGetStatus);
  b.payloadLen = 0;
  uint8_t rawA[ct485::kMaxFrame], rawB[ct485::kMaxFrame];
  const size_t lenA = ct485::encode(a, rawA);
  const size_t lenB = ct485::encode(b, rawB);

  ct485::FrameAccumulator acc;
  feedAll(acc, rawA, lenA);
  for (size_t i = 0; i < lenB; i++) acc.feed(rawB[i], false);  // gapless join
  TEST_ASSERT_FALSE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badLength);

  uint8_t rej[ct485::kMaxFrame];
  const size_t n = acc.takeRejected(rej);
  TEST_ASSERT_EQUAL_UINT32(lenA + lenB, n);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rawA, rej, lenA);          // both frames intact
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rawB, rej + lenA, lenB);
  TEST_ASSERT_EQUAL_UINT32(0, acc.takeRejected(rej));      // take clears the slot

  // A bad-checksum reject is stashed too.
  uint8_t rawC[ct485::kMaxFrame];
  const size_t lenC = ct485::encode(a, rawC);
  rawC[ct485::kHeaderLen] ^= 0x40;
  feedAll(acc, rawC, lenC);
  TEST_ASSERT_FALSE(acc.flush());
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badChecksum);
  TEST_ASSERT_EQUAL_UINT32(lenC, acc.takeRejected(rej));
  TEST_ASSERT_EQUAL_UINT8_ARRAY(rawC, rej, lenC);
}

static void test_accumulator_empty_gap_is_not_a_frame() {
  ct485::FrameAccumulator acc;
  TEST_ASSERT_FALSE(acc.feed(0x01, true));  // first byte ever, nothing to close
  TEST_ASSERT_FALSE(acc.flush());           // 12 bytes needed; lone byte = badLength
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badLength);
  TEST_ASSERT_FALSE(acc.flush());           // truly empty close: no frame, no count
  TEST_ASSERT_EQUAL_UINT32(1, acc.counters().badLength);
  TEST_ASSERT_EQUAL_UINT32(0, acc.counters().framesOk);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_fletcher_known_vector);
  RUN_TEST(test_fletcher_detects_single_bit_flip);
  RUN_TEST(test_roundtrip_encode_decode);
  RUN_TEST(test_roundtrip_zero_and_max_payload);
  RUN_TEST(test_encode_rejects_oversize_payload);
  RUN_TEST(test_decode_rejects_corrupt_checksum);
  RUN_TEST(test_decode_rejects_length_mismatch);
  RUN_TEST(test_openhab_sample_header_and_reject);
  RUN_TEST(test_accumulator_two_frames_with_gaps);
  RUN_TEST(test_accumulator_torn_frame_then_good);
  RUN_TEST(test_accumulator_bad_checksum_counted);
  RUN_TEST(test_accumulator_overrun_counted_once_then_recovers);
  RUN_TEST(test_accumulator_salvages_merged_burst);
  RUN_TEST(test_accumulator_empty_gap_is_not_a_frame);
  return UNITY_END();
}
