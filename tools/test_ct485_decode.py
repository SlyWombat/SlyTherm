"""pytest suite for tools/ct485_decode.py.

Run: python3 -m pytest tools/ -q
"""
from pathlib import Path

import pytest

import ct485_decode as d

FIXTURES = Path(__file__).parent / "fixtures"

# Real frames captured from kpishere/Net485 diag/logs (annotations theirs):
#   "Heat request 60%"  — HEAT_DEMAND 0x64, demand byte 0x78 = 60% (variant A)
REAL_HEAT_60 = bytes.fromhex("ff010202020001032004640060788761")
#   matching "ACK reply"
REAL_HEAT_ACK = bytes.fromhex("01ff0202020002832004640060 78 79 ed".replace(" ", ""))
#   "Fan request 60%" — FAN_DEMAND 0x66 (timer, mode, pct*2)
REAL_FAN_60 = bytes.fromhex("ff0102020200010320056600600078d510")
#   GETSTATUS reply (36 bytes, 24-byte payload)
REAL_STATUS = bytes.fromhex(
    "02ff0201660002822018001600000000"
    "02360f00000000000052020000360f000000" + "63d2")


# ---------------------------------------------------------------------------
# Checksum vectors (mirror the C++ Fletcher tests: seed 0xAA, mod 0xFF)
# ---------------------------------------------------------------------------

@pytest.mark.parametrize("frame", [REAL_HEAT_60, REAL_HEAT_ACK, REAL_FAN_60, REAL_STATUS])
def test_fletcher_validates_real_captured_frames(frame):
    assert len(frame) == frame[9] + 12
    assert d.fletcher_ok(frame)


@pytest.mark.parametrize("frame", [REAL_HEAT_60, REAL_FAN_60, REAL_STATUS])
def test_fletcher_pair_reproduces_captured_checksums(frame):
    body, c1, c2 = frame[:-2], frame[-2], frame[-1]
    assert d.fletcher_pair(body) == (c1, c2)


def test_fletcher_rejects_any_single_byte_corruption():
    for i in range(len(REAL_HEAT_60)):
        bad = bytearray(REAL_HEAT_60)
        bad[i] ^= 0x01
        assert not d.fletcher_ok(bytes(bad)), f"corruption at byte {i} not caught"


def test_fletcher_mod_255_not_256():
    # All-0xFF body stresses the mod-255 wrap; check round-trip self-validates.
    header = bytes([0xFF] * 9 + [4])
    frame = d.build_frame(header, bytes([0xFF] * 4))
    assert d.fletcher_ok(frame)
    assert not d.fletcher_ok(frame[:-1] + bytes([frame[-1] ^ 0xFF]))


def test_build_frame_round_trip():
    payload = bytes([0x64, 0x00, 0x60, 0x78])
    header = bytes([0xFF, 0x01, 0x02, 0x02, 0x02, 0x00, 0x01, 0x03, 0x20, len(payload)])
    assert d.build_frame(header, payload) == REAL_HEAT_60


# ---------------------------------------------------------------------------
# Resync (mid-stream join: slide until a valid frame)
# ---------------------------------------------------------------------------

def test_extract_clean_back_to_back_frames():
    frames, garbage = d.extract_frames(REAL_HEAT_60 + REAL_FAN_60 + REAL_STATUS)
    assert [f for _, f in frames] == [REAL_HEAT_60, REAL_FAN_60, REAL_STATUS]
    assert garbage == 0


def test_resync_skips_partial_frame_from_midstream_join():
    # Join mid-frame: tail of one frame, then two complete frames.
    stream = REAL_STATUS[20:] + REAL_HEAT_60 + REAL_FAN_60
    frames, garbage = d.extract_frames(stream)
    assert [f for _, f in frames] == [REAL_HEAT_60, REAL_FAN_60]
    assert garbage == len(REAL_STATUS) - 20


def test_resync_skips_corrupt_frame_and_recovers():
    bad = bytearray(REAL_HEAT_60)
    bad[13] ^= 0xFF  # corrupt the demand byte -> Fletcher fails
    frames, garbage = d.extract_frames(bytes(bad) + REAL_FAN_60)
    assert [f for _, f in frames] == [REAL_FAN_60]
    assert garbage == len(REAL_HEAT_60)


