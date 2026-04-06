# 06 - Phase 0 Detailed Plan: Contracts and Schema Prep

## Phase intent

Prepare contracts and schema so later phases can ship safely without changing core runtime behavior yet.

## Add / Modify / Remove

## Add

1. **Event contract artifacts**
   - Add concrete event definitions for workflow input orchestration events (v1).
   - Add payload schemas for `workflow_input_event` family.
   - Add envelope-level validation fixtures for required fields.

2. **Schema migrations (Execution + payload refs)**
   - Add migration scripts for payload tables/refs needed by `workflow_input_event`.
   - Add indexes for expected lookup patterns:
     - by `workflow_step_id`
     - by `payload_ref_kind/payload_ref_id`
     - by event replay cursor

3. **Resolver/dispatcher registration**
   - Register payload resolvers and dispatch bindings for new input event kinds.
   - Add backward-read compatibility paths for existing `workflow_event` payload family.

4. **Replay/backfill validation tools**
   - Add a validation task/CLI path to replay outbox rows and verify payload resolution success.

## Modify

1. **Event catalog docs + code constants**
   - Extend event catalog with new workflow input events.

2. **Outbox relay bindings**
   - Extend relevant relay bindings for new event types (no behavior change yet).

3. **Developer docs**
   - Update naming/versioning guidance for `Execution.*` namespace and aggregate-id conventions.

## Remove

- No removals in Phase 0.
- Keep all legacy event families active.

## Guidance and constraints

1. **Backward compatibility first**
   - New contracts are additive; existing event readers must continue to function.

2. **No runtime behavior change goal**
   - Phase 0 should not change claim/materialize/dispatch behavior.

3. **Validation gates before phase exit**
   - New events pass schema checks.
   - Replay/backfill pass has zero unresolved payload rows.

## Suggested SimCoreTests to add for phase exit readiness

1. **Event contract schema coverage test**
   - Validate all new `workflow_input_event` payload shapes pass required-field checks.

2. **Migration apply/rollback test**
   - Apply new migration set, verify required tables/indexes exist, then rollback and verify clean reapply.

3. **Resolver registration test**
   - Ensure each new event type resolves to a registered payload resolver/dispatch binding.

4. **Replay/backfill integrity test**
   - Feed representative outbox rows through replay path and assert zero unresolved payload references.

5. **Backward-read compatibility test**
   - Assert legacy `workflow_event` rows remain readable after introducing `workflow_input_event` family.
