CREATE INDEX IF NOT EXISTS ix_state_savestate_derivation_source_context
    ON state_savestate_derivation(source_context_kind, source_context_id, derivation_id);
