// Unit tests for CaptureBuffer: bounded ring of frame + annotation records,
// oldest-first eviction with wrap-around, insertion-order drain, and the
// drain-format round-trip through a C++ copy of tools/ct485_decode.py's
// parse_line() contract (the on-wire compatibility pin for issue #57).
#include <unity.h>

#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "CaptureBuffer.h"

using capture::CaptureBuffer;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------------------
// Copy of the decoder's line parser (tools/ct485_decode.py parse_line):
//   - lines starting with '#' (after lstrip) are comments -> no bytes
//   - bytes = the LONGEST run of whitespace-separated 2-hex-digit tokens
//   - ts_ms / gap_us = the last two all-digit tokens BEFORE that run
// Mirrors only the paths our drain format exercises; if drain output stops
// round-tripping here it will also break the real decoder.
// ---------------------------------------------------------------------------
struct ParsedLine {
  bool comment = false;
  long tsMs = -1;
  long gapUs = -1;
  std::vector<uint8_t> bytes;
};

static bool isHexPair(const std::string& t) {
  return t.size() == 2 && isxdigit(static_cast<unsigned char>(t[0])) &&
         isxdigit(static_cast<unsigned char>(t[1]));
}

static bool isAllDigits(const std::string& t) {
  if (t.empty()) return false;
  for (char c : t)
    if (!isdigit(static_cast<unsigned char>(c))) return false;
  return true;
}

static ParsedLine parseLikeDecoder(const std::string& line) {
  ParsedLine out;
  size_t first = line.find_first_not_of(" \t");
  if (first != std::string::npos && line[first] == '#') {
    out.comment = true;
    return out;
  }
  std::vector<std::string> tokens;
  std::string cur;
  for (char c : line + " ") {
    if (isspace(static_cast<unsigned char>(c))) {
      if (!cur.empty()) tokens.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  // Longest run of hex-pair tokens.
  size_t bestStart = 0, bestLen = 0, runStart = 0, runLen = 0;
  for (size_t i = 0; i <= tokens.size(); ++i) {
    if (i < tokens.size() && isHexPair(tokens[i])) {
      if (runLen == 0) runStart = i;
      if (++runLen > bestLen) { bestLen = runLen; bestStart = runStart; }
    } else {
      runLen = 0;
    }
  }
  for (size_t i = bestStart; i < bestStart + bestLen; ++i)
    out.bytes.push_back(static_cast<uint8_t>(strtol(tokens[i].c_str(), nullptr, 16)));
  // Last two all-digit tokens before the run -> ts, gap.
  std::vector<std::string> pre;
  for (size_t i = 0; i < bestStart && bestLen > 0; ++i)
    if (isAllDigits(tokens[i])) pre.push_back(tokens[i]);
  if (pre.size() >= 2) pre.erase(pre.begin(), pre.end() - 2);
  if (!pre.empty()) out.tsMs = strtol(pre[0].c_str(), nullptr, 10);
  if (pre.size() == 2) out.gapUs = strtol(pre[1].c_str(), nullptr, 10);
  return out;
}

// ---------------------------------------------------------------------------

static std::vector<uint8_t> testFrame(size_t len, uint8_t seed) {
  std::vector<uint8_t> v(len);
  for (size_t i = 0; i < len; ++i) v[i] = static_cast<uint8_t>(seed + i);
  return v;
}

static std::vector<std::string> drainAll(CaptureBuffer& cb) {
  std::vector<std::string> lines;
  char buf[capture::kMaxLineLen];
  size_t n;
  while ((n = cb.drainNext(buf, sizeof(buf))) > 0) {
    TEST_ASSERT_EQUAL_UINT32(n, strlen(buf));
    lines.emplace_back(buf);
  }
  return lines;
}

static void test_roundtrip_frame_line_through_decoder_parser() {
  CaptureBuffer cb;
  const auto raw = testFrame(16, 0x10);
  TEST_ASSERT_TRUE(cb.addFrame(123456, 4200, raw.data(), raw.size(), true));
  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());

  ParsedLine p = parseLikeDecoder(lines[0]);
  TEST_ASSERT_FALSE(p.comment);
  TEST_ASSERT_EQUAL_INT(123456, p.tsMs);
  TEST_ASSERT_EQUAL_INT(4200, p.gapUs);
  TEST_ASSERT_EQUAL_UINT32(raw.size(), p.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(raw.data(), p.bytes.data(), raw.size());
  TEST_ASSERT_EQUAL_UINT32(0, cb.recordCount());
}

static void test_roundtrip_bad_fcs_marker_is_ignored_by_parser() {
  CaptureBuffer cb;
  const auto raw = testFrame(12, 0x30);
  TEST_ASSERT_TRUE(cb.addFrame(99, 3600, raw.data(), raw.size(), false));
  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());
  // Marker present for humans/grep...
  TEST_ASSERT_NOT_NULL(strstr(lines[0].c_str(), "!fcs"));
  // ...but transparent to the decoder's tokenizer (trailing non-hex token).
  ParsedLine p = parseLikeDecoder(lines[0]);
  TEST_ASSERT_EQUAL_INT(99, p.tsMs);
  TEST_ASSERT_EQUAL_INT(3600, p.gapUs);
  TEST_ASSERT_EQUAL_UINT32(raw.size(), p.bytes.size());
}

static void test_two_digit_millis_does_not_confuse_hex_run() {
  // millis "42" is itself a valid hex-pair token; the frame's longer hex run
  // must still win (mirrors the decoder's longest-run rule).
  CaptureBuffer cb;
  const auto raw = testFrame(12, 0xA0);
  TEST_ASSERT_TRUE(cb.addFrame(42, 77, raw.data(), raw.size(), true));
  auto lines = drainAll(cb);
  ParsedLine p = parseLikeDecoder(lines[0]);
  TEST_ASSERT_EQUAL_INT(42, p.tsMs);
  TEST_ASSERT_EQUAL_INT(77, p.gapUs);
  TEST_ASSERT_EQUAL_UINT32(raw.size(), p.bytes.size());
  TEST_ASSERT_EQUAL_UINT8(0xA0, p.bytes[0]);
}

static void test_annotation_is_comment_line_with_text() {
  CaptureBuffer cb;
  TEST_ASSERT_TRUE(cb.addAnnotation(5000, "forced defrost, FO 3"));
  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());
  ParsedLine p = parseLikeDecoder(lines[0]);
  TEST_ASSERT_TRUE(p.comment);  // decoder skips it: yields no bytes
  TEST_ASSERT_NOT_NULL(strstr(lines[0].c_str(), "5000ms"));
  TEST_ASSERT_NOT_NULL(strstr(lines[0].c_str(), "forced defrost, FO 3"));
}

