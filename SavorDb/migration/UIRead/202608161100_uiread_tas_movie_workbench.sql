BEGIN IMMEDIATE;

CREATE TABLE ui_state_savestate_summary (
    savestate_id INTEGER PRIMARY KEY,
    artifact_id INTEGER NOT NULL,
    savestate_type TEXT NOT NULL,
    note TEXT NULL,
    is_complete INTEGER NOT NULL,
    playback_state TEXT NOT NULL,
    dtm_artifact_id INTEGER NULL,
    sha256 TEXT NOT NULL,
    size_bytes INTEGER NOT NULL,
    filename TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_root_summary (
    tas_movie_root_id INTEGER PRIMARY KEY,
    source_dtm_artifact_id INTEGER NOT NULL,
    dtm_artifact_id INTEGER NOT NULL,
    rtc_value INTEGER NOT NULL,
    itinerary_artifact_id INTEGER NOT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL,
    checkpoint_savestate_id INTEGER NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_tree_summary (
    tas_movie_tree_id INTEGER PRIMARY KEY,
    tas_movie_root_id INTEGER NOT NULL,
    parent_tas_movie_tree_id INTEGER NULL,
    dtm_artifact_id INTEGER NOT NULL,
    itinerary_artifact_id INTEGER NOT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL,
    checkpoint_savestate_id INTEGER NOT NULL,
    source_context_kind TEXT NOT NULL,
    source_context_id INTEGER NOT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_validation_request_summary (
    validation_request_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    step_kind TEXT NOT NULL,
    operation TEXT NOT NULL,
    source_kind TEXT NOT NULL,
    source_ref_id INTEGER NOT NULL,
    source_dtm_artifact_id INTEGER NOT NULL,
    source_dtm_sha256 TEXT NOT NULL,
    rtc_value INTEGER NULL,
    effective_dtm_sha256 TEXT NOT NULL,
    itinerary_artifact_id INTEGER NULL,
    itinerary_sha256 TEXT NULL,
    required_final_breakpoint_pc INTEGER NOT NULL,
    latest_validation_attempt_id INTEGER NULL,
    latest_outcome TEXT NULL,
    latest_failure_reason TEXT NULL,
    latest_actual_pc INTEGER NULL,
    latest_actual_input_count INTEGER NULL,
    produced_tas_movie_root_id INTEGER NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_validation_attempt_summary (
    validation_attempt_id INTEGER PRIMARY KEY,
    validation_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL,
    outcome TEXT NOT NULL,
    failure_reason TEXT NULL,
    expected_pc INTEGER NULL,
    expected_input_count INTEGER NULL,
    actual_pc INTEGER NOT NULL,
    actual_input_count INTEGER NOT NULL,
    last_known_good_savestate_id INTEGER NULL,
    produced_tas_movie_root_id INTEGER NULL,
    worker_id TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_sterilization_request_summary (
    sterilization_request_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    workflow_step_id INTEGER NOT NULL,
    source_savestate_id INTEGER NOT NULL,
    source_savestate_artifact_id INTEGER NOT NULL,
    source_savestate_sha256 TEXT NOT NULL,
    source_dtm_artifact_id INTEGER NOT NULL,
    source_dtm_sha256 TEXT NOT NULL,
    reused_savestate_id INTEGER NULL,
    latest_sterilization_attempt_id INTEGER NULL,
    latest_produced_savestate_id INTEGER NULL,
    latest_candidate_savestate_sha256 TEXT NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_tas_movie_sterilization_attempt_summary (
    sterilization_attempt_id INTEGER PRIMARY KEY,
    sterilization_request_id INTEGER NOT NULL,
    source_job_id INTEGER NOT NULL,
    candidate_savestate_sha256 TEXT NOT NULL,
    produced_savestate_id INTEGER NOT NULL,
    worker_id TEXT NOT NULL,
    recorded_at_utc INTEGER NOT NULL
);

CREATE TABLE ui_battle_context_summary (
    context_probe_id INTEGER PRIMARY KEY,
    wave_id INTEGER NULL,
    source_savestate_id INTEGER NOT NULL,
    exec_job_id INTEGER NULL,
    probe_status TEXT NOT NULL,
    context_version INTEGER NULL,
    context_artifact_id INTEGER NULL,
    entry_pc INTEGER NULL,
    recorded_at_utc INTEGER NULL,
    created_at_utc INTEGER NOT NULL
);

CREATE INDEX ix_ui_state_savestate_selector
    ON ui_state_savestate_summary(is_complete,playback_state,created_at_utc DESC,savestate_id DESC);
CREATE INDEX ix_ui_tas_movie_validation_request_latest
    ON ui_tas_movie_validation_request_summary(created_at_utc DESC,validation_request_id DESC);
CREATE INDEX ix_ui_tas_movie_validation_attempt_request
    ON ui_tas_movie_validation_attempt_summary(validation_request_id,validation_attempt_id DESC);
CREATE INDEX ix_ui_tas_movie_sterilization_attempt_request
    ON ui_tas_movie_sterilization_attempt_summary(sterilization_request_id,sterilization_attempt_id DESC);
CREATE INDEX ix_ui_battle_context_selector
    ON ui_battle_context_summary(probe_status,created_at_utc DESC,context_probe_id DESC);

COMMIT;
