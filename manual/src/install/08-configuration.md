# 8. Configuration Parameter Reference

All control and protection parameters are **firmware-resident**: they live on
the controller, survive network outages, and are exposed to Home Assistant as
editable `number` entities (Section 9). Home Assistant edits them but never
owns them. Values written outside the documented range are **clamped or
rejected** by the firmware. The defaults below are the boot / factory-reset
values.

Temperatures are °C; durations are seconds unless noted. The "Constant" column
gives the firmware identifier for cross-reference with engineering
documentation.

## 8.1 Compressor protection

These are the **primary** compressor protections — do not widen them on the
assumption that the outdoor unit protects itself (Section 7.1).

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Compressor minimum OFF time | `kCompressorMinOffS` | **300 s** | 240–900 s. The outdoor unit's internal ~3-min delay is a backstop only |
| Compressor minimum ON time | `kCompressorMinOnS` | **300 s** | 60–1200 s |
| Maximum compressor starts per hour | `kCompressorMaxStartsPerH` | **3** | — |
| Post-boot compressor hold-off | `kBootCompressorHoldoffS` | **300 s** (+ 0–60 s random jitter) | Timer state is persisted; if it is missing/invalid at boot, the full hold-off is enforced |
| Reset-loop lockout count | `kResetLoopLockoutCount` | **3 resets** | ≥ 3 watchdog/brownout resets… |
| Reset-loop window | `kResetLoopWindowS` | **1800 s** (30 min) | …within this window → latched NO-DEMAND, manual clear required |

## 8.2 Setpoints, deadband, changeover, presets, and holds

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Call hysteresis (heat/cool differential) | `kCallHysteresisC` | **0.55 °C** (≈ 1 °F) | A 0.5 °F differential is field-verified to short-cycle; 0.3 °C enter / 0.2 °C exit is an acceptable alternative |
| Auto-mode heat↔cool setpoint deadband | `kMinSetpointDeltaC` | **2.8 °C** (5 °F) | Minimum separation between heat and cool setpoints |
| Deadband hard floor | `kMinSetpointDeltaFloorC` | **1.1 °C** (2 °F) | Firmware rejects/clamps violating writes and pushes the other setpoint |
| Auto-changeover dwell | `kChangeoverDwellS` | **1800 s** (30 min) | Minimum time since the opposite call; compressor min-off must also be satisfied |
| Changeover trigger sustain | `kChangeoverSustainS` | **600 s** (10 min) | The changeover trigger must persist this long |
| Manual-change hold type | `kDefaultHoldType` | **until next preset** | A manual setpoint/mode change creates a hold of this type; scheduled preset writes are ignored while a hold is active. Types: until-next-preset / 2 h / 4 h / indefinite |
| Timed hold, short | `kHoldShortS` | **7200 s** (2 h) | Timed-hold expiry does **not** revert setpoints — the next scheduled preset write restores the schedule |
| Timed hold, long | `kHoldLongS` | **14400 s** (4 h) | — |
| Preset roster size | `kMaxPresets` | **8 entries** | Roster is config-driven from Home Assistant (retained config topic); invalid entries are skipped |
| Preset name length | `kPresetNameMaxLen` | **23 characters** | A preset pair violating the deadband resolves with the cool value winning (heat pushed down) |

## 8.3 Dual fuel (balance point, lockouts, escalation)

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Balance point (economic changeover) | `kBalancePointC` | **−8 °C** | Mirrors the OEM (R02P034 "P124") range −30…+15 °C |
| Balance-point hysteresis | `kBalancePointHystC` | **2 °C** | — |
| Compressor low-ambient lockout | `kCompressorMinOatC` | **−20 °C** | **Set from the installed model's submittal sheet** (FLEXX-class rated to −30 °C); range disabled…−30 °C |
| Gas/aux high-ambient lockout | `kAuxMaxOatC` | **+10 °C** | No gas/aux heat above this outdoor temperature |
| Escalation droop | `kEscalationDroopC` | **1.0 °C** | Room droop below setpoint… |
| Escalation time | `kEscalationMinS` | **1800 s** (30 min) | …sustained this long… |
| Escalation HP-demand threshold | `kEscalationHpDemandPct` | **95 %** | …while heat-pump demand is saturated → stage gas |
| De-escalation time | `kDeescalationMinS` | **3600 s** (60 min) | Stage back to heat pump after this, with outdoor temp above balance point + hysteresis |

**Hard validation rule:** the firmware rejects any lockout/balance-point
combination that leaves **any outdoor-temperature band with no permitted heat
source**. A configuration that can silently produce "no heat at −25 °C" is
itself treated as a fault.

## 8.4 Gas modulation

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Modulation floor (low fire) | `kGasFloorPct` | **40 %** | Chinook documented low fire. Valid demand is 0 or 40–100 % — never in between; sub-floor demand snaps to 0/low-fire with hysteresis |
| Gas maximum continuous runtime | `kGasMaxRuntimeS` | **14400 s** (4 h) | Without progress → drop + alarm. The heat pump has **no** runtime cap — alarm only, never auto-cycled |
| Gas minimum ON time | `kGasMinOnS` | **300 s** | 60–900 s. Gates **comfort** stops only — the burner is held at low fire until the timer is served. Safety stops (sensor fault, invariant trip, maximum runtime, watchdog) extinguish the burner immediately |
| Gas minimum OFF time | `kGasMinOffS` | **300 s** | 60–900 s. After a power cycle the OFF timer restarts fresh unless persisted state proves it was already served |

