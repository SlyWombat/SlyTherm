# Bus captures

Raw RS-485/CT-485 hex dumps from Phase 1 sniffing go here. Large raw files are
gitignored (see .gitignore); commit only small curated/annotated captures used as
parser test fixtures (e.g. `*.annotated.txt`).

## Capture procedure (sniffer web console)

The sniffer firmware serves a console at `http://<sniffer-ip>/` (serial prints
the URL on boot; with no `src/sniffer_secrets.h` it starts SoftAP
`dettson-sniffer`).

1. Wait for the auto-baud lock in the status panel (or force the baud).
2. **clear counters** at the start of the campaign.
3. Apply the stimulus (heat call, mode change, forced defrost, …) and
   **add an annotation** at each step — e.g. `note heat call raised to 60%`,
   `note W energized`. Annotations are stored in-line with the frames and come
   out as `#` comment lines that `tools/ct485_decode.py` skips.
4. **download capture** and save it under the naming convention below. The
   download DRAINS the on-device buffer (~16 KiB ring, oldest frames evicted
   when full — download before long campaigns overflow it).
5. Decode/analyze: `tools/ct485_decode.py [--summary|--diff] <file>`.

Timestamps in the file are sniffer `millis()` — note the wall-clock offset in
an annotation if a paired `*.terminals.csv` uses another clock.

## Naming convention (capture campaigns)

Name files `<campaign>-<date>-<nn>.<ext>`, where `<campaign>` is one of:

- `cooling-call` — cooling demand active
- `hp-heat-call` — heat pump heating demand active
- `defrost-forced` — forced/observed defrost cycle
- `mode-change` — system mode transitions (heat/cool/auto/off)
- `changeover` — dual-fuel changeover events (HP <-> gas)
- `node-discovery` — bus enumeration / node addressing traffic

## Paired 24 V terminal-state logs

Where possible, record a paired `*.terminals.csv` alongside each bus capture,
logging timestamped Y/O-B/W/G/D terminal states so bus frames can be correlated
with the 24 V signals. Use a shared clock (or note the offset) between the two logs.

## Season dependency

`cooling-call` captures need cooling season (or a forced cooling call at safe
outdoor temps); `defrost-forced` needs cold/frosting conditions or a manually
forced defrost. Plan campaigns around the calendar — some captures cannot be
taken on demand.