static void test_annotation_newlines_flattened_and_truncated() {
  CaptureBuffer cb;
  std::string evil = "line1\nline2\r";
  evil += std::string(300, 'x');  // > kMaxAnnotationLen
  TEST_ASSERT_TRUE(cb.addAnnotation(1, evil.c_str()));
  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(1, lines.size());
  TEST_ASSERT_NULL(strchr(lines[0].c_str(), '\n'));
  TEST_ASSERT_NULL(strchr(lines[0].c_str(), '\r'));
  TEST_ASSERT_TRUE(lines[0].size() <
                   capture::kMaxAnnotationLen + 32);  // truncated, not 300+
}

static void test_interleave_preserves_insertion_order() {
  CaptureBuffer cb;
  const auto f1 = testFrame(12, 1);
  const auto f2 = testFrame(12, 2);
  cb.addFrame(100, 10, f1.data(), f1.size(), true);
  cb.addAnnotation(150, "between");
  cb.addFrame(200, 20, f2.data(), f2.size(), true);
  TEST_ASSERT_EQUAL_UINT32(2, cb.frameCount());
  TEST_ASSERT_EQUAL_UINT32(1, cb.annotationCount());

  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(3, lines.size());
  TEST_ASSERT_EQUAL_INT(100, parseLikeDecoder(lines[0]).tsMs);
  TEST_ASSERT_TRUE(parseLikeDecoder(lines[1]).comment);
  TEST_ASSERT_EQUAL_INT(200, parseLikeDecoder(lines[2]).tsMs);
}

static void test_wraparound_evicts_oldest_and_keeps_order() {
  // Budget for only a few records: 12-byte frames cost 12 (hdr) + 12 = 24 B.
  CaptureBuffer cb(100);
  const size_t kFrames = 50;
  for (size_t i = 0; i < kFrames; ++i) {
    const auto f = testFrame(12, static_cast<uint8_t>(i));
    TEST_ASSERT_TRUE(cb.addFrame(1000 + i, 10, f.data(), f.size(), true));
    TEST_ASSERT_TRUE(cb.bytesUsed() <= cb.budgetBytes());
  }
  TEST_ASSERT_TRUE(cb.evictedRecords() > 0);
  TEST_ASSERT_EQUAL_UINT32(kFrames, cb.frameCount() + cb.evictedRecords());

  auto lines = drainAll(cb);
  TEST_ASSERT_EQUAL_UINT32(0, cb.bytesUsed());
  // Survivors are the NEWEST records, contiguous and in order.
  long expect = 1000 + static_cast<long>(kFrames - lines.size());
  for (const auto& line : lines) {
    ParsedLine p = parseLikeDecoder(line);
    TEST_ASSERT_EQUAL_INT(expect, p.tsMs);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(expect - 1000), p.bytes[0]);
    ++expect;
  }
}