def test_extract_ignores_trailing_garbage():
    frames, garbage = d.extract_frames(REAL_HEAT_60 + b"\x01\x02\x03")
    assert [f for _, f in frames] == [REAL_HEAT_60]
    assert garbage == 3


def test_extract_rejects_absurd_payload_length():
    # buf[9] > 240 must never be taken as a length, whatever follows.
    junk = bytes([0] * 9 + [0xF5]) + bytes(300)
    frames, _ = d.extract_frames(junk)
    assert frames == []


# ---------------------------------------------------------------------------
# Input formats
# ---------------------------------------------------------------------------

def test_parse_sniffer_line_millis_gap_hex():
    line = "123456 4200 " + " ".join(f"{b:02x}" for b in REAL_HEAT_60)
    ts, gap, data = d.parse_line(line)
    assert ts == 123456.0 and gap == 4200 and data == REAL_HEAT_60


def test_parse_net485_log_line_with_annotation():
    line = ("2017-01-04 21:07:25  (00,00)  "
            "ff 01 02 02 02 00 01 03 20 04 64 00 60 78 87 61 "
            "Heat request 60%, OnDuration=00:07")
    ts, gap, data = d.parse_line(line)
    assert data == REAL_HEAT_60
    assert ts is not None and gap is None  # ISO timestamp wins; (00,00) ignored


def test_parse_continuous_hex_blob_token():
    ts, gap, data = d.parse_line(REAL_HEAT_60.hex())
    assert data == REAL_HEAT_60


def test_parse_line_comment_and_prose_yield_nothing():
    assert d.parse_line("# 30 30 7d 07 comment")[2] == b""
    assert d.parse_line("nathan@maggie share $ more hvac.log")[2] == b""


def test_no_gap_split_reassembles_frame_across_lines(tmp_path):
    f = tmp_path / "blob.txt"
    h = REAL_HEAT_60.hex()
    f.write_text(h[:12] + "\n" + h[12:] + "\n" + REAL_FAN_60.hex() + "\n")
    split = d.read_capture(str(f), gap_split=True)
    joined = d.read_capture(str(f), gap_split=False)
    assert [fr.raw for fr in joined] == [REAL_HEAT_60, REAL_FAN_60]
    assert len(split) == 1  # line-split mode cannot see the spanning frame


# ---------------------------------------------------------------------------
# Set-Control demand decode — BOTH offset variants (docs/02 §5a ambiguity)
# ---------------------------------------------------------------------------

def _frame(raw: bytes) -> d.Frame:
    return d.Frame(raw=raw)


def test_demand_decode_reports_both_variants():
    out = "\n".join(d.decode_set_control(_frame(REAL_HEAT_60)))
    assert "AMBIGUOUS" in out
    assert "variant A" in out and "variant B" in out
    # variant A: timer 0x60, demand 0x78 = 60%
    assert "timer=0x60" in out and "0x78 (60.0%)" in out
    # 4-byte payload has no [14]: variant B demand must be flagged absent
    assert "ABSENT" in out


def test_demand_decode_variant_b_present_on_longer_payload():
    payload = bytes([0x64, 0x00, 0x60, 0x78, 0x50])
    header = bytes([0xFF, 0x01, 0x02, 0x02, 0x02, 0x00, 0x01, 0x03, 0x20, len(payload)])
    out = "\n".join(d.decode_set_control(_frame(d.build_frame(header, payload))))
    assert "demand=0x78 (60.0%)" in out      # variant A
    assert "demand=0x50 (40.0%)" in out      # variant B
    assert "ABSENT" not in out               # both layouts plausible -> ambiguous


def test_fan_demand_decode_both_variants():
    out = "\n".join(d.decode_set_control(_frame(REAL_FAN_60)))
    assert "variant A" in out and "pct=0x78 (60.0%)" in out
    assert "variant B" in out and "ABSENT" in out


