# 06 - Phase 0 Detailed Plan: Contracts and Schema Prep

## Phase intent

Prepare contracts and schema so later phases can ship safely without changing core runtime behavior yet.

## Add / Modify / Remove

## Add

1. **Event contract artifacts**
   - Keep concrete event definitions for in-memory workflow input orchestration where the step aggregation service needs them.
   - Do not persist a separate `workflow_input_event` payload family for launch inputs.
   - Add envelope-level validation fixtures for durable outbox event refs that remain in the active runtime.

2. **Schema migrations (Execution + payload refs)**
   - Keep schema migrations for durable workflow lifecycle events, instance input bindings, and scalar instance arguments.
   - The active schema no longer includes `exec_workflow_input_event`.
   - Keep indexes for expected lookup patterns:
     - by `payload_ref_kind/payload_ref_id`
     - by event replay cursor

3. **Resolver/dispatcher registration**
   - Register payload resolvers and dispatch bindings only for durable payload families that remain in the active runtime.
   - Keep `workflow_event` payload resolution for lifecycle/outbox replay.

4. **Replay/backfill validation tools**
   - Add a validation task/CLI path to replay outbox rows and verify payload resolution success.

## Modify

1. **Event catalog docs + code constants**
   - Keep durable event catalog entries aligned with active persisted payload families.
   - In-memory step-input aggregation names do not require `exec_workflow_input_event` storage.

2. **Outbox relay bindings**
   - Extend relevant relay bindings for new event types (no behavior change yet).

3. **Developer docs**
   - Update naming/versioning guidance for `Execution.*` namespace and aggregate-id conventions.

## Remove

- Remove the obsolete `workflow_input_event` durable payload family and `exec_workflow_input_event` table from active schema/code.
- Compatibility with those early workflow-input storage structures is not required for this cleanup pass.

## Guidance and constraints

1. **Fresh active schema**
   - This branch is still early in production; removed early workflow-input storage does not need compatibility adapters.

2. **No runtime behavior change goal**
   - Phase 0 should not change claim/materialize/dispatch behavior.

3. **Validation gates before phase exit**
   - New events pass schema checks.
   - Replay/backfill pass has zero unresolved payload rows.
   - Both current verification paths are required: `SavorTests` and relevant SavorE2E scenarios.
   - Future schema-report output should come from `SavorDbSchemaExport`.

## Validation execution requirements

1. **SavorTests**
   - Run phase-0 schema/contract tests in `SavorTests`.
2. **Schema/reporting utility**
   - Keep phase-0 contract checks in `SavorTests` and SavorE2E scenarios.
   - Once functional, run `SavorDbSchemaExport` to refresh human-readable schema reports.

## Suggested SavorTests to add for phase exit readiness

1. **Event contract schema coverage test**
   - Validate active durable payload shapes pass required-field checks.

2. **Migration apply/rollback test**
   - Apply new migration set, verify required tables/indexes exist, then rollback and verify clean reapply.

3. **Resolver registration test**
   - Ensure each durable event type resolves to a registered payload resolver/dispatch binding.

4. **Replay/backfill integrity test**
   - Feed representative outbox rows through replay path and assert zero unresolved payload references.

5. **Replay compatibility test**
   - Assert remaining `workflow_event` rows resolve during replay/backfill validation.