static void test_max_frame_record_roundtrips() {
  CaptureBuffer cb;
  const auto raw = testFrame(252, 0);
  TEST_ASSERT_TRUE(cb.addFrame(1, 2, raw.data(), raw.size(), true));
  auto lines = drainAll(cb);
  TEST_ASSERT_TRUE(lines[0].size() < capture::kMaxLineLen);
  ParsedLine p = parseLikeDecoder(lines[0]);
  TEST_ASSERT_EQUAL_UINT32(252, p.bytes.size());
  TEST_ASSERT_EQUAL_UINT8_ARRAY(raw.data(), p.bytes.data(), 252);
}

static void test_oversized_record_rejected_without_dropping_data() {
  CaptureBuffer cb(40);  // smaller than a 252-byte frame record
  const auto small = testFrame(12, 9);
  TEST_ASSERT_TRUE(cb.addFrame(1, 1, small.data(), small.size(), true));
  const auto big = testFrame(252, 0);
  TEST_ASSERT_FALSE(cb.addFrame(2, 2, big.data(), big.size(), true));
  TEST_ASSERT_EQUAL_UINT32(1, cb.frameCount());  // small survivor untouched
  TEST_ASSERT_EQUAL_UINT32(0, cb.evictedRecords());
}

static void test_clear_resets_everything() {
  CaptureBuffer cb(100);
  const auto f = testFrame(12, 1);
  for (int i = 0; i < 10; ++i) cb.addFrame(i, 0, f.data(), f.size(), true);
  cb.addAnnotation(11, "note");
  cb.clear();
  TEST_ASSERT_EQUAL_UINT32(0, cb.recordCount());
  TEST_ASSERT_EQUAL_UINT32(0, cb.bytesUsed());
  TEST_ASSERT_EQUAL_UINT32(0, cb.evictedRecords());
  char buf[capture::kMaxLineLen];
  TEST_ASSERT_EQUAL_UINT32(0, cb.drainNext(buf, sizeof(buf)));
  // Still usable after clear.
  TEST_ASSERT_TRUE(cb.addFrame(99, 1, f.data(), f.size(), true));
  TEST_ASSERT_EQUAL_UINT32(1, cb.frameCount());
}

static void test_truncating_out_buffer_still_consumes_record() {
  CaptureBuffer cb;
  const auto f1 = testFrame(252, 1);
  const auto f2 = testFrame(12, 2);
  cb.addFrame(111, 1, f1.data(), f1.size(), true);
  cb.addFrame(222, 2, f2.data(), f2.size(), true);
  char tiny[16];
  size_t n = cb.drainNext(tiny, sizeof(tiny));
  TEST_ASSERT_TRUE(n > 0 && n < sizeof(tiny));
  TEST_ASSERT_EQUAL_UINT32(n, strlen(tiny));
  // Big record fully consumed despite truncation; next drain is frame 2.
  char buf[capture::kMaxLineLen];
  TEST_ASSERT_TRUE(cb.drainNext(buf, sizeof(buf)) > 0);
  TEST_ASSERT_EQUAL_INT(222, parseLikeDecoder(buf).tsMs);
  TEST_ASSERT_EQUAL_UINT32(0, cb.recordCount());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_roundtrip_frame_line_through_decoder_parser);
  RUN_TEST(test_roundtrip_bad_fcs_marker_is_ignored_by_parser);
  RUN_TEST(test_two_digit_millis_does_not_confuse_hex_run);
  RUN_TEST(test_annotation_is_comment_line_with_text);
  RUN_TEST(test_annotation_newlines_flattened_and_truncated);
  RUN_TEST(test_interleave_preserves_insertion_order);
  RUN_TEST(test_wraparound_evicts_oldest_and_keeps_order);
  RUN_TEST(test_max_frame_record_roundtrips);
  RUN_TEST(test_oversized_record_rejected_without_dropping_data);
  RUN_TEST(test_clear_resets_everything);
  RUN_TEST(test_truncating_out_buffer_still_consumes_record);
  return UNITY_END();
}
