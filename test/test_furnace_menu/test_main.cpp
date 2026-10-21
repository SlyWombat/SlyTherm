// FurnaceMenu: page store, HA value extraction, Diag text, and the #228 poll
// cadence — all fed with the REAL 2026-10-18 captures (test/fixtures).
#include <unity.h>

#include <cstring>
#include <string>

#include "Ct485Core.h"
#include "FurnaceMenu.h"
#include "../fixtures/furnace_menu_pages.h"

using namespace ct485;

void setUp() {}
void tearDown() {}

static Frame reply(const uint8_t* p, size_t n, uint8_t dst = 0xFF) {
  Frame f;
  f.dst = dst; f.src = 0x02; f.subnet = 0x02; f.sendMethod = 0x02;
  f.srcNodeType = 0x02; f.msgType = 0xC1; f.packetNum = 0x20;
  f.payloadLen = static_cast<uint8_t>(n);
  memcpy(f.payload, p, n);
  return f;
}
#define REPLY(arr) reply(arr, sizeof(arr))

static std::string valueOf(const std::vector<FurnaceStatusValue>& v, const char* key) {
  for (const auto& x : v) if (strcmp(x.key, key) == 0) return x.value;
  return "<none>";
}

// ---------------------------------------------------------------- store

void test_store_ingests_every_page_and_counts_generations() {
  FurnaceMenuStore s;
  TEST_ASSERT_TRUE(s.ingest(REPLY(kMenuListPayload), 1000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kStatus1IdlePayload), 2000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kStatus2IdlePayload), 3000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kTwoWeekHistPayload), 4000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kLifeHistPayload), 5000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kFaultHistPayload), 6000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kUnitInfoPayload), 7000));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kSetupPayload), 8000));
  TEST_ASSERT_EQUAL_UINT32(8, s.generation());
  for (uint8_t p = 0; p < kFurnaceMenuPages; p++) TEST_ASSERT_TRUE(s.seen(p));
  TEST_ASSERT_EQUAL_UINT32(3000, s.heardMs(2));
  TEST_ASSERT_EQUAL_UINT32(3, s.pageGeneration(2));
  TEST_ASSERT_EQUAL_STRING("DETTSON 105K V3", s.page(6)->rows[0].value.c_str());
}

void test_store_newer_page_replaces_older() {
  FurnaceMenuStore s;
  s.ingest(REPLY(kStatus1IdlePayload), 1000);
  s.ingest(REPLY(kStatus1HeatPayload), 9000);
  TEST_ASSERT_EQUAL_UINT32(9000, s.heardMs(1));
  TEST_ASSERT_EQUAL_STRING("GOOD", s.page(1)->rows[8].value.c_str());
}

void test_store_rejects_non_menu_and_keeps_good_copy_over_torn_one() {
  FurnaceMenuStore s;
  Frame other = REPLY(kStatus1IdlePayload);
  other.msgType = 0x41;                     // the REQUEST direction
  TEST_ASSERT_FALSE(s.ingest(other, 1));
  TEST_ASSERT_TRUE(s.ingest(REPLY(kStatus1HeatPayload), 1000));
  const uint8_t torn[] = {0x01, 0x01, 0, 0, 0, 0xF0, 0xB6, 0xC5, 0xD0, 'S'};
  TEST_ASSERT_FALSE(s.ingest(reply(torn, sizeof(torn)), 2000));
  TEST_ASSERT_EQUAL_UINT32(1000, s.heardMs(1));  // good copy kept
  TEST_ASSERT_EQUAL_UINT32(1, s.generation());
}

void test_store_page_names_from_menu_list_or_defaults() {
  FurnaceMenuStore s;
  TEST_ASSERT_EQUAL_UINT8(7, s.pageCount());
  TEST_ASSERT_EQUAL_STRING("STATUS 1", s.pageName(1).c_str());
  TEST_ASSERT_EQUAL_STRING("SETUP", s.pageName(7).c_str());
  s.ingest(REPLY(kMenuListPayload), 1);
  TEST_ASSERT_EQUAL_UINT8(7, s.pageCount());
  TEST_ASSERT_EQUAL_STRING("LIFE  HIST", s.pageName(4).c_str());  // the furnace's spelling
}

void test_store_records_source() {
  FurnaceMenuStore s;
  s.ingest(REPLY(kStatus2IdlePayload), 1, /*answeredUs=*/true);
  s.ingest(REPLY(kStatus1IdlePayload), 1, false);
  TEST_ASSERT_TRUE(s.answeredUs(2));
  TEST_ASSERT_FALSE(s.answeredUs(1));
}

// ---------------------------------------------------------------- Diag text

