# Stage 4 - Execution Archive and Rehydrate

## Objective
Implement lifecycle for ephemeral execution data: archive old rows to JSONL+blobs and restore into execution DB when requested.

## Exit Criteria
- Archiver can package terminal execution data.
- Archive index records are created.
- Rehydration recreates execution rows safely.
- UI can list archived packages.

---

## 4.1 Archive Package Format (v1)

Directory structure:

- `manifest.json`
- `jobs.jsonl`
- `job_events.jsonl`
- `job_sets.jsonl`
- `triggers.jsonl` (optional)
- `blobs/` (large payload files)
- `checksums.json`

### `manifest.json` fields
- `archive_package_id`
- `source_context`
- `source_root_job_set_id`
- `created_at_utc`
- `schema_version`
- `event_catalog_version`
- `time_range_start_utc`
- `time_range_end_utc`
- `counts` (job_sets, jobs, job_events, blobs)

---

## 4.2 Archiver Pipeline

1. Select eligible runs by retention policy.
2. Export execution rows to JSONL in deterministic order.
3. Extract large payloads to blobs folder and replace payload references.
4. Compute checksums.
5. Store package files in ArchiveStore root.
6. Write `ar_archive_package` and `ar_archive_item` rows.
7. Emit `Archive.PackageCreated.v1` and `Archive.PackageIndexed.v1`.
8. Mark execution rows as archived or purge based on policy.

### Retention Policy v1
- Eligible if run terminal and older than configured threshold.
- Never archive jobs with active leases.
- Optional immediate archive for oversized event payload runs.

---

## 4.3 Rehydration Pipeline

1. Create `ar_rehydrate_request` with `REQUESTED`.
2. Validate package checksum and schema compatibility.
3. Allocate restore namespace/token.
4. Import job_sets/jobs/events into Execution DB.
5. Build `ar_rehydrate_map` old_id -> new_id.
6. Recreate trigger rows if included.
7. Emit `Execution.JobRestored.v1` and `Archive.RehydrateCompleted.v1`.
8. Mark request status `COMPLETED`.

### Rehydrate Failure Handling
- On error, persist partial progress markers.
- Emit `Archive.RehydrateFailed.v1`.
- Leave imported rows isolated by namespace for cleanup/retry.

---

## 4.4 Fingerprint and Identity Rules

- Preserve original fingerprint in `original_fingerprint` column.
- Compute runtime fingerprint variant with namespace suffix to avoid collisions.
- Keep deterministic mapping in `ar_rehydrate_map`.

---

## 4.5 Operational Tooling

Provide commands/tools for:
- dry-run archive preview
- package integrity verification
- rehydrate preview (counts only)
- full restore execution
- restore cleanup

---

## 4.6 Test Matrix

- Archive correctness (row counts match manifest).
- Checksum verification tests.
- Rehydrate no-collision tests.
- Rehydrate then claim-next-job smoke tests.
- Roundtrip test: execute -> archive -> purge -> rehydrate -> query events.
