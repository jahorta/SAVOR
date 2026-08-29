CREATE TABLE exec_workflow_expansion(
    workflow_expansion_id INTEGER PRIMARY KEY,
    expansion_kind TEXT NOT NULL CHECK(expansion_kind IN ('TAS_FIRST_BATTLE','TAS_DELAY')),
    state TEXT NOT NULL CHECK(state IN ('RUNNING','ATTENTION','COMPLETED','CANCELED')),
    source_ref_kind TEXT NOT NULL,
    source_ref_id INTEGER NOT NULL CHECK(source_ref_id>0),
    inherited_rtc INTEGER NULL CHECK(inherited_rtc IS NULL OR inherited_rtc>=0),
    rtc_min INTEGER NULL CHECK(rtc_min IS NULL OR rtc_min>=0),
    rtc_max INTEGER NULL CHECK(rtc_max IS NULL OR rtc_max>=0),
    max_neutral_epochs INTEGER NOT NULL CHECK(max_neutral_epochs>=0),
    failure_text TEXT NULL,
    created_by TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL
);

CREATE TABLE exec_workflow_expansion_member(
    workflow_expansion_member_id INTEGER PRIMARY KEY,
    workflow_expansion_id INTEGER NOT NULL REFERENCES exec_workflow_expansion(workflow_expansion_id) ON DELETE CASCADE,
    member_role TEXT NOT NULL CHECK(member_role IN ('DELAY_PRODUCTION','RTC_BATTLE')),
    neutral_epoch_count INTEGER NOT NULL CHECK(neutral_epoch_count>=0),
    rtc_value INTEGER NULL CHECK(rtc_value IS NULL OR rtc_value>=0),
    workflow_instance_id INTEGER NOT NULL REFERENCES exec_workflow_instance(workflow_instance_id) ON DELETE CASCADE,
    state TEXT NOT NULL,
    created_at_utc INTEGER NOT NULL,
    updated_at_utc INTEGER NOT NULL,
    UNIQUE(workflow_expansion_id,member_role,neutral_epoch_count,rtc_value),
    UNIQUE(workflow_instance_id)
);

CREATE TABLE exec_workflow_expansion_target(
    workflow_expansion_target_id INTEGER PRIMARY KEY,
    workflow_expansion_id INTEGER NOT NULL REFERENCES exec_workflow_expansion(workflow_expansion_id) ON DELETE CASCADE,
    neutral_epoch_count INTEGER NOT NULL CHECK(neutral_epoch_count>=0),
    rtc_value INTEGER NOT NULL CHECK(rtc_value>=0),
    created_at_utc INTEGER NOT NULL,
    UNIQUE(workflow_expansion_id,neutral_epoch_count,rtc_value)
);

CREATE INDEX ix_exec_workflow_expansion_state
ON exec_workflow_expansion(state,workflow_expansion_id);
CREATE INDEX ix_exec_workflow_expansion_member_parent
ON exec_workflow_expansion_member(workflow_expansion_id,neutral_epoch_count,rtc_value);
CREATE INDEX ix_exec_workflow_expansion_target_parent
ON exec_workflow_expansion_target(workflow_expansion_id,rtc_value,neutral_epoch_count);
