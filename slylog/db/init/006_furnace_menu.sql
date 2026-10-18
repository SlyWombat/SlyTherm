-- Furnace user-menu pages (#221). While someone browses the OEM stat's
-- Advanced Installer Menu -> Communicating Devices -> Furnace, the stat sends
-- CT-485 Get User Menu (0x41) and the furnace answers 0xC1 with a text page:
-- Status 1 (gas valve %, flame, blower CFM, limits, pressure switches),
-- Status 2 (Mode, ...), and the history/unit-info pages. The collector decodes
-- each reply into one row per `label = value`, so a heat cycle can be lined
-- up with the furnace's own status without anyone photographing the stat.
--
-- Passive capture only sees pages while the menu is open on the OEM stat
-- (the stat re-requests the displayed page every ~7-10 s). Idempotent: applied
-- by the entrypoint on fresh volumes AND via psql -f on the live database.

CREATE TABLE IF NOT EXISTS furnace_menu (
    ts        timestamptz NOT NULL,   -- wall-clock receive time of the page
    page      smallint    NOT NULL,   -- page index from the reply (1 = Status 1)
    title     text        NOT NULL,   -- e.g. 'STATUS 1'
    seq       smallint    NOT NULL,   -- row position on the page, 0-based
    label     text        NOT NULL,   -- e.g. 'GAS VLV PRCNT'
    value     text        NOT NULL,   -- trimmed text: '100%', 'CLOSED', 'GOOD'
    value_num double precision,       -- numeric reading (100, 1682, 'DAYS 68' -> 68) else NULL
    options   text                    -- editable SETUP rows: other choices, '|'-joined
);
ALTER TABLE furnace_menu ADD COLUMN IF NOT EXISTS options text;
SELECT create_hypertable('furnace_menu', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS furnace_menu_dedupe
    ON furnace_menu (ts, page, seq);
CREATE INDEX IF NOT EXISTS furnace_menu_label_idx
    ON furnace_menu (label, ts DESC);

-- The page bytes as received, one row per reply, so anything the grammar
-- misses (a page not yet seen, a new value format) can be re-decoded later.
-- `complete` is false when the reply did not parse to its terminator.
CREATE TABLE IF NOT EXISTS furnace_menu_pages (
    ts       timestamptz NOT NULL,
    page     smallint    NOT NULL,
    title    text,
    rows_n   smallint    NOT NULL,
    payload  bytea       NOT NULL,
    complete boolean     NOT NULL
);
SELECT create_hypertable('furnace_menu_pages', 'ts', if_not_exists => TRUE);
CREATE UNIQUE INDEX IF NOT EXISTS furnace_menu_pages_dedupe
    ON furnace_menu_pages (ts, page);
