# Stage 4 - Execution Archive and Rehydrate

## Objective
Implement lifecycle for ephemeral execution data: archive old execution rows to JSONL+blobs and restore into Execution DB when requested.

## Exit Criteria
- Archiver can package terminal execution data.
- Archive index records are created.
- Rehydration recreates execution rows safely.
- UI can list archived packages and rehydrate status.

---

## 4.1 Archive Package Format (v1)

Directory structure:

- `manifest.json`
- `job_sets.jsonl`
- `jobs.jsonl`
- `job_events.jsonl`
- `workflow_instances.jsonl`
- `workflow_steps.jsonl`
- `workflow_edges.jsonl`
- `workflow_events.jsonl` (optional)
- `triggers.jsonl` (optional)
- `outbox.jsonl` (optional)
- `blobs/` (large payload files)
- `checksums.json`

### `manifest.json` fields
- `archive_package_id`
- `source_context` (`Execution`)
- `source_root_job_set_id`
- `created_at_utc`
- `schema_version`
- `event_catalog_version`
- `time_range_start_utc`
- `time_range_end_utc`
- `counts` (job_sets, jobs, job_events, workflow_instances, workflow_steps, workflow_edges, workflow_events, triggers, blobs)

---

## 4.2 Archiver Pipeline

1. Select eligible root job sets by retention policy.
2. Export `exec_job_set`, `exec_job`, `exec_job_event`, workflow rows (`exec_workflow_instance`, `exec_workflow_step`, `exec_workflow_edge`, optional `exec_workflow_event`), and optional `exec_trigger` rows to JSONL in deterministic order.
3. Extract large payloads to blobs folder and replace payloads with blob references.
4. Compute checksums.
5. Store package files in ArchiveStore root.
6. Write `ar_archive_package` and `ar_archive_item` rows.
7. Emit `Archive.PackageCreated.v1` and `Archive.PackageIndexed.v1`.
8. Mark execution rows as archived or purge based on policy, with subscriber-aware safety checks for any projected outbox streams.

### Retention Policy v1
- Eligible if root run is terminal and older than configured threshold.
- Never archive jobs with active leases.
- Optional immediate archive for oversized event payload runs.
- For any source outbox rows considered for purge/archive, enforce Stage 3e subscriber safety floor:
  - only purge rows strictly below `MIN(last_outbox_id)` across active projector subscriptions for that source stream.
  - if any required subscription is paused/error beyond threshold, block purge and require operator action.
  - use owning-module retention preview helpers to compute lag, active rows, and safe floor before each purge batch.

---

## 4.3 Rehydration Pipeline

1. Create `ar_rehydrate_request` with `REQUESTED`.
2. Validate package checksum and schema compatibility.
3. Allocate restore namespace token.
4. Import job_sets/jobs/events/workflows into Execution DB with deterministic ID remapping.
5. Build `ar_rehydrate_map` old_id -> new_id.
6. Recreate trigger rows if included.
7. Regenerate execution fingerprints deterministically for namespace safety.
8. Emit `Execution.JobRestored.v1` and `Archive.RehydrateCompleted.v1`.
9. Mark request status `COMPLETED`.

### Rehydrate Failure Handling
- On error, persist partial progress markers.
- Emit `Archive.RehydrateFailed.v1`.
- Leave imported rows isolated by namespace for cleanup/retry.

---

## 4.4 Fingerprint and Identity Rules

- Do not depend on a dedicated `original_fingerprint` execution column.
- Preserve original IDs/fingerprints in archive package records.
- Use deterministic namespace-aware remap during restore to generate safe runtime fingerprints.
- Persist deterministic old->new mapping in `ar_rehydrate_map`.

---

## 4.5 Operational Tooling

Provide commands/tools for:
- dry-run archive preview
- package integrity verification
- rehydrate preview (counts only)
- full restore execution
- restore cleanup
- subscriber-safe purge preview (shows active subscription floors and would-purge ranges)

### Operator runbook alignment (Stage 3e + Stage 4)
1. Query UIRead subscriptions for the source stream and compute safe floor (`MIN(last_outbox_id)` over `ACTIVE`).
2. Feed subscription snapshots into the owning service retention preview (Execution/State/Analysis/Authoring/Archive) to get:
   - active subscriptions,
   - lag per subscription,
   - safe purge floor.
3. If required subscriptions are stalled in `PAUSED`/`ERROR` beyond policy threshold, stop and remediate before purge.
4. Execute purge in bounded batches through owning service helpers (published rows only, strictly below floor).
5. Repeat preview + purge loop until archive retention target is reached.

---

## 4.6 Test Matrix

- Archive correctness (row counts match manifest).
- Checksum verification tests.
- Rehydrate no-collision tests.
- Rehydrate then claim-next-job smoke tests.
- Roundtrip test: execute -> archive -> purge -> rehydrate -> query jobs/events.
- Subscriber-safety retention test: purge candidate ranges never exceed active subscription floors.
