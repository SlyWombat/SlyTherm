# Bus captures

Raw RS-485/CT-485 hex dumps from Phase 1 sniffing go here. Large raw files are
gitignored (see .gitignore); commit only small curated/annotated captures used as
parser test fixtures (e.g. `*.annotated.txt`).

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
