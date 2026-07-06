# 11. Rollback to the OEM Thermostat

Rollback returns the system to its **certified configuration**. The wiring
must be designed (Section 3.2: labeled conductors, ferruled terminations,
accessible terminal blocks) so this swap **takes minutes**. Rehearse it once
at commissioning.

## 11.1 When to roll back

- Sustained loss-of-heat alarm in heating season that cannot be promptly
  diagnosed.
- Any commissioning or in-service finding that a demand channel does not
  fail safe.
- Equipment service visits — present the technician with the certified
  configuration.
- Owner request, sale of the home, or decommissioning.

## 11.2 The rollback device

| Installation | Rollback thermostat |
| --- | --- |
| Communicating (Case A) | **Dettson R02P034** (current successor to the discontinued R02P030; R02P032 also applies) |
| Conventional (Case B) | **Dettson R02P033** or other conventional thermostat per the original wiring map |

The rollback device is kept **on site** as a condition of installation
(Section 1.4).

## 11.3 Procedure

1. At the touchscreen or Home Assistant, set the system to **OFF**. Wait for
   `active_equipment` to read `idle`. If the controller is dead, skip this
   step — equipment will drop its calls on bus silence / open relays.
2. Kill 24 VAC to the control circuit (furnace service switch).
3. Remove the SlyTherm display from its bracket and disconnect the field wiring
   at the enclosure terminal blocks: R, C, 1, 2 — and, in Case B, Y1/(Y2)/
   O-B/G and the sense conductors.
4. **Leave the hardwired condensate float switch in the Y circuit** — it is
   equipment protection, not part of the controller.
5. Remove any termination/bias components **you** added at the wall plate
   (per the Section 5.5 record) so the bus returns to its pre-install state.
6. Mount the OEM thermostat base and land the conductors per the
   **photographed pre-install wiring map** (survey step 5): R, C, 1, 2 for a
   communicating thermostat; the conventional map for an R02P033.
7. Restore 24 VAC. Verify the OEM thermostat powers up and communicates
   (communicating models) or controls conventionally.
8. Run one heat call (and one cool call, in season) to completion of startup
   to confirm normal operation.
9. In Home Assistant, the SlyTherm entities will go `offline` (Last Will);
   disable any automations that command them.

## 11.4 Returning to service

Re-installation of the SlyTherm after a rollback repeats the relevant parts of
commissioning (Section 10): boot-silence check, the fault-injection rows for
any channel whose wiring was touched, and the interlock tests if Case-B
wiring was disturbed.
