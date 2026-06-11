# tools/ — PC-side CT-485 analysis instruments (Phase 2)

## ct485_decode.py

Decodes raw hex captures of the ClimateTalk / CT-485 bus into validated frames
and runs the Phase 2 analyses from
[`docs/02-protocol-climatetalk.md`](../docs/02-protocol-climatetalk.md) §8 and
[`docs/05-firmware-plan.md`](../docs/05-firmware-plan.md) (Phase 2). Python
3.12, **stdlib only** — no install needed. Protocol constants mirror
`lib/Ct485Core/Ct485Core.h` (the single source of truth; keep them in sync).

```sh
python3 tools/ct485_decode.py capture.log                 # full frame-by-frame decode
python3 tools/ct485_decode.py --summary capture.log       # frame census by src/dst/msgType
python3 tools/ct485_decode.py --diff capture.log          # 0x03/0x82 byte-grid diffs
python3 tools/ct485_decode.py --terminals cap.terminals.csv cap.log
python3 tools/ct485_decode.py --field-dict field-dict.md capture.log
python3 tools/ct485_decode.py --no-gap-split blob.txt     # continuous hex blob
```

### Input formats (permissive)

- **Sniffer serial lines** — `millis gap_us HEX HEX ...` (the Phase 1 rig's
  output). The decimal tokens immediately before the hex run become the frame
  timestamp (ms) and the measured inter-frame gap (µs).
- **Annotated logs** (e.g. `kpishere/Net485 diag/logs`) —
  `2017-01-04 21:07:25 (00,00) ff 01 ... Heat request 60%`. The extractor takes
  the **longest run of whitespace-separated 2-hex-digit tokens** per line, so
  dates, `(00,00)` markers, and trailing prose are ignored; an ISO timestamp
  anywhere on the line is used as the frame time.
- **Continuous hex blobs** — a long unspaced hex token per line; with
  `--no-gap-split` all bytes from all lines are concatenated and frames may span
  line breaks. (Caveat: in blob mode a *digit-only* token of ≥12 even digits is
  treated as hex — keep decimal metadata out of blob inputs.)
- Lines starting with `#` are comments (used by the annotated fixtures).

### Frame validation & resync

A frame is accepted only if `total == buf[9] + 12` fits and the **Fletcher-16
(seed 0xAA, mod 0xFF)** check passes (ported from docs/02 §3 reference C++).
On a mid-stream join or corruption the extractor **slides one byte at a time
until a frame validates**; skipped bytes are reported as garbage counts, never
silently dropped.

### Decode

Header fields are labelled from the docs/02 tables (msg types, node types,
addresses, send methods, packet-number bits). Per message type:

- **`0x03`/`0x83` Set Control** — command code from the 16-bit LE payload echo;
  demand bytes decoded under **BOTH provisional offset variants**
  (variant A `[12]`=timer/`[13]`=demand, variant B `[13]`/`[14]` — the docs/02
  §5a single-source ambiguity). When the payload is too short for a variant the
  output says so explicitly ("evidence AGAINST this variant"). **The tool never
  picks a winner — that is the §5a sniff-confirmation gate.** (For what it's
  worth: every `0x03` frame in the Net485 fixtures is only consistent with
  variant A — but that's someone else's install, not the Dettson.)
- **`0x86` Get Diagnostics resp** — null-separated fault-string split,
  non-printables escaped.
- **`0x87` Get Sensor Data resp** — TLV walk `(db_id, db_len, data)`, safe on
  truncated/garbage lengths, flags MDI id 0 (expected OAT slot — confirm).
- **`0x82`/`0x9D`** — payload dumped as an offset-labelled byte grid.

### Analysis modes

- `--summary` — frame census by (src, dst, msgType) plus a src-address →
  advertised-node-type table (the "who is on this bus" question: furnace,
  coordinator, and whether a HP/interface-board node exists at all).
