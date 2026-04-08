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
- Override migration root (filesystem migration mode):
  - `SimCoreDBValidation --run all --migration-root <path-to-SimCoreDB/migration>`

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
  - Applies Execution/Authoring/State migrations, seeds placeholder materialization prerequisites (`au_seed_probe_spec`, `state_artifact`, `state_savestate`), validates `.sav`-shaped fixture handling, and checks replay cursor robustness across restart-like replays.
- `phase3.per_service_dedupe_isolation`
  - Verifies dedupe keys are isolated per service instance so one service’s dedupe decisions do not suppress another’s.
- `phase3.progress_terminal_stream_separation`
  - Verifies high-frequency progress/input events are persisted in `exec_workflow_input_event` while terminal state is persisted in `exec_workflow_event`.
- `phase3.lag_dead_letter_readiness`
  - Verifies outbox relay dead-letter behavior and confirms lag-preview telemetry can detect lagging subscriptions.