void test_render_seen_page_lists_rows_with_age() {
  FurnaceMenuStore s;
  s.ingest(REPLY(kStatus1HeatPayload), 10000);
  const std::string t = s.renderPage(1, 22000);
  TEST_ASSERT_TRUE(t.rfind("STATUS 1", 0) == 0);
  TEST_ASSERT_TRUE(t.find("heard 12 s ago, via the OEM thermostat") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("GAS VLV PRCNT   100%") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("BLOWER CFM   1682") != std::string::npos);
}

void test_render_setup_shows_values_and_choices_read_only() {
  FurnaceMenuStore s;
  s.ingest(REPLY(kSetupPayload), 0);
  const std::string t = s.renderPage(7, 5u * 60u * 1000u);
  TEST_ASSERT_TRUE(t.find("heard 5 min ago") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("HEATRISE ADJUST   55F   (set on the OEM thermostat; also 60F)") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("RESET ALL DFLTS   NO") != std::string::npos);
}

void test_render_never_seen_page_says_how_to_get_it() {
  FurnaceMenuStore s;
  const std::string t = s.renderPage(5, 1000);
  TEST_ASSERT_TRUE(t.find("Not heard since the controller started") != std::string::npos);
  TEST_ASSERT_TRUE(t.find("Furnace > FAULT HIST") != std::string::npos);
}

void test_format_age_units() {
  TEST_ASSERT_EQUAL_STRING("0 s", formatAge(999).c_str());
  TEST_ASSERT_EQUAL_STRING("59 s", formatAge(59999).c_str());
  TEST_ASSERT_EQUAL_STRING("1 min", formatAge(60000).c_str());
  TEST_ASSERT_EQUAL_STRING("2 h", formatAge(2u * 3600u * 1000u).c_str());
  TEST_ASSERT_EQUAL_STRING("3 d", formatAge(3u * 86400u * 1000u).c_str());
}

// ---------------------------------------------------------------- HA values

void test_status_values_heat_call() {
  const auto v = furnaceStatusValues(decodeUserMenuPage(REPLY(kStatus1HeatPayload)));
  TEST_ASSERT_EQUAL_STRING("100", valueOf(v, "gas_valve_pct").c_str());
  TEST_ASSERT_EQUAL_STRING("GOOD", valueOf(v, "flame").c_str());
  TEST_ASSERT_EQUAL_STRING("1682", valueOf(v, "blower_cfm").c_str());
  TEST_ASSERT_EQUAL_STRING("2720", valueOf(v, "inducer_rpm").c_str());
  TEST_ASSERT_EQUAL_UINT32(4, v.size());
}

void test_status_values_idle_inducer_off_is_zero() {
  const auto v = furnaceStatusValues(decodeUserMenuPage(REPLY(kStatus1IdlePayload)));
  TEST_ASSERT_EQUAL_STRING("0", valueOf(v, "inducer_rpm").c_str());
  TEST_ASSERT_EQUAL_STRING("0", valueOf(v, "gas_valve_pct").c_str());
  TEST_ASSERT_EQUAL_STRING("OFF", valueOf(v, "flame").c_str());
}

void test_status_values_cooling_status2() {
  const auto v = furnaceStatusValues(decodeUserMenuPage(REPLY(kStatus2CoolPayload)));
  TEST_ASSERT_EQUAL_STRING("AC", valueOf(v, "mode").c_str());
  TEST_ASSERT_EQUAL_STRING("1995", valueOf(v, "blower_cfm").c_str());
  TEST_ASSERT_EQUAL_STRING("ON", valueOf(v, "eac").c_str());
  TEST_ASSERT_TRUE(valueOf(v, "motor_rpm") != "<none>");
}

void test_status_values_history_pages_carry_none() {
  TEST_ASSERT_EQUAL_UINT32(0, furnaceStatusValues(decodeUserMenuPage(REPLY(kLifeHistPayload))).size());
  TEST_ASSERT_EQUAL_UINT32(0, furnaceStatusValues(decodeUserMenuPage(REPLY(kSetupPayload))).size());
}

// ---------------------------------------------------------------- poller

void test_poller_idle_until_permitted() {
  FurnaceMenuPoller p;
  TEST_ASSERT_EQUAL_UINT8(0, p.due(0, false, true));
  TEST_ASSERT_EQUAL_UINT8(0, p.due(100000, false, true));
  TEST_ASSERT_EQUAL_UINT32(0, p.requests());
}

void test_poller_cycle_status1_then_status2_then_waits_interval() {
  FurnaceMenuPoller p;
  TEST_ASSERT_EQUAL_UINT8(1, p.due(1000, true, true));
  p.onRequested(1, 1000);
  TEST_ASSERT_EQUAL_UINT8(0, p.due(1500, true, true));   // waiting for the answer
  p.onPageHeard(1, 2000);
  TEST_ASSERT_EQUAL_UINT8(2, p.due(2000, true, true));
  p.onRequested(2, 2000);
  p.onPageHeard(2, 3000);
  TEST_ASSERT_EQUAL_UINT8(0, p.due(3000 + 29999, true, true));
  TEST_ASSERT_EQUAL_UINT8(1, p.due(3000 + 30000, true, true));   // active cadence
}

