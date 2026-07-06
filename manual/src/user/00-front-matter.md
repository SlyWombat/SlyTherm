::: {.title-page}

![](theme/slytherm-mark.png)

# SlyTherm Dual-Fuel Smart Thermostat {.unlisted .unnumbered}

[User Manual]{.doc-title}

[Model ST-1 (Dettson dual-fuel edition)]{.model}

[SlyTherm]{.manufacturer}

[DRAFT v0.4 — PRE-RELEASE, NOT FOR FIELD USE]{.draft-banner}

| Revision | Date | Changes |
| --- | --- | --- |
| 0.1 | 2026-06-11 | Initial draft |
| 0.2 | 2026-06-11 | Custom presets, timed holds, sensor calibration, gas cycle timers, HA starter package |
| 0.3 | 2026-06-11 | Screen lock, smart recovery, accessory coordination, safety supervisor, bench firmware |
| 0.4 | 2026-07-06 | SlyTherm branding; screen-by-screen reference; on-device vacation hold; current Home/Sensors UI |

:::

> **DRAFT v0.4 — PRE-RELEASE, NOT FOR FIELD USE**
>
> This manual describes a product that is still in development. Some behaviour
> described here has not yet been verified on installed equipment. Sections
> that depend on pending verification are clearly marked. Do not rely on this
> document to operate a live heating system.

*Product and model names are provisional branding and may change before release.*

## About this manual

This manual is for the people who live with the thermostat: how to read the
screen, set temperatures, choose modes, use schedules and remote sensors, and
understand what the alerts mean. It assumes the thermostat has already been
installed and commissioned.

If you are installing, wiring, or configuring the thermostat for the first
time, use the separate **Installation & Commissioning Manual**. Nothing in
this user manual involves opening the wall unit, the furnace, or the heat
pump.

## Copyright and disclaimer

© 2026 SlyTherm. All rights reserved.

This thermostat is an experimental, open-source controller for a gas furnace
and heat pump. It is **not** a certified thermostat. The furnace and heat
pump keep all of their own built-in safety systems — the thermostat only
*requests* heating or cooling and the equipment decides whether it is safe to
run — but installing a non-certified control on a gas appliance can affect
your equipment warranty, may be regulated work in your area, and could affect
your home insurance. Discuss this with your installer.

SlyTherm provides this product and documentation "as is", without warranty
of any kind. Operating a gas appliance always carries risk: keep a working
carbon monoxide (CO) alarm in your home, and read the safety section of this
manual before using the system.
