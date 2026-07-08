# Dual-fuel control in cold climates — research notes for SlyTherm's control programming

Literature survey (2026-07-08) on optimized operation of a modulating gas
furnace + air-source heat pump in Canadian-style weather, mapped onto
SlyTherm's control stack (`DualFuelArbiter`, `DemandShaper`/`PidShaper`,
`RecoveryEstimator`, `SensorFusion`, CT-485 blower control). Each
recommendation cites its source; nothing here is implemented yet — this doc
exists to steer the control-programming issues that follow.

## 1. Switch over on ECONOMICS, not a fixed thermal balance point

The strongest common finding: conventional dual-fuel controls with a fixed
switchover temperature are measurably sub-optimal. The ORNL/Purdue "Hybrid
Heat Pump Controls" paper builds exactly our architecture (HP + gas furnace,
smart-thermostat control) and shows cost/emissions-optimal switchover varies
with the electricity:gas price ratio and the HP's COP-vs-OAT curve
([OSTI 1885313](https://www.osti.gov/servlets/purl/1885313)). The Canadian
SDFSS study (smart dual-fuel switching, Ontario) demonstrates simultaneous
cost + GHG reduction from dynamic switching
([Journal of Sustainability](https://journalofsustainability.net/ojs/JoS/article/view/5)),
and NRCan's hybrid-heating field homes measured ~30% GHG reduction over a
full heating season
([NRCan Simply Science](https://natural-resources.canada.ca/stories/simply-science/future-home-heating-hybrid-home-heating-systems-offer-energy-savings-reduce-ghg-emissions)).

The arithmetic (["economic balance point"](https://agentcalc.com/dual-fuel-heat-pump-balance-point-planner)):
gas heat costs `gas$/therm ÷ (29.3 kWh × AFUE)` per kWh-thermal; the HP costs
`elec$/kWh ÷ COP(OAT)`. Break-even `COP* = elec$ × 29.3 × AFUE ÷ gas$`
(≈2.7–2.8 at $0.15/kWh, $1.50/therm, 95 AFUE). Run the HP whenever its
current-OAT COP exceeds COP*, subject to capacity and comfort floors.

**SlyTherm mapping:** `DualFuelArbiter`'s balance point becomes a computed
quantity: configurable electricity/gas prices (HA-supplied, eventually
time-of-use aware) + a per-unit COP(OAT) table (start from nameplate curve;
§5 below on learning it). Keep the existing hard floors (compressor lockout
temp, capacity balance point ~-10 °C per the Canadian two-stage-furnace
guidance in the [eSim ASHP sizing study](https://publications.ibpsa.org/proceedings/esim/2020/papers/esim2020_1136.pdf))
— economics only ever moves switchover WITHIN the thermally safe band.

## 2. Predict the crossing; recover on the CHEAP ramp (the user's ask)

The canonical mechanism is Honeywell's adaptive-recovery scheme
([US 5,622,310](https://patents.google.com/patent/US5622310A/en)): learn the
house's °/hour recovery rate per stage, start recovery early on a shallow
ramp sized so the *heat pump alone* makes the target on time, and hold a
SECOND, steeper "auxiliary ramp" underneath it — expensive heat (gas, here)
engages only if the measured temperature falls below that fallback line.
MPC papers generalize this: predictive controllers using occupancy +
weather cut heating cost up to ~30% with negligible comfort loss
([Neurothermostat](https://www.researchgate.net/publication/2252092_The_Neurothermostat_Predictive_Optimal_Control_of_Residential_Heating_Systems),
[intermittent-heating MPC](https://www.academia.edu/28169209/Optimal_temperature_control_of_intermittently_heated_buildings_using_Model_Predictive_Control_Part_I_Building_modeling),
[Québec grey-box field study](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC11534675/)).

**SlyTherm mapping:** `RecoveryEstimator` already learns recovery slopes —
extend it in two directions:
- **Scheduled recovery (setback → comfort):** start = target_time −
  ΔT/slope_HP(OAT), gas engages only on the fallback ramp. In deep cold the
  estimator itself will conclude HP-alone can't make it and blend gas in
  from the start — that's the correct behavior falling out of one rule.
- **Steady-state crossing prediction:** SensorFusion's fused temp already
  trends; project time-to-setpoint-crossing from the current slope + OAT.
  When a crossing is predicted within N minutes (not merely when the
  deadband is violated), begin the gentle pre-actions in §3/§4 instead of
  waiting to slam on at full demand.

## 3. Blower-first starts (low, early) — the specific idea is sound

Three separate literatures support starting the ECM blower on low ahead of
(and beyond) the burner/compressor:
- **Destratification / measurement truth:** low continuous circulation mixes
  ceiling-warm and floor-cold air, and long low-speed cycles measurably
  even out room-to-room temperature
  ([variable-speed vs single-stage analysis](https://hvacloadcalculate.com/blog/variable-speed-vs-single-stage-furnace/),
  [ECM blower guides](https://furnace.direct/blogs/news/ecm-blower-motor-what-it-is-worth-the-upgrade)).
  For SlyTherm this has a second payoff: a pre-mix minute gives SensorFusion
  a truer whole-space reading BEFORE the arbiter commits to a stage.
- **Cheap at low speed:** ECM power draw falls steeply with speed (cube-law
  fan affinity) — a low-speed pre-run costs single-digit watts
  ([ECM vs PSC](https://www.griffithenergyservices.com/articles/your-furnace-replacement-the-ecm-motor-vs-the-psc)).
- **Cold-blow avoidance:** ramped/delayed blower profiles at heat-pump start
  and staged tempering during defrost are established comfort practice
  ([US 5,332,028](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/5332028)).

**SlyTherm mapping:** when §2 predicts a crossing, issue a CT-485 fan-low
demand ~1–3 min before the heat call (predictive pre-circulation); keep the
blower on low briefly after burner-off to harvest exchanger residual heat
(verify how much of this the Dettson does internally before duplicating);
fold into the existing fan-circulate-duty work (issue #53) so pre-runs
count toward the circulate duty rather than adding runtime on top.

## 4. Prefer LONG, LOW modulation over short high-fire cycles

Modulating furnaces earn their AFUE at part load: long low-fire burns keep
the exchanger condensing, cut cycling losses, and reduce temperature
overshoot/droop
([modulating-furnace guide](https://www.budgetheating.com/understanding-modulating-furnaces-and-their-benefits-guid/),
[US 6,321,744 low-stage fuel-utilization patent](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6321744)).
Same logic applies to the HP: steady low-compressor operation beats cycling.

**SlyTherm mapping:** bias `DemandShaper`/`PidShaper` tuning so the steady-
state solution is continuous low modulation (demand shaped to the load, not
bang-bang around the deadband), with §2's prediction absorbing disturbances
before they force a high-fire correction. The gradual-recovery ramp in §2
is also what keeps recovery itself inside low/mid fire.

## 5. Learn the real COP curve from our own telemetry

Field studies (ACEEE cold-climate HP monitoring:
[Rising Up to the Challenge](https://www.aceee.org/sites/default/files/proceedings/ssb24/pdfs/Rising%20up%20to%20the%20Challenge%20-%20Cold%20Climate%20Heat%20Pumps%20in%20the%20Field.pdf),
NRCan's [ccASHP assessment](https://natural-resources.canada.ca/maps-tools-publications/publications/cold-climate-air-source-heat-pumps-assessing-cost-effectiveness-energy-savings-greenhouse-gas-emissions-reductions-canadian-homes))
consistently show installed HP performance deviates from nameplate — defrost
overhead alone adds 5–15% in humid subfreezing weather
([COP curve strategies](https://www.pickhvac.com/heat-pump-efficiency-temperature-cop-curves-smart-cold/)).
SlyTherm already logs OAT, runtime, stage, and fused-temp slope — a
degree-days-per-runtime-hour proxy per OAT bucket is enough to correct the
COP(OAT) table seasonally without any extra hardware, closing the loop on
§1's economic switchover.

## 6. Defrost tempering with gas (validate the policy we sketched)

During HP defrost the indoor coil goes cold; the standard fix is staged
supplemental heat during defrost, sized to just offset the cold blow
([US 5,332,028](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/5332028)).
With a modulating furnace we can do better than resistance strips: low-fire
gas during defrost windows. `DualFuelArbiter` already models defrost
(kEquipHpHeat+kEquipGas concurrently) — the research supports making
low-fire-during-defrost the default, not an option.

## 7. Grid/price-responsive pre-conditioning (future)

Model-based dual-fuel control that pre-heats with the HP ahead of price
peaks (and lets gas carry the peak) shows meaningful bill reductions in
co-simulation ([Benefits of Dual Fuel Heat Pump Grid-responsive Control](https://www.sciencedirect.com/science/article/abs/pii/S0378778825010175));
the Canadian Climate Institute's analysis frames the hybrid-economics
context for Canadian provinces
([Heat Pumps Pay Off](https://climateinstitute.ca/wp-content/uploads/2023/09/2024-02-14_Dunsky-CCI-Heat-Pumps-Pay-Off-Revised-Technical-Report_FINAL-5-cities.pdf)).
The mirror strategy holds in cooling season: MPC-based **optimal
pre-cooling** with smart thermostats shifts compressor runtime out of the
peak window with maintained comfort
([MPC optimal precooling field implementation](https://www.sciencedirect.com/science/article/abs/pii/S0378778823010204)).

**Local solar + battery changes the arithmetic.** With TOU rates AND home
storage (e.g. an EcoFlow Delta Pro Ultra class battery, solar-charged), the
"cheap window" is not just the utility's off-peak band — it is off-peak
PLUS whatever stored/solar energy is available before the peak starts.
Concrete summer example (owner-supplied use case): on a TOU plan whose peak
begins at 07:00, it can be advantageous to **drive the space temperature
DOWN before 07:00 — even while occupants are asleep** — running the
compressor on overnight rates and/or battery, so the house coasts through
the morning peak with the compressor idle. The #90 sleep state must treat
this as a deliberate scheduled economic action (it outranks the sleep
preset's comfort band within configured limits), not as a comfort response.
The winter mirror applies equally: during EXTREME low-cost windows
(ultra-low overnight TOU, negative/curtailment pricing, or battery full of
surplus solar), **pre-warm the house above the schedule's setpoint** —
banking heat in the building's thermal mass with the HP at high COP so the
following expensive hours coast on gas-free, compressor-free drift.
The controller should NOT model the battery itself: consume a simple
"cheap-energy window" signal (schedule or HA-computed from
battery-state-of-charge + solar forecast) and feed it to the same
price-shifted recovery ramps as §2.

Once §1's price inputs exist via HA, time-of-use pre-heating/pre-cooling is
a natural extension of §2's recovery machinery — same ramps, price-shifted
targets.

## Suggested implementation order

1. §4 shaping bias + §6 defrost tempering — pure tuning/policy on existing
   modules, no new inputs.
2. §2 crossing prediction + §3 blower-first pre-circulation — extends
   `RecoveryEstimator` + one new CT-485 fan-low pre-demand path.
3. §1 economic balance point — needs HA price config + COP table plumbing.
4. §5 COP learning, then §7 TOU preheating — telemetry first, optimization
   after a season of data.