def test_system_switch_decode():
    payload = bytes([0x05, 0x00, 0x00, 0x03])
    header = bytes([0xFF, 0x01, 0x02, 0x02, 0x05, 0x00, 0x01, 0x03, 0x20, len(payload)])
    out = "\n".join(d.decode_set_control(_frame(d.build_frame(header, payload))))
    assert "SYSTEM_SWITCH_MODIFY" in out and "HEAT" in out


def test_set_control_with_truncated_payload_does_not_crash():
    header = bytes([0xFF, 0x01, 0x02, 0x02, 0x64, 0x00, 0x01, 0x03, 0x20, 1])
    out = "\n".join(d.decode_set_control(_frame(d.build_frame(header, b"\x64"))))
    assert "too short" in out


# ---------------------------------------------------------------------------
# Diagnostics fault-string split
# ---------------------------------------------------------------------------

def test_diagnostics_fault_split():
    out = d.decode_diagnostics(b"PRESSURE SW\x00FLAME LOSS\x00")
    assert out == ["  fault[0]: PRESSURE SW", "  fault[1]: FLAME LOSS"]


def test_diagnostics_empty_and_nonprintable():
    assert "no faults" in d.decode_diagnostics(b"")[0]
    assert "no faults" in d.decode_diagnostics(b"\x00\x00")[0]
    out = d.decode_diagnostics(b"ERR\x01CODE\x00")
    assert "\\x01" in out[0]  # non-printable escaped, not passed through


# ---------------------------------------------------------------------------
# Sensor TLV walk — must be safe on truncated/garbage input
# ---------------------------------------------------------------------------

def test_tlv_walk_valid_records():
    payload = bytes([0, 2, 0x12, 0x34, 5, 1, 0xFF])
    recs, truncated = d.walk_sensor_tlv(payload)
    assert recs == [(0, b"\x12\x34"), (5, b"\xff")]
    assert not truncated


def test_tlv_walk_truncated_value():
    recs, truncated = d.walk_sensor_tlv(bytes([0, 10, 0x01, 0x02]))  # len 10, only 2
    assert recs == [] and truncated


def test_tlv_walk_truncated_header_and_empty():
    recs, truncated = d.walk_sensor_tlv(bytes([7]))  # id without length byte
    assert recs == [] and truncated
    recs, truncated = d.walk_sensor_tlv(b"")
    assert recs == [] and not truncated


def test_tlv_walk_huge_length_does_not_overread():
    payload = bytes([0, 0xFF]) + bytes(8)
    recs, truncated = d.walk_sensor_tlv(payload)
    assert recs == [] and truncated


# ---------------------------------------------------------------------------
# --diff mode on synthetic walk data (the modulation-byte hunt)
# ---------------------------------------------------------------------------

def _heat_walk_frames(pcts):
    frames = []
    for i, pct in enumerate(pcts):
        payload = bytes([0x64, 0x00, 0x60, pct * 2])
        header = bytes([0xFF, 0x01, 0x02, 0x02, 0x02, 0x00, 0x01, 0x03, 0x20,
                        len(payload)])
        frames.append(d.Frame(raw=d.build_frame(header, payload),
                              ts_ms=1000.0 * i, index=i))
    return frames


def test_diff_localizes_demand_byte_in_walk():
    frames = _heat_walk_frames([40, 40, 60, 80, 100, 0])
    groups = d.diff_groups(frames)
    assert len(groups) == 1
    (key, group), = groups.items()
    assert key[2] == 0x03 and key[3] == 0x64
    assert d.changed_offsets(group) == {3}  # payload offset 3 = buffer [13]


def test_diff_render_marks_changes_and_collapses_identicals():
    out = "\n".join(d.render_diff(d.diff_groups(_heat_walk_frames([40, 40, 60]))))
    assert "HEAT_DEMAND" in out
    assert "identical frame(s)" in out
    assert "[3] 50->78" in out
    assert "variable payload offsets: 3(buf 13)" in out


def test_diff_groups_split_by_target_node_type():
    f1 = _heat_walk_frames([60])[0]
    raw = bytearray(f1.raw)
    raw[d.OFF_PARAM_HI] = 0x05  # same command, routed to heat pump instead
    f2 = d.Frame(raw=d.build_frame(bytes(raw[:10]), bytes(raw[10:-2])), index=1)
    assert len(d.diff_groups([f1, f2])) == 2


