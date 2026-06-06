# 01 - Current State and Gap Analysis

## Current state summary

### Existing strengths

1. **Workflow orchestration persistence model exists**
   - `exec_workflow_instance`, `exec_workflow_step`, `exec_workflow_edge`, `exec_workflow_event`.
   - Step lifecycle states are already modeled (`WAITING`, `READY`, `MATERIALIZED`, `RUNNING`, `COMPLETED`, `FAILED`, `SKIPPED`).

2. **Coordinator-driven materialization path exists**
   - `DBWorkflowWorkerCoordinator` polls ready steps, deduplicates, materializes, and can dispatch workers.

3. **Event outbox and relay infrastructure exists**
   - Context outboxes (e.g. `exec_outbox_message`) and generic `OutboxRelay`.
   - Subscription cursors/projection subscriptions in UIRead.

4. **Program-kind descriptor interfaces exist**
   - Adapters for persistence, runtime init, result mapping, and transitions are already formalized.

### Current constraints

1. **Ready-step polling is loop-based**, not yet fully event-triggered.
2. **Workflow events are primarily lifecycle-level**; in-memory input-fragment and step-readiness contracts exist, but they are not persisted through a separate workflow-input-event table.
3. **Cross-context async input gathering** for step materialization is not yet a first-class subsystem.
4. **Transition handling exists by interface, but not yet fully standardized around event contracts.**

## Gap analysis to target hybrid model

## Gap A: Step input aggregation before materialization

**Target:** each workflow step can gather `*_spec` information from multiple sources (sync/async), then materialize jobs once complete.

**Current:** ready-step detection exists, but no general-purpose input fragment collector + readiness gate.

**Action direction:** add a `StepInputAggregationService` + in-memory per-step assembly context with explicit timeout and idempotency behavior.

## Gap B: Program-kind adapters need orchestration-level invocation points

**Target:** the orchestrator invokes `IJobPersistenceAdapter` after input readiness, then `IRuntimeInitAdapter` for coordinator warming, then `IResultMapper`, then `IWorkflowTransitionHandler`.

**Current:** adapter interfaces exist but lifecycle touchpoints are not fully codified end-to-end.

**Action direction:** define canonical invocation sequence and ownership boundaries in coordinator/control-plane code.

## Gap C: Publish/subscribe topic contracts for workflow internals

**Target:** explicit event stream for `StepInputRequested`, `StepInputFragmentReady`, `StepInputComplete`, `JobsEnqueued`, `ResultMapped`, `StepCompleted`, `TransitionRequested`.

**Current:** strong lifecycle event basis exists, but internal workflow-step orchestration events are sparse.

**Action direction:** keep durable Execution outbox contracts focused on lifecycle/terminal authority and source-context projection events. In-memory step-input aggregation may keep internal event names, but new durable launch-input storage should use instance input bindings and arguments.

## Gap D: Operational controls and error semantics

**Target:** clear timeout, retryability classification, fail-fast vs partial-success step semantics, and dead-letter handling.

**Current:** event retry/dead-letter concepts exist in relay; workflow-specific policies need explicit docs/implementation.

**Action direction:** define policy matrix per stage and enforce in coordinator + subscriptions.

## Decisions locked in (iteration 2)

1. **Input assembly key:** use `(workflow_instance_id, step_key)` as the primary in-memory key.
2. **Readiness policy:** strict all-required fragments (`all-inputs-required`) before materialization.
3. **Transition mode:** transition decisions run asynchronously as a subscriber path.
4. **Pilot workflow:** start with Seed Probe workflow, but avoid hardcoding the definition path so composable workflow definitions remain the long-term target.
5. **Claimed-job staging:** use in-memory DB with payload-map lifecycle and affinity-based matching.
6. **Result mapping ownership:** mapper returns payloads to context-owned writers.
7. **Dedupe persistence:** dedicated dedupe tables per service.
8. **Completion trigger invariant:** only mark step complete after all jobs in step job set are terminal.
9. **Workflow composition ownership:** composition validates required inputs against possible outputs, while program descriptors/adapters lazily create Analysis rows only when a step actually materializes or maps real results.

## Seed Probe pilot guidance

- The current Seed Probe flow is a useful pilot reference, but should be decomposed into granular workflow steps and program kinds:
  - `SeedProbe.Neutral`
  - `SeedProbe.Grid`
  - `SeedProbe.Unique`
- Each of these should be treated as a single-job-set workflow step to preserve clear dependencies and transition boundaries.
- This implies we should split portions of the existing codec/DB codec behavior into separate `ProgramKindDescriptor` implementations.

## Inconsistencies to resolve

- The existing coordinator can both materialize and dispatch workers; this is now resolved toward a split architecture and we should plan explicit service boundaries for:
  - `WorkflowMaterializationService` (step -> job set/job rows)
  - `WorkflowDispatchCoordinator` (claim strategy, pre-warm, dispatch)
- Earlier plans experimented with a separate `workflow_input_event` payload family. That table/resolver path has been removed from the active schema/runtime; launch input state belongs to instance bindings and scalar arguments.

## Resolved inconsistencies

1. **Payload family migration ambiguity resolved**:
   - keep `workflow_event` as the durable workflow lifecycle payload family,
   - do not reintroduce `workflow_input_event` for launch inputs,
   - validate replay/backfill against remaining durable payload refs.
2. **Completion invariant remediation resolved**:
   - on invariant violation, pause workflow instance,
   - emit invariant-violation event,
   - run reconciliation service,
   - either reopen step or terminal-fail with explicit failure code.