## 8.5 Defrost tempering

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Tempering heat demand | `kDefrostTemperHeatPct` | **35 %** fixed | Never PID-driven |
| Tempering time cap | `kDefrostTemperMaxS` | **900 s** (15 min) | Hard cap; fan % and heat % separately configurable |

> **⚠ Pending verification:** ownership of defrost tempering (whether the
> interface board handles it autonomously in Case A) is unconfirmed. The
> expected outcome is that the controller only detects defrost and holds
> steady; these parameters bound the alternative.

## 8.6 Heat-pump demand shaping

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Demand slew rate | `kHpSlewPctPerMin` | **10 %/min** | Applies only if a proportional demand path exists |
| Demand step size | `kHpStepPct` | **5 %** | — |
| Heat-pump demand floor | `kHpFloorPct` | **30 %** | **Verify against the installed model** |

> **⚠ Pending verification:** whether the installed system offers a
> proportional (modulating) heat-pump demand path at all — vs staged
> demand — is unconfirmed. In the staged/relay case these parameters are
> unused; the relay path duty-cycles within the Section 8.1 timers instead.

## 8.7 Indoor sensor fusion

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Per-sensor staleness timeout | `kSensorMaxAgeS` | **300 s** | 180–900 s; relies on the 60 s bridge heartbeat (below) |
| Bridge heartbeat interval | `kSensorHeartbeatS` | **60 s** | Home Assistant republish cadence (Section 9.4) |
| Valid range, low | `kSensorRangeMinC` | **5 °C** | Readings outside the gate are rejected |
| Valid range, high | `kSensorRangeMaxC` | **40 °C** | — |
| Outlier exclusion | `kSensorOutlierC` | **4 °C** | A sensor > 4 °C from the participant median is excluded + alarmed |
| Occupied sensor weight | `kOccupiedWeight` | **2.0** | vs 1.0 unoccupied ("follow-me" weighting) |
| Occupancy window | `kOccupancyWindowS` | **1800 s** (30 min) | — |
| Weight ramp time constant | `kWeightRampTauMinS` / `MaxS` | **600–1800 s** | Sensors phase in/out smoothly |
| Fusion smoothing time constant | `kFusionSmoothingTauMinS` / `MaxS` | **120–300 s** | — |
| Fusion slew limit | `kFusionSlewCPerMin` | **0.1 °C/min** | A sensor joining/leaving cannot step the control input |
| Fallback-sensor disagreement alarm | `kDs18b20DisagreeAlarmC` | **5 °C** | Local sensor vs fused aggregate sanity check |
| Per-sensor calibration offset | `kSensorOffsetMaxC` (clamp) | **0 °C** | Adjustable **±5 °C** per sensor (0.1 °C steps in Home Assistant), including the built-in fallback sensor. Applied **before** the health gates, so range/stuck/outlier checks judge corrected values; an offset change ramps through the fusion slew limit rather than stepping the control input |

## 8.8 Outdoor temperature

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Source staleness | `kOatStaleS` | **1800 s** (30 min) | → fall to the next source rung: bus sensor → wired outdoor sensor → Home Assistant weather |
| Cross-source disagreement alarm | `kOatRungDisagreeAlarmC` | **5 °C** | — |

All outdoor sources stale → **fail cold**: gas heat allowed, compressor
locked out (and cooling locked out under the indoor-temperature policy
below). The compressor is never run on an unknown outdoor temperature.

## 8.9 Fallback and degraded modes

| Parameter | Constant | Default | Range / notes |
| --- | --- | --- | --- |
| Network-stale fallback, heat-to | `kFallbackHeatSetpointC` | **18 °C** | Dual-bounded with the cool-to value; mode = last user mode; never escalates to OFF |
| Network-stale fallback, cool-to | `kFallbackCoolSetpointC` | **27 °C** | 27–28 °C acceptable |
| Network staleness threshold | `kMqttStaleS` | **1800 s** (30 min) | No MQTT command/heartbeat traffic for this long → fallback profile |
| Degraded-mode heat floor | `kDegradedHeatFloorC` / `kDegradedHeatCeilC` | **16–18 °C** | Local-fallback-sensor-only mode: bounded heat, **cooling disabled** (or ≥ 29 °C ceiling), demand capped, persistent alarm |
| Indoor cooling lockout | `kCoolingIndoorLockoutC` | **18 °C** | Never cool when indoor temperature is below this |

## 8.10 Installer notes

- After any configuration change to lockouts or balance point, confirm the
  controller accepted it (the echoed state in Home Assistant shows the
  post-clamp value) and that no validation alarm is raised.
- Set `kCompressorMinOatC` from the **installed outdoor unit's submittal
  sheet**, recorded during the pre-install survey — not from the default.
- Record the as-left value of every parameter you change in the
  commissioning record (Section 10).
