# Stage 4 Archive/Rehydrate Operator Runbook

This runbook maps Stage 4 operator commands to remediation steps.

## Command sequence

1. **archive preview**
   - Use before any purge/archive action to collect candidate counts, safe floors, and blocking reasons.
   - If `blocking_reasons` is non-empty, stop and remediate before execute.

2. **archive execute**
   - Runs package write + source purge policy + bounded outbox purge.
   - Capture machine-readable fields: `candidate_count`, `package_count`, `purged_outbox_rows`, `purged_source_roots`, `errors`.

3. **package verify**
   - Validate manifest row counts against indexed package item counts.
   - Validate on-disk checksums for each package file.
   - Capture `manifest_row_total`, `archive_item_row_total`, `checksum_verified_files`, and `blocking_reasons`.

4. **rehydrate preview**
   - Dry run package integrity and expected row restore counts.
   - Capture `manifest_file_count`, `manifest_row_total`, `expected_jobs`, and `blocking_reasons`.

5. **rehydrate execute**
   - Creates request row and restores rows with no-collision ID remap.
   - Capture `request_ids`, restored job counts, and structured errors.

6. **rehydrate cleanup**
   - Remove terminal request + map rows after post-restore validation is complete.
   - Cleanup is blocked for active (`REQUESTED`) requests.

## Remediation matrix

### A) Purge/Archive is blocked by subscriber floor
- Symptom: `archive preview` contains blocking reasons derived from paused/error subscriptions or missing safe floor.
- Actions:
  1. Query `ui_projection_subscription` for `Execution/exec_outbox_message`.
  2. Resume required projectors that are `PAUSED`/`ERROR`.
  3. Wait until `last_outbox_id` advances.
  4. Re-run **archive preview** and confirm a stable floor.

### B) Package verification fails row counts
- Symptom: `package verify` emits `row_count_mismatch:<item_kind>`.
- Actions:
  1. Rebuild package for the same root id with a new event/correlation id.
  2. Compare `ar_archive_item` rows and manifest `files[]` entries.
  3. Keep the bad package quarantined until postmortem is complete.

### C) Package verification fails checksum
- Symptom: `package verify` emits `checksum_mismatch:<path>`.
- Actions:
  1. Treat package as corrupted (disk or transfer).
  2. Re-copy artifacts from trusted storage or regenerate package.
  3. Re-run **package verify** and require zero checksum blockers.

### D) Rehydrate execute fails
- Symptom: `rehydrate execute` returns `success=false` with structured error payload.
- Actions:
  1. Inspect `ar_rehydrate_request.error_text` and archive outbox event payloads.
  2. Fix root cause (schema/version mismatch, missing files, checksum mismatch).
  3. Re-run with a new request id (do not mutate old request row).

### E) Cleanup blocked
- Symptom: `rehydrate cleanup` returns a blocking reason for active request.
- Actions:
  1. Confirm request is terminal (`COMPLETED` or `FAILED`).
  2. If still `REQUESTED`, finish/abort rehydrate execution first.
  3. Re-run cleanup and verify both `ar_rehydrate_request` and `ar_rehydrate_map` rows are removed.

## Post-operation checks

- Query representative restored jobs/events and confirm expected business keys.
- Validate no required projector subscription trails behind safe floor before next purge batch.
- Archive the command summaries (JSON fields) with incident/change ticket references.