def test_diff_handles_payload_length_change():
    a = _heat_walk_frames([60])[0]
    payload = bytes([0x64, 0x00, 0x60, 0x78, 0x01])
    header = bytes([0xFF, 0x01, 0x02, 0x02, 0x02, 0x00, 0x01, 0x03, 0x20,
                    len(payload)])
    b = d.Frame(raw=d.build_frame(header, payload), index=1)
    diffs = d.diff_payloads(a.payload, b.payload)
    assert diffs == [(4, None, 0x01)]


# ---------------------------------------------------------------------------
# --terminals merge
# ---------------------------------------------------------------------------

def test_terminals_merge_annotates_latest_state_at_or_before(tmp_path):
    csv_file = tmp_path / "t.terminals.csv"
    csv_file.write_text("timestamp_ms,Y,W\n1000,0,1\n2000,1,0\n")
    rows = d.load_terminals(str(csv_file))
    frames = _heat_walk_frames([40, 60, 80])
    frames[0].ts_ms, frames[1].ts_ms, frames[2].ts_ms = 500.0, 1500.0, 2000.0
    n = d.annotate_terminals(frames, rows)
    assert n == 2
    assert frames[0].terminals is None       # before first row
    assert frames[1].terminals == "Y=0 W=1"
    assert frames[2].terminals == "Y=1 W=0"  # at-or-before is inclusive


def test_terminals_seconds_unit_and_headerless(tmp_path):
    csv_file = tmp_path / "t.csv"
    csv_file.write_text("1.0,1,0,0\n")
    rows = d.load_terminals(str(csv_file), unit="s")
    assert rows == [(1000.0, "T1=1 T2=0 T3=0")]


# ---------------------------------------------------------------------------
# --field-dict emit/update
# ---------------------------------------------------------------------------

def test_field_dict_emit_and_meaning_preserved_on_update(tmp_path):
    out = tmp_path / "dict.md"
    frames = _heat_walk_frames([40, 60, 80])
    d.update_field_dict(str(out), frames)
    text = out.read_text()
    assert "| 0x03 SET_CONTROL_CMD | 0x64 HEAT_DEMAND | 3 |" in text

    # Human names the field, then the dictionary is regenerated from new data.
    assert "| 0x50, 0x78, 0xa0 |  |" in text
    text = text.replace("| 0x50, 0x78, 0xa0 |  |",
                        "| 0x50, 0x78, 0xa0 | demand pct*2 |")
    out.write_text(text)
    d.update_field_dict(str(out), _heat_walk_frames([40, 100]))
    updated = out.read_text()
    assert "demand pct*2" in updated
    assert "0xc8" in updated  # new observation merged


# ---------------------------------------------------------------------------
# Fixture integration (real Net485 captures + openHAB sample + synthetic)
# ---------------------------------------------------------------------------

def test_real_fixture_hvac_001_fully_decodes():
    path = FIXTURES / "hvac_001.log"
    if not path.exists():
        pytest.skip("fixture not fetched")
    stats = d.CaptureStats()
    frames = d.read_capture(str(path), stats=stats)
    assert len(frames) == 5
    assert stats.garbage_bytes == 0
    assert {f.msg_type for f in frames} == {0x03, 0x83}


def test_real_fixture_hvac_000_status_walk():
    path = FIXTURES / "hvac_000.log"
    if not path.exists():
        pytest.skip("fixture not fetched")
    frames = d.read_capture(str(path))
    status = [f for f in frames if f.msg_type == 0x82]
    assert len(status) >= 40
    groups = d.diff_groups(frames)
    offs = set().union(*(d.changed_offsets(g) for g in groups.values()))
    assert offs  # the demand/stage bytes do move in this capture


def test_openhab_sample_is_truncated_and_does_not_validate():
    path = FIXTURES / "openhab_sample.txt"
    stats = d.CaptureStats()
    frames = d.read_capture(str(path), stats=stats)
    assert frames == []           # dst/src=0x30 artifact + truncation: no valid frame
    assert stats.garbage_bytes == 22


