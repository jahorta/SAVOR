# DBMigrateQueues 01 — Contracts and Message Model

## Status
Draft v0.3 (planning)

## Purpose
Define the logical command/query contracts used by per-context queues/workers and their response semantics.

## Goals
- Standardize async envelopes across all contexts.
- Make retries/idempotency safe and explicit.
- Support strong traceability across workflows and events.

## Command Envelope (logical)
Suggested fields:
- `request_id` (unique for this request)
- `correlation_id` (workflow/request chain)
- `causation_id` (upstream event/command id)
- `context` (execution/state/analysis/authoring/ui_read/archive)
- `command_name`
- `submitted_at_utc`
- `idempotency_key` (required for side-effecting commands)
- `payload` (typed command object)

## Query Envelope (logical)
Suggested fields:
- `request_id`
- `correlation_id`
- `context`
- `query_name`
- `submitted_at_utc`
- `payload` (typed query object)

## Command Result Model
- `state`: `success | canceled | failed`
- `request_id`
- `context`
- `command_name`
- `completed_at_utc`
- `error` (typed, optional)
- operation-specific response object (optional, per command)

## Query Result Model
- `state`: `success | canceled | failed`
- `request_id`
- `context`
- `query_name`
- `result` (typed DTO, nullable on non-success)
- `completed_at_utc`
- `error` (typed, optional)
- operation-specific response object (optional, per query)

## Error Taxonomy (starter)
- ValidationError (bad request / invariant violation)
- ConflictError (optimistic conflict, already terminal state, duplicate semantic op)
- NotFoundError
- TransientInfraError (retryable)
- PermanentInfraError (non-retryable)

## Standardized Machine-Readable Error Codes (Decision)
Use standardized machine-readable error codes from day one.

Starter pattern:
- `SCDB_VALIDATION_*`
- `SCDB_CONFLICT_*`
- `SCDB_NOT_FOUND_*`
- `SCDB_TRANSIENT_*`
- `SCDB_PERMANENT_*`

Guidance:
- Include `code` + human-readable `message` in error payloads.
- Keep codes stable even if message text changes.


## Initial v1 Error Code Baseline (Decision)
Core cross-context codes:
- `SCDB_VALIDATION_REQUIRED_FIELD`
- `SCDB_VALIDATION_INVALID_VALUE`
- `SCDB_NOT_FOUND`
- `SCDB_CONFLICT_STATE`
- `SCDB_TRANSIENT_DB_BUSY`
- `SCDB_TRANSIENT_TIMEOUT`
- `SCDB_PERMANENT_DB_ERROR`

Context extension prefixes:
- `SCDB_EXEC_*`, `SCDB_ANALYSIS_*`, `SCDB_STATE_*`, `SCDB_AUTHORING_*`, `SCDB_ARCHIVE_*`, `SCDB_UIREAD_*`

## Idempotency Guidance
- Commands that mutate state MUST include idempotency key.
- Command handlers SHOULD store a dedupe record keyed by context + idempotency key + command name.
- Replays return previous completion metadata.
- Initial storage decision: use **per-context idempotency table**.

## Traceability Guidance
- All command writes should preserve correlation/causation ids in event emission where available.
- Query responses include request ids for log stitching.

## Decisions Logged (Latest Review)
1. Operation-specific response objects will be introduced case-by-case as implementation proceeds.
2. Initial machine-readable error codes should start with a core cross-context set plus context-specific extensions.
3. Error payloads should include optional `retry_after_ms` hints for transient/backpressure scenarios when known.

## Open Questions
- None currently.
