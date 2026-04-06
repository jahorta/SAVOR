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
- Override migration root (filesystem migration mode):
  - `SimCoreDBValidation --run all --migration-root <path-to-SimCoreDB/migration>`

## Phase 0 validations

- `phase0.event_contracts`
  - Confirms workflow input events are registered in payload dispatch and enforce `workflow_input_event` payload-family validation.
- `phase0.replay_backfill`
  - Applies Execution migrations into an in-memory sqlite DB, seeds representative legacy/new outbox rows, replays them through `OutboxRelay`, and asserts payload resolution has zero unresolved rows.
