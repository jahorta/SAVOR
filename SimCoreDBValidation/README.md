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
