// Unit tests for lib/OtaCatalog — semver compare (#113), catalog.json parsing
// and target/hwRev/version resolution (#60). Tolerance philosophy mirrors
// lib/HaMqtt: malformed documents yield nothing, malformed entries are
// skipped, and a resolution never offers an image to the wrong hardware.
#include <unity.h>

#include <string>
#include <vector>

#include "OtaCatalog.h"

using namespace dettson::ota;

void setUp() {}
void tearDown() {}

// ---------- semver ----------

static void test_semver_parse_valid() {
  Semver v = parseSemver("0.3.0");
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_EQUAL_INT(0, v.major);
  TEST_ASSERT_EQUAL_INT(3, v.minor);
  TEST_ASSERT_EQUAL_INT(0, v.patch);

  v = parseSemver("12.34.56");
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_EQUAL_INT(12, v.major);
  TEST_ASSERT_EQUAL_INT(34, v.minor);
  TEST_ASSERT_EQUAL_INT(56, v.patch);

  // Build metadata ignored (SLYTHERM_FW_BUILD style).
  v = parseSemver("0.3.0+g1a2b3c4-dirty");
  TEST_ASSERT_TRUE(v.ok);
  TEST_ASSERT_EQUAL_INT(3, v.minor);
}

static void test_semver_parse_rejects_junk() {
  TEST_ASSERT_FALSE(parseSemver(nullptr).ok);
  TEST_ASSERT_FALSE(parseSemver("").ok);
  TEST_ASSERT_FALSE(parseSemver("1.2").ok);
  TEST_ASSERT_FALSE(parseSemver("1.2.3.4").ok);
  TEST_ASSERT_FALSE(parseSemver("v1.2.3").ok);
  TEST_ASSERT_FALSE(parseSemver("1.2.x").ok);
  TEST_ASSERT_FALSE(parseSemver("1.2.3-rc1").ok);  // pre-release unsupported
  TEST_ASSERT_FALSE(parseSemver("-1.2.3").ok);
  TEST_ASSERT_FALSE(parseSemver("1..3").ok);
  TEST_ASSERT_FALSE(parseSemver("0.0.0-dev").ok);  // the host-build fallback is not a version
}

static void test_semver_compare() {
  auto cmp = [](const char* a, const char* b) {
    return cmpSemver(parseSemver(a), parseSemver(b));
  };
  TEST_ASSERT_EQUAL_INT(0, cmp("1.2.3", "1.2.3"));
  TEST_ASSERT_TRUE(cmp("1.2.3", "1.2.4") < 0);
  TEST_ASSERT_TRUE(cmp("1.2.10", "1.2.9") > 0);   // numeric, not lexicographic
  TEST_ASSERT_TRUE(cmp("1.10.0", "1.9.9") > 0);
  TEST_ASSERT_TRUE(cmp("2.0.0", "1.99.99") > 0);
  TEST_ASSERT_EQUAL_INT(0, cmp("0.3.0+ga1b2c3", "0.3.0"));  // metadata ignored
}

// ---------- catalog parse ----------

static const char* kTwoTargets =
    "{\"schema\":1,\"targets\":["
    "{\"id\":\"wall-s3\",\"name\":\"SlyTherm Controller\","
    "\"chipFamily\":\"ESP32-S3\",\"hwRev\":\"s3-43b-r1.1\","
    "\"version\":\"0.4.0\","
    "\"appUrl\":\"https://github.com/SlyWombat/SlyTherm/releases/download/v0.4.0/wall-s3-0.4.0.app.bin\","
    "\"appSize\":1382416,"
    "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\","
    "\"sig\":\"MEUCIQDbase64==\",\"minVersion\":\"0.3.0\",\"mandatory\":false,"
    "\"notesUrl\":\"https://github.com/SlyWombat/SlyTherm/releases/tag/v0.4.0\"},"
    "{\"id\":\"remote-p4\",\"hwRev\":\"p4-m3-r1\",\"version\":\"0.4.0\","
    "\"appUrl\":\"https://example.com/remote-p4-0.4.0.app.bin\","
    "\"sha256\":\"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff\"}"
    "]}";

static void test_catalog_parses_two_targets() {
  std::vector<CatalogEntry> es;
  TEST_ASSERT_TRUE(parseCatalogJson(kTwoTargets, es));
  TEST_ASSERT_EQUAL_UINT(2, es.size());

  const CatalogEntry& w = es[0];
  TEST_ASSERT_EQUAL_STRING("wall-s3", w.id.c_str());
  TEST_ASSERT_EQUAL_STRING("s3-43b-r1.1", w.hwRev.c_str());
  TEST_ASSERT_EQUAL_STRING("0.4.0", w.version.c_str());
  TEST_ASSERT_EQUAL_STRING("0.3.0", w.minVersion.c_str());
  TEST_ASSERT_EQUAL_UINT32(1382416u, w.appSize);
  TEST_ASSERT_FALSE(w.mandatory);
  TEST_ASSERT_EQUAL_STRING("SlyTherm Controller", w.name.c_str());
  TEST_ASSERT_TRUE(w.appUrl.find("wall-s3-0.4.0.app.bin") != std::string::npos);

  // Optional fields absent on the second entry -> defaults.
  const CatalogEntry& r = es[1];
  TEST_ASSERT_EQUAL_STRING("remote-p4", r.id.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.minVersion.c_str());
  TEST_ASSERT_EQUAL_STRING("", r.sig.c_str());
  TEST_ASSERT_EQUAL_UINT32(0u, r.appSize);
}

