// matter_glue.h — Matter data-model glue for the Controller (epic #179 phase 2).
//
// Wraps the pinned platform's precompiled Arduino Matter library
// (MatterThermostat) and exposes exactly two seams to main_thermostat.cpp:
//
//   inbound  — ecosystem writes (mode / heat sp / cool sp) surface as a
//              Command handed to the registered CommandFn. The callback runs
//              on the CHIP/Matter task, so the consumer must be thread-safe;
//              main_thermostat routes it into gPending under gCmdMux — the
//              SAME no-hold write path HA commands use. Matter never touches
//              the control pipeline directly; the safety pipeline stays
//              authoritative (docs/04) exactly as for every other intent.
//   outbound — publishState() (control task, ~1 Hz) mirrors the fused temp,
//              user mode and setpoints into the Matter attributes,
//              change-compared so a quiet system writes nothing.
//
// Pairing surface for the UI: commissioned() / manualCode() / qrPayload()
// (the raw "MT:..." onboarding payload — what a phone app's QR scan expects).
//
// Ecosystem quirks handled here: Alexa/chip may send EMERGENCY_HEAT (mapped to
// heat), PRECOOLING/FAN_ONLY/DRY/SLEEP are rejected (callback returns false),
// and 0xffff sentinel setpoint writes (single-sided setCoolingHeatingSetpoints)
// are ignored.
#pragma once
#ifdef SLYTHERM_MATTER
#include <cstdint>

namespace matter_glue {

struct Command {          // one ecosystem write, already mapped to hm values
  bool hasMode = false;   uint8_t hmMode = 0;  // hm::Mode numeric value
  bool hasHeatSp = false; float heatC = 0;
  bool hasCoolSp = false; float coolC = 0;
};
using CommandFn = void (*)(const Command&);

// Start the Matter stack + thermostat endpoint. Call ONCE, after WiFi STA is
// connected (the chip stack binds to the netif). onCommand may be called from
// the Matter task any time after this returns.
void begin(CommandFn onCommand);

// Control-task outbound mirror (~1 Hz). hmMode is the hm::Mode numeric value;
// emHeat mirrors the separate emergency-heat switch (#163) as EMERGENCY_HEAT.
void publishState(float fusedC, bool fusedValid, uint8_t hmMode, bool emHeat,
                  float heatSpC, float coolSpC);

bool commissioned();          // false until paired (and before begin())
const char* manualCode();     // "3497-011-2332"-style digits ("" before begin())
const char* qrPayload();      // "MT:..." raw onboarding payload ("" before begin())

}  // namespace matter_glue
#endif  // SLYTHERM_MATTER
