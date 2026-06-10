BEGIN IMMEDIATE;

CREATE TABLE IF NOT EXISTS ui_projection_subscription (
    projector_name TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    last_outbox_id INTEGER NOT NULL DEFAULT 0,
    last_event_id TEXT NULL,
    updated_at_utc INTEGER NOT NULL,
    status TEXT NOT NULL DEFAULT 'ACTIVE' CHECK(status IN ('ACTIVE', 'PAUSED', 'ERROR')),
    last_error TEXT NULL,
    PRIMARY KEY (projector_name, source_context, source_outbox_table)
);

CREATE INDEX IF NOT EXISTS ix_ui_projection_subscription_stream_status_outbox
    ON ui_projection_subscription(source_context, source_outbox_table, status, last_outbox_id);

CREATE INDEX IF NOT EXISTS ix_ui_projection_subscription_status_updated
    ON ui_projection_subscription(status, updated_at_utc DESC);

CREATE TABLE IF NOT EXISTS ui_projection_subscription_audit (
    subscription_audit_id INTEGER PRIMARY KEY,
    projector_name TEXT NOT NULL,
    source_context TEXT NOT NULL,
    source_outbox_table TEXT NOT NULL,
    from_outbox_id INTEGER NOT NULL,
    to_outbox_id INTEGER NOT NULL,
    processed_count INTEGER NOT NULL DEFAULT 0,
    failed_count INTEGER NOT NULL DEFAULT 0,
    recorded_at_utc INTEGER NOT NULL
);

CREATE INDEX IF NOT EXISTS ix_ui_projection_subscription_audit_stream_recorded
    ON ui_projection_subscription_audit(source_context, source_outbox_table, recorded_at_utc DESC);

INSERT INTO ui_projection_subscription(
    projector_name,
    source_context,
    source_outbox_table,
    last_outbox_id,
    last_event_id,
    updated_at_utc,
    status,
    last_error)
SELECT
    CASE
        WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.exec_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.state_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.sp_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.ab_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.asp_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.au_outbox_message'))
        WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.ar_outbox_message'))
        ELSE NULL
    END AS projector_name,
    CASE
        WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN 'Execution'
        WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN 'State'
        WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN 'AnalysisSeedProbe'
        WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN 'AnalysisBattle'
        WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN 'AnalysisSpine'
        WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN 'Authoring'
        WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN 'Archive'
        ELSE NULL
    END AS source_context,
    CASE
        WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN 'exec_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN 'state_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN 'sp_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN 'ab_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN 'asp_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN 'au_outbox_message'
        WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN 'ar_outbox_message'
        ELSE NULL
    END AS source_outbox_table,
    COALESCE(checkpoint.last_outbox_id, 0),
    checkpoint.last_event_id,
    checkpoint.updated_at_utc,
    'ACTIVE',
    NULL
FROM ui_projection_checkpoint checkpoint
WHERE
    (
        checkpoint.projector_name LIKE '%.exec_outbox_message' OR
        checkpoint.projector_name LIKE '%.state_outbox_message' OR
        checkpoint.projector_name LIKE '%.sp_outbox_message' OR
        checkpoint.projector_name LIKE '%.ab_outbox_message' OR
        checkpoint.projector_name LIKE '%.asp_outbox_message' OR
        checkpoint.projector_name LIKE '%.au_outbox_message' OR
        checkpoint.projector_name LIKE '%.ar_outbox_message'
    )
    AND NOT EXISTS (
        SELECT 1
        FROM ui_projection_subscription existing
        WHERE existing.projector_name = CASE
            WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.exec_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.state_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.sp_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.ab_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.asp_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.au_outbox_message'))
            WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN substr(checkpoint.projector_name, 1, length(checkpoint.projector_name) - length('.ar_outbox_message'))
            ELSE ''
        END
          AND existing.source_context = CASE
            WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN 'Execution'
            WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN 'State'
            WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN 'AnalysisSeedProbe'
            WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN 'AnalysisBattle'
            WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN 'AnalysisSpine'
            WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN 'Authoring'
            WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN 'Archive'
            ELSE ''
        END
          AND existing.source_outbox_table = CASE
            WHEN checkpoint.projector_name LIKE '%.exec_outbox_message' THEN 'exec_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.state_outbox_message' THEN 'state_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.sp_outbox_message' THEN 'sp_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.ab_outbox_message' THEN 'ab_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.asp_outbox_message' THEN 'asp_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.au_outbox_message' THEN 'au_outbox_message'
            WHEN checkpoint.projector_name LIKE '%.ar_outbox_message' THEN 'ar_outbox_message'
            ELSE ''
        END
    );

COMMIT;