- `--diff` — groups `0x03` (by src/dst/command/target node type) and `0x82`
  (by src/dst) frames; prints the baseline payload as a byte grid with every
  ever-changing offset marked, then each change event
  (`[off] old->new` with timestamp, identical frames collapsed). This is the
  **stimulus-response modulation-byte hunt** (docs/02 §8 step 5): walk the OEM
  stat low→high fire and read the variable offsets off the output.
- `--terminals file.terminals.csv` — merges a 24 V terminal-state CSV
  (the `captures/README.md` convention: first column timestamp — ISO, epoch
  seconds (`--terminals-unit s`), or the sniffer's millis clock — then columns
  like `Y,O-B,W,G,D`). Each frame is annotated with the most recent terminal
  state at-or-before its timestamp, in both decode and diff output — the
  bus-vs-24V correlation log (Phase 2 done-gate (f)).
- `--field-dict out.md` — emits/updates the field-dictionary markdown table
  (Phase 2 deliverable): one presence row per (msgType, command) seen, plus one
  row per payload offset that varies within a diff group, with observed values.
  The `meaning` column is **human-edited and preserved across updates**;
  everything else is regenerated from the captures.

## fixtures/

| File | Provenance | Validates? |
|---|---|---|
| `hvac_000.log` | fetched from [`kpishere/Net485`](https://github.com/kpishere/Net485) `diag/logs/` (real captures, someone else's CT-485 system) | **41/42 lines** → 41 Fletcher-valid frames, 0 garbage bytes (line 1 is a shell-prompt artifact, correctly ignored) |
| `hvac_001.log` | same | **5/5 frames valid**, incl. the annotated "Heat request 60%" frame — demand byte `0x78`=60 % at buffer offset [13] (variant A) |
| `hvac_002.log` | same | **87 frames valid**, 32 garbage bytes = four 8-byte `SessionID`/`MacID` metadata dumps that are not frames (correctly skipped) |
| `openhab_sample.txt` | the docs/02 §8 parser sanity frame from the openHAB ClimateTalk thread | **Does NOT validate, by design.** The leading `dst/src = 0x30` bytes are an ASCII-'0' adapter artifact, and the frame is truncated in the source (buf[9]=0x7c implies a 136-byte frame; only 22 bytes were quoted). Kept as a garbage/resync-handling fixture; the tool must (and does) report 0 valid frames / 22 garbage bytes on it. |
| `synthetic_sniffer.txt` | **synthetic** — hand-built with `ct485_decode.build_frame()` (real Fletcher checksums) in the sniffer `millis gap_us HEX` format | 16/16 frames valid after resync; line 1 deliberately starts mid-frame (11 garbage bytes) to exercise the slider. Encodes a HEAT_DEMAND walk 40→100→0 % with matching 0x82 status responses whose "modulation byte" tracks it — the `--diff` demo/test input |
| `synthetic_sniffer.terminals.csv` | synthetic, paired with the above | terminal-state merge demo (`W/G` go active during the walk) |

The big `BasementHVAC.log` (714 kB) in the same upstream directory was left
unfetched to keep the repo small — point the tool at a local copy if needed.

**Honest caveats:** the real fixtures are from a *different* (Net485 author's)
furnace/HP install — they prove the framing, checksum, and decode plumbing, not
Dettson-specific payload meanings. The demand-offset evidence in them
(variant A) does **not** discharge the docs/02 §5a TX gate for this furnace.
The synthetic fixture's 0x82 "modulation byte at payload offset 7" is invented
for testing `--diff`; the real Get-Status layout must be mapped empirically.

## Tests

```sh
python3 -m pytest tools/ -q
```

Covers: Fletcher vectors against real captured frames + corruption sweep
(mirrors the C++ checksum tests), resync from mid-stream joins, both demand
offset variants, fault-string split, TLV truncation safety, `--diff` on
synthetic walk data, terminals merge, field-dict meaning preservation, and CLI
smoke runs on the fixtures. (pytest is the only non-stdlib dependency, tests
only: `pip install --user --break-system-packages pytest`.)
