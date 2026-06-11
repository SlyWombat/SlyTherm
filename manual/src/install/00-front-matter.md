# ElectricRV Dual-Fuel Smart Thermostat — Technical Installation Manual

> **DRAFT v0.3 — PRE-RELEASE, NOT FOR FIELD USE**
>
> This manual describes a design that has not yet completed bench validation or
> field commissioning. Several behaviors of the installed equipment are still
> pending verification and are explicitly marked as such throughout. Do not
> install this product from this draft.

| | |
| --- | --- |
| **Product** | ElectricRV Dual-Fuel Smart Thermostat |
| **Model** | DT-1 (Dettson/Gree edition) |
| **Manufacturer** | ElectricRV |
| **Document** | Technical Installation and Integration Manual |
| **Revision** | 0.3 (see Revision History, final section) |
| **Date** | 2026-06-11 |

*Product, model, and manufacturer names are provisional branding and may
change before release.*

## About this manual

This manual is written for the **installing technician and systems
integrator**. It covers physical installation, low-voltage wiring, the RS-485
bus connection, the relay output stage (where applicable), configuration,
Home Assistant integration, commissioning, and rollback.

A separate **User Manual** covers day-to-day operation of the thermostat.

The DT-1 is designed for one specific equipment family:

- **Furnace:** Dettson Chinook modulating gas furnace (ClimateTalk / CT-485
  communicating control, 40–100 % modulation).
- **Heat pump:** Gree-built outdoor unit sold with the Dettson system —
  either a communicating Dettson Alizé configuration (with the Dettson
  K03085 interface board) or a conventional 24 V Gree FLEXX configuration.

Do not install the DT-1 on any other equipment.

## Who may install this product

Installation involves the low-voltage control system of a **gas-fired
appliance** and a **heat-pump compressor circuit**. In many jurisdictions,
work on gas-appliance controls is restricted to licensed technicians. Read
Section 1 (Critical Safety Information) in full before opening any equipment.
At minimum, a **licensed HVAC technician must review the completed
installation** before the system is left in service.

## Conventions used in this manual

| Marker | Meaning |
| --- | --- |
| **WARNING** | Risk of personal injury, fire, carbon monoxide exposure, or significant property/equipment damage. |
| **CAUTION** | Risk of equipment damage or malfunction. |
| **> ⚠ Pending verification:** | A behavior of the installed equipment that the design depends on but that has **not yet been confirmed** on real hardware. Treat as unknown until the referenced commissioning step proves it. |
| **> ⚠ TBD (Phase 0):** | Content that depends on the result of the **pre-install equipment survey** (called "Phase 0" in engineering documents; see Section 2.4). The survey decides which installation case applies to your site. |

Procedures are numbered and must be performed in order. Specifications and
parameters are given in tables; temperatures are in °C unless noted.

## Document set

| Document | Audience |
| --- | --- |
| User Manual | Homeowner / daily operator |
| **Technical Installation Manual (this document)** | Installer / integrator |
