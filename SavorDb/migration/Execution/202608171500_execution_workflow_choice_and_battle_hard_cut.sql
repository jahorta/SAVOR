PRAGMA foreign_keys = OFF;
BEGIN IMMEDIATE;

ALTER TABLE exec_workflow_instance_argument
    RENAME TO exec_workflow_instance_argument_legacy;

CREATE TABLE exec_workflow_instance_argument (
    workflow_instance_argument_id INTEGER PRIMARY KEY,
    workflow_instance_id INTEGER NOT NULL,
    node_key TEXT NOT NULL DEFAULT '',
    argument_key TEXT NOT NULL,
    value_type TEXT NOT NULL CHECK(value_type IN ('integer','text','json','boolean','choice')),
    integer_value INTEGER NULL,
    text_value TEXT NULL,
    source_kind TEXT NULL,
    created_at_utc INTEGER NOT NULL,
    FOREIGN KEY(workflow_instance_id) REFERENCES exec_workflow_instance(workflow_instance_id),
    CONSTRAINT uq_exec_workflow_instance_argument UNIQUE (workflow_instance_id,node_key,argument_key),
    CHECK(
        (value_type IN ('integer','boolean') AND integer_value IS NOT NULL AND text_value IS NULL)
        OR
        (value_type IN ('text','json','choice') AND integer_value IS NULL AND text_value IS NOT NULL)
    )
);

INSERT INTO exec_workflow_instance_argument(
    workflow_instance_argument_id,workflow_instance_id,node_key,argument_key,
    value_type,integer_value,text_value,source_kind,created_at_utc)
SELECT workflow_instance_argument_id,workflow_instance_id,node_key,argument_key,
       value_type,integer_value,text_value,source_kind,created_at_utc
FROM exec_workflow_instance_argument_legacy;

DROP TABLE exec_workflow_instance_argument_legacy;
CREATE INDEX ix_exec_workflow_instance_argument_instance
    ON exec_workflow_instance_argument(workflow_instance_id,node_key,argument_key);

-- Terminalize only nonterminal workflows that contain a pre-cut Battle step.
CREATE TEMP TABLE _battle_hard_cut_workflow(workflow_instance_id INTEGER PRIMARY KEY);
INSERT INTO _battle_hard_cut_workflow(workflow_instance_id)
SELECT DISTINCT workflow_instance_id
FROM exec_workflow_step
WHERE step_kind IN ('battle.start','battle.single_turn');

UPDATE exec_workset_dispatch_attempt
SET state='CLOSED',lease_expires_at_utc=NULL,draining_at_utc=NULL,
    closed_at_utc=unixepoch('now')*1000,
    close_reason_code='BATTLE_DIRECT_PLAN_HARD_CUT',
    close_reason_text='Pre-cut Battle execution was terminalized for the direct Battle Plan contract.'
WHERE state IN ('CLAIMED','ACTIVE','DRAINING')
  AND workset_id IN (
      SELECT w.workset_id FROM exec_workset w
      JOIN exec_workflow_step s ON s.workflow_step_id=w.workflow_step_id
      JOIN _battle_hard_cut_workflow h ON h.workflow_instance_id=s.workflow_instance_id
  );

UPDATE exec_job
SET state='FAILED',ended_at_utc=COALESCE(ended_at_utc,unixepoch('now')*1000),
    claimed_by_token=NULL,lease_expires_at_utc=NULL,
    error_code='BATTLE_DIRECT_PLAN_HARD_CUT',
    error_text='Pre-cut Battle execution was terminalized for the direct Battle Plan contract.'
WHERE state NOT IN ('SUCCEEDED','SUCCEEDED_WINNER','FAILED','CANCELED')
  AND job_set_id IN (
      SELECT job_set_id FROM exec_workflow_step s
      JOIN _battle_hard_cut_workflow h ON h.workflow_instance_id=s.workflow_instance_id
      WHERE job_set_id IS NOT NULL
  );

UPDATE exec_workflow_step
SET state=CASE WHEN state IN ('WAITING','READY') THEN 'SKIPPED' ELSE 'FAILED' END,
    blocked_reason='BATTLE_DIRECT_PLAN_HARD_CUT',
    failed_at_utc=CASE WHEN state IN ('WAITING','READY') THEN failed_at_utc
                       ELSE COALESCE(failed_at_utc,unixepoch('now')*1000) END
WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM _battle_hard_cut_workflow)
  AND state NOT IN ('COMPLETED','FAILED','SKIPPED');

UPDATE exec_workflow_instance
SET state='FAILED',completed_at_utc=COALESCE(completed_at_utc,unixepoch('now')*1000),
    failure_code='BATTLE_DIRECT_PLAN_HARD_CUT',
    failure_text='Pre-cut Battle workflow requires relaunch with a direct Battle Plan and typed continuation.'
WHERE workflow_instance_id IN (SELECT workflow_instance_id FROM _battle_hard_cut_workflow)
  AND state IN ('PENDING','RUNNING');

DROP TABLE _battle_hard_cut_workflow;
COMMIT;
PRAGMA foreign_keys = ON;
