#pragma once
// SensorRoster — pure roster-slot name resolution (#155).
//
// A sensor slot carries two names: its wire id ("living") in `name`, and its
// #85 friendly display label ("Living Room") in `disp`. Callers resolve slots
// by either one: the cmd/sensor/<id>/... MQTT handlers pass the id, while the
// Sensors-tab panel row passes the friendly display string. Matching only the
// id made the panel participation toggle a silent no-op for any friendly-named
// sensor (#155). Resolution matches EITHER field so both paths succeed.
//
// Pure C++17, no Arduino.h — unit-tested in test/test_sensor_roster.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace slyroster {

// npos-style sentinel; mirrors main_thermostat's findSensor "not found" value.
constexpr uint8_t kNotFound = 0xFF;

// A roster slot matches `query` when the query equals its wire id (`name`) OR
// its friendly label (`disp`). Bounded compare (maxLen); an empty `disp` never
// matches so id-only slots keep their old behavior.
inline bool slotMatches(const char* name, const char* disp,
                        const char* query, size_t maxLen) {
  if (std::strncmp(name, query, maxLen) == 0) return true;
  if (disp[0] != '\0' && std::strncmp(disp, query, maxLen) == 0) return true;
  return false;
}

// Find the first used slot in [begin,end) whose id or friendly name matches
// `query`; returns kNotFound if none. SlotT must expose `.used`, `.name`,
// `.disp`. This is the exact resolution the firmware's findSensor runs (minus
// the local id==0 special case, which the caller handles first).
template <typename SlotT>
inline uint8_t findSlot(const SlotT* table, size_t begin, size_t end,
                        const char* query, size_t maxLen) {
  for (size_t i = begin; i < end; ++i)
    if (table[i].used &&
        slotMatches(table[i].name, table[i].disp, query, maxLen))
      return static_cast<uint8_t>(i);
  return kNotFound;
}

}  // namespace slyroster