void test_poller_idle_cadence_is_five_minutes() {
  FurnaceMenuPoller p;
  p.due(0, true, false); p.onRequested(1, 0); p.onPageHeard(1, 100);
  p.due(100, true, false); p.onRequested(2, 100); p.onPageHeard(2, 200);
  TEST_ASSERT_EQUAL_UINT8(0, p.due(200 + 60000, true, false));
  TEST_ASSERT_EQUAL_UINT8(1, p.due(200 + 300000, true, false));
}

void test_poller_demand_starting_tightens_cadence() {
  FurnaceMenuPoller p;
  p.due(0, true, false); p.onRequested(1, 0); p.onPageHeard(1, 0);
  p.due(0, true, false); p.onRequested(2, 0); p.onPageHeard(2, 0);
  TEST_ASSERT_EQUAL_UINT8(0, p.due(20000, true, false));
  TEST_ASSERT_EQUAL_UINT8(1, p.due(30000, true, true));   // call started: 30 s rule
}

void test_poller_stalls_after_misses_and_backs_off() {
  // A silent furnace: every request goes unanswered. Step time in 1 s ticks
  // the way the control loop does, and ask whenever the poller says so.
  FurnaceMenuPoller p;
  uint32_t t = 0;
  while (!p.stalled() && t < 600000) {
    const uint8_t pg = p.due(t, true, true);
    if (pg != 0) p.onRequested(pg, t);
    t += 1000;
  }
  TEST_ASSERT_TRUE(p.stalled());
  TEST_ASSERT_EQUAL_UINT32(3, p.requests());    // exactly missLimit requests, no hot retry
  t -= 1000;                                    // the tick that stalled
  TEST_ASSERT_EQUAL_UINT8(0, p.due(t, true, true));
  TEST_ASSERT_EQUAL_UINT32(3, p.misses());
  TEST_ASSERT_EQUAL_UINT8(0, p.due(t + 899999, true, true));   // backing off
  TEST_ASSERT_EQUAL_UINT8(1, p.due(t + 900000, true, true));
  p.onRequested(1, t + 900000);
  p.onPageHeard(1, t + 901000);
  TEST_ASSERT_FALSE(p.stalled());
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());
}

void test_poller_losing_the_bus_drops_the_request_in_flight() {
  FurnaceMenuPoller p;
  p.due(0, true, true); p.onRequested(1, 0);
  TEST_ASSERT_EQUAL_UINT8(1, p.pending());
  TEST_ASSERT_EQUAL_UINT8(0, p.due(1000, false, true));
  TEST_ASSERT_EQUAL_UINT8(0, p.pending());
  TEST_ASSERT_EQUAL_UINT32(0, p.misses());      // not a furnace miss
}

void test_poller_ignores_overheard_pages() {
  FurnaceMenuPoller p;
  p.due(0, true, true); p.onRequested(1, 0);
  p.onPageHeard(2, 100);                       // someone else's page 2
  TEST_ASSERT_EQUAL_UINT8(1, p.pending());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_store_ingests_every_page_and_counts_generations);
  RUN_TEST(test_store_newer_page_replaces_older);
  RUN_TEST(test_store_rejects_non_menu_and_keeps_good_copy_over_torn_one);
  RUN_TEST(test_store_page_names_from_menu_list_or_defaults);
  RUN_TEST(test_store_records_source);
  RUN_TEST(test_render_seen_page_lists_rows_with_age);
  RUN_TEST(test_render_setup_shows_values_and_choices_read_only);
  RUN_TEST(test_render_never_seen_page_says_how_to_get_it);
  RUN_TEST(test_format_age_units);
  RUN_TEST(test_status_values_heat_call);
  RUN_TEST(test_status_values_idle_inducer_off_is_zero);
  RUN_TEST(test_status_values_cooling_status2);
  RUN_TEST(test_status_values_history_pages_carry_none);
  RUN_TEST(test_poller_idle_until_permitted);
  RUN_TEST(test_poller_cycle_status1_then_status2_then_waits_interval);
  RUN_TEST(test_poller_idle_cadence_is_five_minutes);
  RUN_TEST(test_poller_demand_starting_tightens_cadence);
  RUN_TEST(test_poller_stalls_after_misses_and_backs_off);
  RUN_TEST(test_poller_losing_the_bus_drops_the_request_in_flight);
  RUN_TEST(test_poller_ignores_overheard_pages);
  return UNITY_END();
}
