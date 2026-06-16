BEGIN IMMEDIATE;

PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS au_runtime_symbol_pack (
    runtime_symbol_pack_id INTEGER PRIMARY KEY,
    pack_id TEXT NOT NULL,
    schema_name TEXT NOT NULL,
    schema_version INTEGER NOT NULL,
    name TEXT NOT NULL,
    description TEXT NULL,
    content_hash TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    imported_at_utc INTEGER NOT NULL,
    CONSTRAINT uq_au_runtime_symbol_pack UNIQUE(pack_id, schema_version),
    CONSTRAINT ck_au_runtime_symbol_pack_schema CHECK(schema_name = 'savor.runtime-symbol-pack'),
    CONSTRAINT ck_au_runtime_symbol_pack_version CHECK(schema_version = 1)
);

CREATE TABLE IF NOT EXISTS au_context_symbol (
    context_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    value_type TEXT NOT NULL,
    description TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_context_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_context_symbol_id CHECK(stable_id LIKE 'user.ctx.%'),
    CONSTRAINT ck_au_context_symbol_type CHECK(value_type IN ('u8','u16','u32','f32','f64','string','gc_input_frame','battle_path'))
);

CREATE TABLE IF NOT EXISTS au_address_symbol (
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
    CONSTRAINT ck_au_address_symbol_region CHECK(region IN ('MEM1','MEM2','DERIVED')),
    CONSTRAINT ck_au_address_symbol_width CHECK(width IS NULL OR width IN (1,2,4,8)),
    CONSTRAINT ck_au_address_symbol_type CHECK(value_type IS NULL OR value_type IN ('u8','u16','u32','f32','f64','string','gc_input_frame','battle_path'))
);

CREATE TABLE IF NOT EXISTS au_breakpoint_symbol (
    breakpoint_symbol_id INTEGER PRIMARY KEY,
    runtime_symbol_pack_id INTEGER NOT NULL,
    stable_id TEXT NOT NULL,
    name TEXT NOT NULL,
    address_id TEXT NOT NULL,
    kind TEXT NOT NULL,
    enabled INTEGER NOT NULL CHECK(enabled IN (0, 1)),
    domain TEXT NULL,
    notes TEXT NULL,
    ordinal INTEGER NOT NULL,
    FOREIGN KEY(runtime_symbol_pack_id) REFERENCES au_runtime_symbol_pack(runtime_symbol_pack_id) ON DELETE CASCADE,
    CONSTRAINT uq_au_breakpoint_symbol_pack_id UNIQUE(runtime_symbol_pack_id, stable_id),
    CONSTRAINT ck_au_breakpoint_symbol_id CHECK(stable_id LIKE 'user.bp.%'),
    CONSTRAINT ck_au_breakpoint_symbol_kind CHECK(kind = 'execute')
);

CREATE INDEX IF NOT EXISTS ix_au_context_symbol_pack
    ON au_context_symbol(runtime_symbol_pack_id, ordinal);

CREATE INDEX IF NOT EXISTS ix_au_address_symbol_pack
    ON au_address_symbol(runtime_symbol_pack_id, ordinal);

CREATE INDEX IF NOT EXISTS ix_au_breakpoint_symbol_pack
    ON au_breakpoint_symbol(runtime_symbol_pack_id, ordinal);

COMMIT;