def test_synthetic_sniffer_fixture_resyncs_and_diffs():
    path = FIXTURES / "synthetic_sniffer.txt"
    stats = d.CaptureStats()
    frames = d.read_capture(str(path), stats=stats)
    assert stats.garbage_bytes == 11  # the deliberate mid-frame join on line 1
    assert len(frames) == 16
    groups = d.diff_groups(frames)
    demand_groups = [g for k, g in groups.items() if k[3] == 0x64]
    assert demand_groups and d.changed_offsets(demand_groups[0]) == {3}


# ---------------------------------------------------------------------------
# CLI smoke
# ---------------------------------------------------------------------------

def test_cli_summary_runs_clean_on_fixture(capsys):
    path = FIXTURES / "synthetic_sniffer.txt"
    assert d.main(["--summary", str(path)]) == 0
    out = capsys.readouterr().out
    assert "frame census" in out and "SET_CONTROL_CMD" in out


def test_cli_decode_with_terminals(capsys):
    cap = FIXTURES / "synthetic_sniffer.txt"
    csv_file = FIXTURES / "synthetic_sniffer.terminals.csv"
    assert d.main(["--terminals", str(csv_file), str(cap)]) == 0
    out = capsys.readouterr().out
    assert "24V terminals: Y=0 O-B=0 W=1 G=1 D=0" in out


# ---------------------------------------------------------------------------
# Telnet-mirror ingest (wall-unit LISTEN stream via ct485cap.py, fw >= 0.5.2)
# ---------------------------------------------------------------------------

def _mirror_lines(frame: bytes, millis: int) -> list[str]:
    """Render one raw frame the way sniffFrame() mirrors it."""
    plen = frame[9]
    payload = frame[10:10 + plen]
    pv = " ".join(f"{b:02X}" for b in payload[:16])
    out = [f"[ct485] {millis} {frame[1]:02X}>{frame[0]:02X} t{frame[7]:02X} l{plen}"
           + (f" {pv}" if pv else "")]
    for idx, off in enumerate(range(16, plen, 24)):
        hx = " ".join(f"{b:02X}" for b in payload[off:off + 24])
        out.append(f"[ct485+] {millis} {idx} {hx}")
    return out


def test_telnet_summary_line_synthesizes_frame(tmp_path):
    log = tmp_path / "cap.log"
    log.write_text("2026-07-08T14:52:03 [ct485] 389052 01>FF t03 l4 65 00 60 3C\n")
    frames = d.read_capture(str(log))
    assert len(frames) == 1
    f = frames[0]
    assert (f.src, f.dst, f.msg_type) == (0x01, 0xFF, 0x03)
    assert f.synthesized and not f.truncated
    assert f.command_code() == 0x65
    assert d.fletcher_ok(f.raw)  # rides the normal validated-frame paths


def test_telnet_continuation_completes_long_payload(tmp_path):
    lines = _mirror_lines(REAL_STATUS, 700123)
    assert len(lines) == 2  # 24B payload -> preview + one continuation
    log = tmp_path / "cap.log"
    log.write_text("\n".join(lines) + "\n")
    frames = d.read_capture(str(log))
    assert len(frames) == 1
    assert frames[0].payload == REAL_STATUS[10:34]
    assert not frames[0].truncated


def test_telnet_rej_burst_recovers_merged_frames(tmp_path):
    blob = REAL_HEAT_60 + REAL_HEAT_ACK          # merged back-to-back burst
    lines = [f"[ct485-rej] 800500 {i} " + " ".join(f"{b:02X}" for b in blob[o:o + 24])
             for i, o in enumerate(range(0, len(blob), 24))]
    lines.append("[ct485-stats] 801000 ok=5 badLen=1 badCk=0 over=0")
    log = tmp_path / "cap.log"
    log.write_text("\n".join(lines) + "\n")
    stats = d.CaptureStats()
    frames = d.read_capture(str(log), stats=stats)
    assert [f.raw for f in frames] == [REAL_HEAT_60, REAL_HEAT_ACK]
    assert stats.garbage_bytes == 0
    assert not frames[0].synthesized  # rej bytes are the real wire bytes
