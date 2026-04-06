# Stage 3e - Projector Subscription Cursors (Outbox Pub-Sub Migration)

## Objective
Migrate UI projection relay behavior from shared outbox consumption markers to **per-projector subscription cursors**, so multiple projectors can safely consume the same source outbox streams without interference.

This stage implements the architecture decision in Stage 0 (D8) and hardens replay/backfill behavior prior to final UI cutover.

## Exit Criteria
- No projector depends on globally marking source outbox rows as published for correctness.
- Each projector tracks progress independently per source outbox stream.
- Replay/reset of one projector does not affect progress of other projectors.
- Source outbox retention/cleanup rules are defined against subscriber progress.

---

## 3e.1 Problem Statement and Target Behavior

### Current risk
- Shared outbox publish markers can cause one consumer to hide events from another consumer when both read the same source outbox stream.
- This can happen for execution events where multiple read models need the same event set.

### Target behavior
- Source outbox tables remain append-only event logs for subscribers.
- Every projector owns its own subscription cursor per source stream.
- Event handling remains idempotent (`event_id`) and ordered (`outbox_id`).

---

## 3e.2 Schema and Metadata Additions (UIRead)

Add new subscription cursor tables in UIRead:

### 1) `ui_projection_subscription`
- `projector_name` (text, not null)
- `source_context` (text, not null)  
  Examples: `Execution`, `State`, `AnalysisSeedProbe`, `AnalysisBattle`, `AnalysisSpine`, `Authoring`, `Archive`
- `source_outbox_table` (text, not null)
- `last_outbox_id` (integer, not null, default 0)
- `last_event_id` (text nullable)
- `updated_at_utc` (integer, not null)
- `status` (text, not null, default `ACTIVE`)  
  enum: `ACTIVE`, `PAUSED`, `ERROR`
- `last_error` (text nullable)
- PRIMARY KEY (`projector_name`, `source_context`, `source_outbox_table`)

### 2) `ui_projection_subscription_audit` (optional but recommended)
- `subscription_audit_id` (PK)
- `projector_name`
- `source_context`
- `source_outbox_table`
- `from_outbox_id`
- `to_outbox_id`
- `processed_count`
- `failed_count`
- `recorded_at_utc`

### Compatibility note
- Keep existing `ui_projection_checkpoint` during migration for backward compatibility.
- Stage 3e completes when all projector relays use `ui_projection_subscription` as source-of-truth progress.

---

## 3e.3 Relay Contract Changes

## Existing contract behavior to retire
- Generic relay marks producer outbox rows as published on handler success.

## New contract behavior
Introduce subscription-aware relay API (conceptual):

1. Resolve subscription row for (`projector_name`, `source_context`, `source_outbox_table`), creating if missing.
2. Read source outbox rows where:
   - `outbox_id > subscription.last_outbox_id`
   - `context_name = source_context`
   - ordered by `outbox_id ASC`
   - limited batch size
3. For each row:
   - validate envelope fields
   - dispatch handler by (`event_type`, `event_version`)
   - enforce idempotency via handled-event table (`projector_name`, `event_id`)
4. If batch succeeds:
   - advance only the subscription cursor (`last_outbox_id`, `last_event_id`, `updated_at_utc`)
5. On failure:
   - keep source outbox unchanged
   - set subscription `status=ERROR`, update `last_error`

### Key design rule
- Subscriber progress is isolated from producer outbox mutation.

---

## 3e.4 Projector Migration Plan

Migrate projector execution paths in this order:

1. **Execution-derived projectors**
   - Job read models
   - Battle execution rollups
   - Workflow read models
2. **State projector**
   - Artifact browser
3. **Analysis projectors**
   - SeedProbe
   - Battle domain tables
   - Spine (no-op consumers can still advance subscriptions)
4. **Archive projector**
   - Archive catalog
5. **Authoring projector subscription**
   - no-op subscriber allowed for observability parity, or remove if unused

For each projector:
- Add subscription bootstrap migration.
- Switch checkpoint reads/writes to subscription table.
- Preserve handled-event idempotency behavior.
- Verify duplicate replay produces no net data drift.

---

## 3e.5 Retention and Cleanup Policy

Define source outbox retention with subscriber awareness:

1. Query subscription status directly from `ui_projection_subscription` for a (`source_context`, `source_outbox_table`) stream.
2. Compute safe floor with a UIRead query helper:
   - `safe_floor_outbox_id = MIN(last_outbox_id) WHERE status='ACTIVE'`.
3. Generate a retention preview report per source module (Execution/State/Analysis/Authoring/Archive):
   - active subscription rows,
   - lag per subscription (`source_max_outbox_id - last_outbox_id`),
   - computed safe purge floor.
4. Enforce purge blocking policy:
   - if any **required** subscription stays `PAUSED`/`ERROR` beyond threshold, purge/archive helpers must stop and return a block reason.
5. Purge/archive rows strictly below the safe floor (and still subject to retention horizon/time policy).
6. Keep forced-retention override as an explicit operator action for emergency disk pressure.

### Operator workflow (actual)
1. List subscriptions and safe floor from UIRead.
2. Build source-context retention preview in the owning DB service.
3. If preview reports blocking required subscriptions, resolve them first (resume/reset/decommission).
4. Run bounded purge batches using the owning service helper.
5. Re-run preview until target window is reached.

---

## 3e.6 Rollout Strategy

1. **Dual progress mode**
   - Continue writing legacy checkpoints while introducing subscription cursors.
2. **Shadow validation**
   - Compare legacy checkpoint position vs subscription position over repeated runs.
3. **Primary switch**
   - Make subscription cursor the authoritative progress source.
4. **Cleanup**
   - Remove relay dependency on producer `published_at_utc` for projector correctness.
5. **Post-cut migration**
   - Remove deprecated checkpoint paths once Stage 5 cutover stabilizes.

---

## 3e.7 Validation and Test Matrix

### Correctness
- Two projectors consuming the same source outbox both receive/process identical eligible event IDs.
- Resetting one projector cursor does not modify or regress other projector cursors.

### Idempotency
- Replaying overlapping outbox ranges produces no duplicate read-model rows.
- Duplicate event delivery is absorbed by handled-event idempotency.

### Failure isolation
- One projector entering `ERROR` does not block other projector subscriptions from progressing.

### Performance/ops
- Subscription cursor update latency and relay throughput meet target.
- Retention safe-floor computation scales with number of subscriptions.

---

## 3e.8 Deliverables

- UIRead migration SQL for subscription tables.
- Subscription-aware relay coordinator implementation.
- Projector adapters migrated to subscription cursors.
- Retention policy/runbook updates.
- Integration tests proving multi-projector same-stream safety.

---

## Dependencies and Sequencing

- Depends on Stage 3/3d event catalog and projector baseline.
- Can run in parallel with late Stage 4 archive hardening where practical.
- Must complete before final Stage 5 signoff to avoid projection correctness risks during UIRead cutover.

---

## 3e.9 Migration Notes (Checkpoint Retirement)

### Added migration
- `SimCoreDB/migration/UIRead/202604051500_uiread_stage3f_projection_checkpoint_retire.sql`

### What this migration does
1. Runs under `BEGIN IMMEDIATE` to keep checkpoint-retirement changes atomic.
2. Drops `ix_ui_projection_checkpoint_outbox` and then drops `ui_projection_checkpoint` after subscription cursor cutover.
3. Includes validation query comments to verify:
   - legacy checkpoint table/index are gone, and
   - `ui_projection_subscription` is the only cursor-progress source.

### Rollout guardrail
- Apply only after projector runtime code no longer reads/writes `ui_projection_checkpoint`.
