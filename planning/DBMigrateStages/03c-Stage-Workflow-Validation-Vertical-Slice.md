# Stage 3c - Workflow Validation Vertical Slice

## Objective
Implement and validate a real end-to-end workflow vertical slice between Stage 3b foundations and Stage 3 broad event/projector rollout.

This stage proves that workflow orchestration works with real worker execution by wiring the new SimCoreDB interfaces into runner scheduling paths, while preserving a dual-path fallback.

## Exit Criteria
- SeedProbe workflow chain runs end-to-end in `DualWriteObserve` mode with real worker execution.
- Workflow state is inspectable through query/command services and UIRead workflow projections.
- Legacy trigger outputs and workflow outputs pass parity checks for the pilot chain.
- Crash/restart resume works without duplicate workflow-step materialization.

---

## Ordered Implementation List

1. Confirm Stage 3b schema contracts are present and stable (`exec_workflow_*`, `ui_workflow_*`, workflow query/command contracts).
   - Review merged migration files and interface headers, then create a short compatibility matrix mapping each required table/column/enum/API to its concrete source file.
   - Add a startup guard in the vertical-slice bootstrap that checks schema version and fails fast if Stage 3b migrations are missing.
   - Current matrix artifact: `03c-Stage3b-Compatibility-Matrix.md` (keep updated as interfaces evolve).

2. Implement concrete `IExecutionDb` wiring for workflow orchestration query/command services.
   - Create a concrete `ExecutionDb` composition root that owns query and command service implementations and exposes them through `IExecutionDb`.
   - Route database handle/transaction lifetimes through one shared execution DB context to avoid split-transaction bugs.

3. Implement workflow persistence repositories/services for instance, step, edge, and workflow event rows.
   - Add repository methods for insert/update/get/list covering state transitions and key lookups by `workflow_instance_id` and `step_key`.
   - Enforce uniqueness and transition invariants in write methods (single `job_set_id` per step, no edge rows for missing steps).

4. Implement workflow definition registry for `SEED_PROBE_CHAIN` with ordered steps (`Neutral`, `Grid`, `Unique`, `Done`).
   - Add a typed in-process definition object for each step containing `step_key`, `step_kind`, dependencies, guards, and retry policy.
   - Register definitions at startup and validate graph shape (no duplicate step keys, no orphan dependencies, no cycles).

5. Implement workflow engine components for readiness resolution, guard evaluation, step materialization, terminal handling, and dependency advancement.
   - Build the engine as deterministic pure logic over persisted state snapshots, then persist resulting transitions in an explicit transaction boundary.
   - Keep guard evaluation isolated in its own module so each guard can be unit-tested independently.

6. Implement startup/recovery reconciliation for in-flight workflow steps bound to existing `job_set` terminal state.
   - On process start, scan non-terminal workflow instances and reconcile steps in `RUNNING`/`MATERIALIZED` against `job_set` status.
   - Record reconciliation actions in workflow event rows so operators can audit recovery decisions.

7. Add runner integration path under `SimCore/Runner/Parallel/SimCoreDB/` that adapts workflow materialization to worker scheduling primitives.
   - Create an adapter layer that translates `READY` workflow steps into existing `job_set`/`job` scheduling operations without changing worker execution semantics.
   - Keep runner-facing payload shapes aligned with existing worker coordinator expectations for incremental rollout safety.

8. Add a new coordinator integration header in that folder that adapts or replaces `DBWorkerCoordinator.h` usage with SimCoreDB interfaces and workflow scheduling hooks.
   - Introduce a coordinator-facing interface that accepts workflow materialization callbacks and terminal status callbacks.
   - Gate this integration behind a feature flag so the legacy coordinator path can still be selected during fallback.

9. Keep legacy trigger path available and add dual-path mode control (`LegacyOnly`, `DualWriteObserve`, `WorkflowPrimary`, `WorkflowOnly`) with `DualWriteObserve` default for non-prod.
   - Centralize mode selection in one runtime config source and log the active mode at startup.
   - In dual mode, run legacy trigger actions as authoritative while writing equivalent workflow state transitions for parity observation.

10. Implement terminal job/job_set bridge callbacks so workflow steps complete/fail on real execution outcomes.
    - Subscribe to terminal job/job_set transitions in the execution path and map outcomes to workflow step transitions.
    - Make callback handlers idempotent so duplicate terminal notifications cannot double-complete a step.

11. Emit workflow lifecycle events (`WorkflowInstanceCreated`, `WorkflowStepReady`, `WorkflowStepMaterialized`, `WorkflowStepCompleted`, `WorkflowStepFailed`, `WorkflowInstanceCompleted`).
    - Emit events from the same transaction context as state transitions using an outbox write to preserve atomicity.
    - Include stable correlation/causation metadata so downstream parity diagnostics can group full chains.

12. Implement minimum Stage 3 relay/projector coverage required for workflow visibility (`WorkflowProjector` writing `ui_workflow_instance`, `ui_workflow_step`, `ui_workflow_edge`, `ui_workflow_alert`).
    - Add projector handlers for each workflow lifecycle event and use upsert semantics keyed by workflow IDs.
    - Update projector checkpointing so replaying the same events is safe and produces identical read models.

13. Implement operator/dev interfaces for list/get graph/blocked steps/step-to-job mapping and command interfaces for retry/skip/cancel/resume.
    - Expose query APIs with filters for workflow kind/state/date range and return consistent pagination/sorting behavior.
    - Validate command preconditions (e.g., skip policy allowed, retry only failed step) and return structured error codes/messages.

14. Add parity diagnostics comparing legacy trigger outcomes against workflow-materialized outcomes for SeedProbe pilot runs.
    - Persist parity comparison rows keyed by run/workflow identifiers and include mismatch category fields.
    - Build a simple report that summarizes parity pass rate and lists mismatches by step key and outcome type.

