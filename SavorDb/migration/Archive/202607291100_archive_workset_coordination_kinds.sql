BEGIN IMMEDIATE;

INSERT OR IGNORE INTO ar_archive_item_kind_catalog(item_kind, include_by_default, notes)
VALUES
    ('worksets', 1, 'Published execution worksets'),
    ('workset_dispatch_attempts', 1, 'Durable workset dispatch-attempt history'),
    ('job_cancellation_requests', 1, 'Authoritative job-cancellation request history');

COMMIT;