static void test_catalog_rejects_structural_junk() {
  std::vector<CatalogEntry> es;
  TEST_ASSERT_FALSE(parseCatalogJson(nullptr, es));
  TEST_ASSERT_FALSE(parseCatalogJson("", es));
  TEST_ASSERT_FALSE(parseCatalogJson("not json", es));
  TEST_ASSERT_FALSE(parseCatalogJson("{}", es));                         // no schema
  TEST_ASSERT_FALSE(parseCatalogJson("{\"schema\":2,\"targets\":[]}", es));  // wrong schema
  TEST_ASSERT_FALSE(parseCatalogJson("{\"schema\":1}", es));             // no targets
  TEST_ASSERT_FALSE(parseCatalogJson("{\"schema\":1,\"targets\":[42]}", es));
  // Empty targets is a legitimate "nothing published yet".
  TEST_ASSERT_TRUE(parseCatalogJson("{\"schema\":1,\"targets\":[]}", es));
  TEST_ASSERT_EQUAL_UINT(0, es.size());
}

static void test_catalog_skips_invalid_entries() {
  // Bad sha256 length, bad version, missing appUrl, duplicate (id,hwRev):
  // each skipped; the one good entry survives.
  std::string good =
      "{\"id\":\"wall-s3\",\"hwRev\":\"r1\",\"version\":\"1.0.0\","
      "\"appUrl\":\"https://x/a.bin\","
      "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}";
  std::string doc = "{\"schema\":1,\"targets\":["
      "{\"id\":\"a\",\"hwRev\":\"r1\",\"version\":\"1.0.0\",\"appUrl\":\"https://x\",\"sha256\":\"abc\"},"
      "{\"id\":\"b\",\"hwRev\":\"r1\",\"version\":\"1.0\",\"appUrl\":\"https://x\","
      "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"},"
      "{\"id\":\"c\",\"hwRev\":\"r1\",\"version\":\"1.0.0\","
      "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}," +
      good + "," + good + "]}";
  std::vector<CatalogEntry> es;
  TEST_ASSERT_TRUE(parseCatalogJson(doc.c_str(), es));
  TEST_ASSERT_EQUAL_UINT(1, es.size());
  TEST_ASSERT_EQUAL_STRING("wall-s3", es[0].id.c_str());
}

static void test_catalog_rejects_uppercase_sha_and_bad_optionals() {
  // Uppercase hex sha rejected (contract: lowercase), bad mandatory rejected.
  std::vector<CatalogEntry> es;
  std::string doc =
      "{\"schema\":1,\"targets\":["
      "{\"id\":\"a\",\"hwRev\":\"r1\",\"version\":\"1.0.0\",\"appUrl\":\"https://x\","
      "\"sha256\":\"ABCDEF0000000000000000000000000000000000000000000000000000000000\"},"
      "{\"id\":\"b\",\"hwRev\":\"r1\",\"version\":\"1.0.0\",\"appUrl\":\"https://x\","
      "\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\","
      "\"mandatory\":\"yes\"}]}";
  TEST_ASSERT_TRUE(parseCatalogJson(doc.c_str(), es));
  TEST_ASSERT_EQUAL_UINT(0, es.size());
}

// ---------- resolve ----------

static void test_resolve_matrix() {
  std::vector<CatalogEntry> es;
  TEST_ASSERT_TRUE(parseCatalogJson(kTwoTargets, es));

  // Update available: running 0.3.0 < 0.4.0, minVersion 0.3.0 satisfied.
  Resolution r = resolve(es, "wall-s3", "s3-43b-r1.1", "0.3.0");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_TRUE(r.updateAvailable);
  TEST_ASSERT_FALSE(r.blockedByMinVersion);
  TEST_ASSERT_EQUAL_STRING("0.4.0", r.entry.version.c_str());

  // Up to date.
  r = resolve(es, "wall-s3", "s3-43b-r1.1", "0.4.0");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_FALSE(r.updateAvailable);

  // Running ahead of catalog (dev build) -> no "update".
  r = resolve(es, "wall-s3", "s3-43b-r1.1", "0.5.0");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_FALSE(r.updateAvailable);

  // Blocked by minVersion: running 0.2.9 < minVersion 0.3.0.
  r = resolve(es, "wall-s3", "s3-43b-r1.1", "0.2.9");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_FALSE(r.updateAvailable);
  TEST_ASSERT_TRUE(r.blockedByMinVersion);

  // Wrong hwRev: never offered, even though the id matches.
  r = resolve(es, "wall-s3", "s3-43b-r2.0", "0.3.0");
  TEST_ASSERT_FALSE(r.targetFound);
  TEST_ASSERT_FALSE(r.updateAvailable);

  // Unknown target.
  r = resolve(es, "sniffer-s3", "r1", "0.3.0");
  TEST_ASSERT_FALSE(r.targetFound);

  // Invalid running version (host fallback "0.0.0-dev"): found, never offered.
  r = resolve(es, "wall-s3", "s3-43b-r1.1", "0.0.0-dev");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_FALSE(r.updateAvailable);

  // Remote target with no minVersion: 0.0.0 default never blocks.
  r = resolve(es, "remote-p4", "p4-m3-r1", "0.1.0");
  TEST_ASSERT_TRUE(r.targetFound);
  TEST_ASSERT_TRUE(r.updateAvailable);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_semver_parse_valid);
  RUN_TEST(test_semver_parse_rejects_junk);
  RUN_TEST(test_semver_compare);
  RUN_TEST(test_catalog_parses_two_targets);
  RUN_TEST(test_catalog_rejects_structural_junk);
  RUN_TEST(test_catalog_skips_invalid_entries);
  RUN_TEST(test_catalog_rejects_uppercase_sha_and_bad_optionals);
  RUN_TEST(test_resolve_matrix);
  return UNITY_END();
}
