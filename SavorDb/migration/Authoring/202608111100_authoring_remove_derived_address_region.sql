BEGIN IMMEDIATE;

ALTER TABLE au_address_symbol RENAME TO au_address_symbol_legacy;

CREATE TABLE au_address_symbol (
    address_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    region TEXT NOT NULL,
    base_address INTEGER NOT NULL,
    width INTEGER NULL,
    value_type TEXT NULL,
    notes TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_address_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_address_symbol_id CHECK(stable_id LIKE 'user.addr.%'),
    CONSTRAINT ck_au_address_symbol_region CHECK(region IN ('MEM1','MEM2')),
    CONSTRAINT ck_au_address_symbol_width CHECK(width IS NULL OR width IN (1,2,4,8)),
    CONSTRAINT ck_au_address_symbol_type CHECK(value_type IS NULL OR value_type IN ('u8','u16','u32','f32','f64','string','gc_input_frame','battle_path'))
);

INSERT INTO au_address_symbol(
    address_symbol_id, runtime_symbol_pack_id, stable_id, name, region,
    base_address, width, value_type, notes, ordinal)
SELECT
    address_symbol_id, runtime_symbol_pack_id, stable_id, name, region,
    base_address, width, value_type, notes, ordinal
FROM au_address_symbol_legacy
WHERE region IN ('MEM1','MEM2');

DROP TABLE au_address_symbol_legacy;

CREATE INDEX ix_au_address_symbol_pack
    ON au_address_symbol(runtime_symbol_pack_id, ordinal);

COMMIT;
