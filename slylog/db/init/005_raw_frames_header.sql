-- raw_frames: the five CT-485 header bytes the RX log has carried since
-- SlyTherm #204 (firmware v1.4.6, 2026-09-04 12:44:13 EDT). Fully idempotent:
-- applied by the image entrypoint on fresh volumes AND manually against the
-- live database (psql -f), so everything is IF NOT EXISTS.
--
-- NULL means "never logged", NOT zero. Every RX frame captured before the
-- cutover lacks these bytes, and they cannot be backfilled: they were never
-- captured. Post-cutover rows written before this migration are filled from
-- the capture log by `python -m slylog_collector.backfill_header`.
--
-- What each byte is good for (docs/02 §2, SlyTherm #204/#205/#209):
--   packet_num     bit 7 (0x80) = dataflow flag. Set on every 17-byte
--                  ACK/session frame, clear on every real command: the only
--                  reliable way to tell the two apart.
--   src_node_type  identifies the originator (0x01 thermostat, 0x02 furnace,
--                  0xA5 coordinator). The src ADDRESS does not: 0xFF is shared.
--   send_method    0x01 by priority (demands), 0x02 by node type (HUM_DEMAND,
--                  SUBSYSTEM_BUSY).
--   send_param_hi  repeats the command code under send_method 0x01; the TARGET
--                  node type under 0x02. The command itself is payload[0].
--   subnet         0x02 V1 / 0x03 V2.
ALTER TABLE raw_frames ADD COLUMN IF NOT EXISTS subnet        smallint;
ALTER TABLE raw_frames ADD COLUMN IF NOT EXISTS send_method   smallint;
ALTER TABLE raw_frames ADD COLUMN IF NOT EXISTS send_param_hi smallint;
ALTER TABLE raw_frames ADD COLUMN IF NOT EXISTS src_node_type smallint;
ALTER TABLE raw_frames ADD COLUMN IF NOT EXISTS packet_num    smallint;

-- Real commands only: the common query ("what did the OEM actually send?").
-- Partial, so it costs nothing for the ~94% of t03 that are dataflow frames or
-- the pre-cutover rows with no header.
CREATE INDEX IF NOT EXISTS raw_frames_commands_idx
    ON raw_frames (msg_type, ts DESC)
    WHERE packet_num IS NOT NULL AND (packet_num & 128) = 0;
