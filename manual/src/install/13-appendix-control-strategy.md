# Appendix — Control-Strategy Research (Cold-Climate Dual Fuel)

This appendix records the technical literature reviewed while designing the
controller's dual-fuel heating strategy — both the approaches the product
adopts and the approaches that were considered and deliberately **not**
used, with the reasoning. It is reference material for installers and
technically minded owners; nothing in this appendix changes an installation
step. The working bibliography with implementation mapping is maintained in
the repository (`docs/13-dual-fuel-control-research.md`).

## Strategies the controller adopts (implemented or planned)

### Economic switchover between heat pump and gas

Field and simulation studies of hybrid (heat pump + gas furnace) systems in
cold climates consistently find that a **fixed** heat-pump/furnace
switchover temperature is sub-optimal; the cost- and emissions-optimal
changeover moves with the electricity-to-gas price ratio and the heat
pump's efficiency curve versus outdoor temperature
([ORNL/Purdue, Hybrid Heat Pump Controls](https://www.osti.gov/servlets/purl/1885313);
[Smart Dual Fuel Switching System study, Ontario](https://journalofsustainability.net/ojs/JoS/article/view/5)).
Natural Resources Canada's instrumented hybrid-heating homes measured
roughly a 30% seasonal greenhouse-gas reduction against furnace-only
operation ([NRCan](https://natural-resources.canada.ca/stories/simply-science/future-home-heating-hybrid-home-heating-systems-offer-energy-savings-reduce-ghg-emissions)).
The controller therefore computes an *economic balance point* — run the
heat pump whenever its efficiency at the current outdoor temperature beats
the break-even point implied by configured energy prices — bounded by hard
thermal floors (compressor lockout, capacity limits).

### Predictive recovery ("start early, start gentle")

Rather than waiting for the room temperature to cross the setpoint and then
demanding full heat, the controller predicts the crossing and begins a
shallow, heat-pump-first recovery ramp early, holding gas in reserve behind
a steeper fallback line — the adaptive-recovery scheme long established for
heat-pump thermostats ([US 5,622,310](https://patents.google.com/patent/US5622310A/en)).
Predictive control of this family shows double-digit heating-cost
reductions with negligible comfort penalty in residential studies
([Neurothermostat](https://www.researchgate.net/publication/2252092_The_Neurothermostat_Predictive_Optimal_Control_of_Residential_Heating_Systems);
[Québec experimental-house predictive heating study](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC11534675/)).

### Blower-first starts and low-speed pre-circulation

Ahead of a predicted heat call the controller may start the furnace's ECM
blower on low for one to three minutes. Low-speed circulation destratifies
the space (ceiling-warm/floor-cold mixing), which both improves comfort and
gives the controller's fused room-temperature reading a truer picture
*before* it commits to a heating stage
([variable-speed airflow analyses](https://hvacloadcalculate.com/blog/variable-speed-vs-single-stage-furnace/)).
ECM power draw falls steeply at low speed, so the pre-run cost is single-
digit watts. A short post-burn blower run to harvest residual heat from the
exchanger is likewise standard practice.

### Long, low-fire modulation

Modulating condensing furnaces earn their efficiency at part load: long,
low-fire burns keep the heat exchanger condensing and avoid cycling losses
and temperature overshoot
([modulating-furnace fuel-utilization patent US 6,321,744](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/6321744)).
The controller's demand shaping is biased toward continuous low modulation
rather than bang-bang cycling around the deadband.

### Gas tempering during heat-pump defrost

During a defrost cycle the indoor coil runs cold; unmitigated, this
produces the "cold blow" complaint typical of heat pumps, and defrost
overhead adds 5–15% to energy use in humid subfreezing weather
([defrost/supplemental-heat control, US 5,332,028](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/5332028)).
With a modulating furnace available, low-fire gas during defrost windows is
the default tempering policy.

### Seasonal self-calibration

Installed heat-pump performance deviates from nameplate in the field
([ACEEE cold-climate monitoring](https://www.aceee.org/sites/default/files/proceedings/ssb24/pdfs/Rising%20up%20to%20the%20Challenge%20-%20Cold%20Climate%20Heat%20Pumps%20in%20the%20Field.pdf);
[NRCan ccASHP assessment](https://natural-resources.canada.ca/maps-tools-publications/publications/cold-climate-air-source-heat-pumps-assessing-cost-effectiveness-energy-savings-greenhouse-gas-emissions-reductions-canadian-homes)).
The controller already records outdoor temperature, stage runtime, and room
temperature trends; these are used to correct the assumed efficiency curve
over a season, keeping the economic switchover honest without added
instrumentation.

## Strategies considered and NOT adopted

### Fixed switchover temperature as the primary policy

The industry-default single changeover setpoint (commonly around −10 °C
for a two-stage furnace in Canadian design conditions,
[eSim ASHP sizing study](https://publications.ibpsa.org/proceedings/esim/2020/papers/esim2020_1136.pdf))
was **rejected as the primary policy** for the reasons in the first section
— it is retained only as an installer-configurable safety floor and manual
override, never the optimizer.

### Electric resistance auxiliary heat

Most published heat-pump recovery/defrost strategies assume electric strip
auxiliaries. **Not applicable here**: this system's supplemental heat is
the modulating gas furnace, which is both the cheaper marginal heat source
and already in the airstream. Resistance strips would also add substantial
electrical panel load that this installation class cannot spare.

### Full model-predictive control (MPC) on the controller

Formal MPC — grey-box or neural building models optimized over a forecast
horizon ([intermittent-heating MPC](https://www.academia.edu/28169209/Optimal_temperature_control_of_intermittently_heated_buildings_using_Model_Predictive_Control_Part_I_Building_modeling))
— shows the largest savings in the literature but was **deliberately not
adopted on-device**: it requires reliable weather forecasts, a maintained
building model, and optimization compute that do not belong on a
safety-adjacent embedded controller. The adopted adaptive dual-ramp
recovery captures the bulk of the benefit with a model the controller can
learn and explain. MPC remains a candidate as a *supervisory* layer in Home
Assistant, issuing ordinary setpoint schedules that the controller executes
and re-validates.

### Continuous 24/7 low-speed circulation

Running the blower continuously at low speed maximizes destratification
but was **rejected as a default**: even efficient ECM blowers accumulate
meaningful energy over a season, and constant airflow increases filter
loading and perceived draft. The adopted policy is a scheduled circulation
duty plus the predictive pre-runs described above, which deliver mixing
when it matters.

### Time-of-use / grid-responsive pre-conditioning

Pre-heating with the heat pump ahead of electricity price peaks (letting
gas carry the peak) shows real savings in co-simulation
([grid-responsive dual-fuel control](https://www.sciencedirect.com/science/article/abs/pii/S0378778825010175);
Canadian context in [Heat Pumps Pay Off](https://climateinstitute.ca/wp-content/uploads/2023/09/2024-02-14_Dunsky-CCI-Heat-Pumps-Pay-Off-Revised-Technical-Report_FINAL-5-cities.pdf)),
and the mirrored summer strategy — model-predictive **pre-cooling** ahead
of the peak window — has been demonstrated with ordinary smart thermostats
([MPC optimal precooling](https://www.sciencedirect.com/science/article/abs/pii/S0378778823010204)).

Where the installation pairs time-of-use rates with **local solar and
battery storage** (for example an EcoFlow Delta Pro Ultra), the economics
strengthen further: the cheap-energy window is the utility off-peak band
*plus* stored solar. In summer, on a plan whose peak begins at 07:00, it
can pay to pre-cool the space **before 07:00 even while occupants are
asleep** — compressor runtime happens on overnight rates or battery, and
the home coasts through the morning peak. The same logic runs in reverse in
winter: during extreme low-cost windows — ultra-low overnight rates,
negative/curtailment pricing, or a battery holding surplus solar — the
controller may pre-warm the house **above** the scheduled setpoint,
banking heat in the building's thermal mass while the heat pump is at its
cheapest, so the expensive hours that follow are coasted through with
neither compressor nor burner running. The controller treats both cases as
scheduled economic actions bounded by configured comfort limits (they
deliberately override the night/sleep comfort band within those limits),
and consumes a simple "cheap-energy window" signal from Home Assistant
rather than modeling the battery itself.

**Deferred, not rejected**: it requires the price/window feeds the
controller does not yet consume. The recovery machinery is designed so
price-shifted targets can be added later without new control paths.

### Equipment resizing / oversizing guidance

Several studies optimize by re-sizing the heat pump itself. **Out of
scope** for the controller — sizing is a system-design decision made before
installation. Installers should consult the
[eSim Canadian sizing study](https://publications.ibpsa.org/proceedings/esim/2020/papers/esim2020_1136.pdf)
during equipment selection.