15. Execute integration tests for dual-path SeedProbe chain end-to-end, crash recovery, and duplicate terminal event idempotency.
    - Create repeatable test fixtures for SeedProbe inputs and run them in both legacy and dual modes.
    - Add restart-in-the-middle test cases where the process exits between materialization and terminal completion.

16. Execute data integrity checks for single materialized `job_set` per step, complete terminal closure, and no dangling edges.
    - Add SQL integrity queries as automated checks in CI/dev scripts.
    - Run checks after each integration test scenario and fail the pipeline on any invariant violation.

17. Document observed bottlenecks and readiness scan latency before advancing to broader Stage 3 event/projector scope.
    - Capture per-cycle orchestration timings and queue depths in logs/metrics for each test run.
    - Summarize top bottlenecks and proposed mitigations in a short stage notes document.

18. Record go/no-go recommendation for moving SeedProbe from `DualWriteObserve` to `WorkflowPrimary`.
    - Define promotion thresholds (parity %, crash recovery success, integrity checks) before evaluation.
    - Produce a decision record with approver names, date, evidence links, and explicit rollback trigger conditions.

---


## SimCoreTests Validation Plan (Required for Stage 3c)

1. **Item 1 (Stage 3b contract verification)** — *Integration schema contract test*: in `SimCoreTests`, open a migrated execution/UIRead test DB and assert required workflow tables/columns/indexes and enum-domain values exist before running orchestration tests.
2. **Item 2 (IExecutionDb wiring)** — *Construction + smoke integration test*: instantiate the concrete `IExecutionDb` implementation from test composition root and assert query/command service pointers resolve and perform a minimal read/write roundtrip.
3. **Item 3 (workflow persistence repos/services)** — *Repository integration test*: insert/update/list workflow instance/step/edge/event rows and assert constraints/invariants (including duplicate key and invalid FK behavior) using transactional rollback fixtures.
4. **Item 4 (workflow definition registry)** — *Unit test with registry fixtures*: register valid and invalid `SEED_PROBE_CHAIN` definitions and assert validation catches duplicate step keys, missing deps, and cycle attempts.
5. **Item 5 (workflow engine core)** — *Deterministic engine unit tests*: feed persisted-state fixtures into readiness/guard/materialization/advance logic and assert exact state transition outputs for each scenario.
6. **Item 6 (startup/recovery reconciliation)** — *Crash-recovery integration test*: pre-seed DB with in-flight workflow rows + terminal/non-terminal `job_set` states, run recovery pass, and assert reconciled states and emitted workflow recovery events.
7. **Item 7 (runner integration adapter path)** — *Coordinator-adapter integration test*: use test doubles around worker scheduling boundary to verify `READY` steps create expected `job_set`/`job` payloads and preserve existing scheduling semantics.
8. **Item 8 (new coordinator integration header/path)** — *Compile-time + behavior test*: build SimCoreTests target with new coordinator integration selected and run behavior tests proving terminal callbacks and materialization callbacks are invoked in expected order.
9. **Item 9 (dual-path mode control)** — *Mode matrix integration test*: run identical SeedProbe fixtures in `LegacyOnly`, `DualWriteObserve`, `WorkflowPrimary`, and `WorkflowOnly` and assert configured-authority behavior plus workflow row parity outputs.
10. **Item 10 (terminal bridge callbacks)** — *Idempotency integration test*: publish duplicate terminal completion/failure signals and assert workflow step terminalization occurs exactly once with stable timestamps/outcomes.
11. **Item 11 (workflow lifecycle events)** — *Outbox integration test*: execute transitions and assert the correct event set, ordering, correlation/causation metadata, and no missing/extra lifecycle events.
12. **Item 12 (minimum relay/projector coverage)** — *Projector replay test*: feed workflow events through relay/projector in SimCoreTests, then replay same stream and assert `ui_workflow_*` rows are identical and checkpoints advance idempotently.
13. **Item 13 (operator/dev interfaces)** — *API contract integration test*: test list/get/blocked/map query outputs and retry/skip/cancel/resume command preconditions, including structured error behavior for invalid commands.
14. **Item 14 (parity diagnostics)** — *Comparative integration test*: execute paired legacy vs workflow runs and assert parity diagnostic rows classify matches/mismatches by step key and mismatch category.
15. **Item 15 (end-to-end dual-path scenarios)** — *End-to-end integration suite*: run SeedProbe chain to completion in dual mode, include controlled mid-run restart, and assert final workflow/job states and emitted events are correct.
16. **Item 16 (data integrity checks)** — *Post-run integrity SQL tests*: after each scenario, run integrity queries from SimCoreTests asserting one `job_set` per materialized step, terminal closure completeness, and no dangling edges.
17. **Item 17 (performance/bottleneck evidence)** — *Performance integration benchmark test*: measure orchestration readiness scan latency and queue depth metrics under seeded large workflow graphs and assert against agreed thresholds.
18. **Item 18 (go/no-go recommendation evidence)** — *Gate aggregation test/report*: in SimCoreTests, aggregate parity %, recovery pass/fail, integrity pass/fail, and performance results into a machine-readable decision artifact for promotion review.

---

## Dependencies and Sequencing

- Depends on Stage 3b foundations being merged.
- Must complete before Stage 3 is expanded to the full event catalog/projector surface area.
- Produces validated integration patterns reused by Stage 3, Stage 4, and Stage 5.

Recommended execution order update:
1. Stage 1
2. Stage 2
3. Stage 3b
4. **Stage 3c (this stage)**
5. Stage 3
6. Stage 4
7. Stage 5
