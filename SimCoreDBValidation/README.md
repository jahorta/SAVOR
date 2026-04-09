# SimCoreDBValidation

Command-line validation tool for DB-migrate workflow phase gates.

## Usage

- List validations:
  - `SimCoreDBValidation --list`
- Run all validations:
  - `SimCoreDBValidation --run all`
- Run a specific validation:
  - `SimCoreDBValidation --run phase0.event_contracts`
  - `SimCoreDBValidation --run phase0.replay_backfill`
  - `SimCoreDBValidation --run phase1.aggregation_gating`
  - `SimCoreDBValidation --run phase1.timeout_retry_once`
  - `SimCoreDBValidation --run phase3.replay_robustness`
  - `SimCoreDBValidation --run phase3.per_service_dedupe_isolation`
  - `SimCoreDBValidation --run phase3.progress_terminal_stream_separation`
  - `SimCoreDBValidation --run phase3.lag_dead_letter_readiness`
  - `SimCoreDBValidation --run phase4.invariant_violation_remediation_sequence`
  - `SimCoreDBValidation --run phase4.power_loss_during_claimed_job_materialization`
  - `SimCoreDBValidation --run phase4.duplicate_terminal_replay`
  - `SimCoreDBValidation --run phase4.partial_writer_failure_recovery`
  - `SimCoreDBValidation --run phase4.missing_decision_result_restart_rerun`
  - `SimCoreDBValidation --run phase4.observability_retention_readiness`
- Override migration root (filesystem migration mode):
  - `SimCoreDBValidation --run all --migration-root <path-to-SimCoreDB/migration>`
- Override phase-3 default seed rows JSON and/or provide additional JSONL row folder:
  - `SimCoreDBValidation --run phase3.replay_robustness --phase3-default-rows-json <path-to-default-rows.json>`
  - `SimCoreDBValidation --run phase3.replay_robustness --phase3-jsonl-dir <path-to-jsonl-folder>`
  - `SimCoreDBValidation --run phase3.replay_robustness --savestate-file <path-to-file.sav>`
  - JSONL files are mapped by filename stem to table name (for example, `exec_outbox_message.jsonl` -> `exec_outbox_message`).

## Phase 0 validations

- `phase0.event_contracts`
  - Confirms workflow input events are registered in payload dispatch and enforce `workflow_input_event` payload-family validation.
- `phase0.replay_backfill`
  - Applies Execution migrations into an in-memory sqlite DB, seeds representative legacy/new outbox rows, replays them through `OutboxRelay`, and asserts payload resolution has zero unresolved rows.

## Phase 1 validations

- `phase1.aggregation_gating`
  - Seeds a Seed Probe READY step, appends `input_requested`, `input_fragment_ready`, and `input_complete` events through command service, and verifies workflow-step-scoped outbox payloads are emitted.
- `phase1.timeout_retry_once`
  - Exercises timeout-retry-once policy shape by appending two timeout request events and asserting failed terminal transition is accepted for the step.

## Phase 3 validations

- `phase3.replay_robustness`
  - Applies Execution/Authoring/State migrations through `DbPreparer`, optionally inserts `--savestate-file` into `state_artifact`/`state_savestate` first and uses that savestate id to override subsequent seeded `*savestate_id` columns, seeds placeholder materialization prerequisites from a JSON object (`au_seed_probe_spec`, `state_artifact`, `state_savestate`, `exec_outbox_message`), optionally applies additional per-table JSONL rows from a folder, validates `.sav`-shaped fixture handling, and checks replay cursor robustness across restart-like replays.
- `phase3.per_service_dedupe_isolation`
  - Verifies dedupe keys are isolated per service instance so one service’s dedupe decisions do not suppress another’s.
- `phase3.progress_terminal_stream_separation`
  - Verifies high-frequency progress/input events are persisted in `exec_workflow_input_event` while terminal state is persisted in `exec_workflow_event`.
- `phase3.lag_dead_letter_readiness`
  - Verifies outbox relay dead-letter behavior and confirms lag-preview telemetry can detect lagging subscriptions.

## Phase 4 validations

- `phase4.invariant_violation_remediation_sequence`
  - Pass criteria: remediation ordering reaches detection, isolation, reconciliation, and queue-resume stages without gaps.
- `phase4.power_loss_during_claimed_job_materialization`
  - Pass criteria: restart path detects incomplete claimed-job materialization and reruns to a committed state.
- `phase4.duplicate_terminal_replay`
  - Pass criteria: replaying duplicate terminal events applies terminal transition exactly once.
- `phase4.partial_writer_failure_recovery`
  - Pass criteria: partial writer failure records rollback intent and returns persisted state to pre-write consistency.
- `phase4.missing_decision_result_restart_rerun`
  - Pass criteria: restart detects missing decision-result output and schedules/runs rerun until decision result exists.
- `phase4.observability_retention_readiness`
  - Pass criteria: completion-gate mismatch frequency, replay-loop symptom count, and dedupe-growth anomaly ratio signals are computed; dedupe TTL and claimed-job staging cleanup policy values are present and bounded; escalation thresholds are coherent.

## Phase 4 policy + alert thresholds reference

The validation currently enforces the following policy defaults and guardrails:

- **Dedupe table TTL**: `168h` (7 days), sane range `[24h, 720h]`.
- **Claimed-job staging cleanup window**: `36h`, sane range `[6h, 168h]`.

The same validation also enforces non-empty/escalating threshold policies used by runbook alerting:

- **Completion-gate mismatch frequency** (mismatches / gate checks):
  - Warn: `>= 0.005` (0.5%)
  - Page/escalate: `>= 0.02` (2.0%)
- **Repeated replay-loop symptom count** (steps with >=3 replays in sample window):
  - Warn: `>= 3`
  - Page/escalate: `>= 6`
- **Dedupe growth anomaly ratio** (`current_hour_growth / baseline_hourly_growth`):
  - Warn: `>= 1.4x`
  - Page/escalate: `>= 2.0x`
