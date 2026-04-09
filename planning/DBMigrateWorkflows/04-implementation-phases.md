# 04 - Implementation Phases (Hybrid)

## Phase 0 - Contract and schema prep

- Define additive workflow-input event types.
- Add payload tables and resolvers for new event families.
- Add/update event catalog entries.
- Define migration scripts for any new execution workflow metadata.
- Add/maintain `SimCoreDBValidation` CLI validations for phase-gate checks.

### Exit criteria

- Event contracts documented and reviewed.
- Backward compatibility checklist approved.
- `SimCoreTests` coverage and `SimCoreDBValidation` phase-0 validations both pass.

## Phase 1 - Step input aggregation MVP

- Implement `StepInputAggregationService` in-process with control plane.
- Emit:
  - `step.input_requested`
  - `step.input_fragment_ready`
  - `step.input_complete`
- Add timeout policy with **single retry** before terminal failure event emission.
- Enforce strict all-required fragments.
- Add phase-1 `SimCoreDBValidation` checks for aggregation gating and timeout/retry behavior.

### Exit criteria

- One workflow step can gather from at least two sources (one sync + one async simulation).
- Idempotent duplicate fragment handling verified.
- `SimCoreTests` phase-1 suites and `SimCoreDBValidation` phase-1 checks both pass.

## Phase 2 - Adapter invocation chain

- **Status:** Completed
- **Completion date:** 2026-04-08
- **Verification mode:** Manual verification (`SimCoreTests` + `SimCoreDBValidation` phase-2 checks passed)

- Wire adapter lifecycle:
  1. input complete -> `IJobPersistenceAdapter`
  2. job claimed -> `IRuntimeInitAdapter`
  3. job terminal -> `IResultMapper`
  4. step terminal (event subscriber) -> `IWorkflowTransitionHandler`
- Seed Probe pilot decomposition:
  - split current multi-phase behavior into step-scoped descriptors (`SeedProbe.Neutral`, `SeedProbe.Grid`, `SeedProbe.Unique`)
  - target one-job-set-per-step behavior
- Result-mapping write path:
  - mapper returns typed payloads
  - context-owned writers perform durable writes
- Add phase-2 `SimCoreDBValidation` checks for adapter invocation order and completion gate invariants.

### Exit criteria

- One end-to-end workflow (2 steps) transitions successfully.
- Failed-step semantics tested (FAILED terminal path).
- `SimCoreTests` phase-2 suites and `SimCoreDBValidation` phase-2 checks both pass.

## Phase 3 - Pub/sub extraction and scaling

- **Status:** Completed
- **Completion date:** 2026-04-09
- **Verification mode:** Manual validation (`SimCoreTests` phase-3 suites + `SimCoreDBValidation` phase-3 checks passed)

- Move input providers and completion/transition handlers to independent subscribers where valuable.
- Split coordinator responsibilities into:
  - `WorkflowMaterializationService`
  - `WorkflowDispatchCoordinator`
- Implement in-memory DB claimed-job staging with payload-map lifecycle:
  - claim -> materialize payload -> mark materialized -> dispatch from materialized subset
- Clarify phase-3 ownership boundaries:
  - workflow-step materialization uses `EncodeForQueueing(...)` (step/job-set creation),
  - claimed-job payload materialization uses `BuildRuntimeInit(job_id)` (job runtime init),
  - claim/materialize/dispatch decisions run in coordinator loop and are not step-local.
- Implement claim-time selection heuristics in priority order:
  1. savestate affinity
  2. program/runtime affinity
  3. fairness
- Dispatch loop behavior (resolved):
  - on each coordinator round, iterate every open worker slot and request dispatch with that worker's loaded `savestate_id` hint.
- Tune topic partitions and consumer groups.
- Add lag/dead-letter operational dashboards.
- Add phase-3 `SimCoreDBValidation` checks for replay robustness, dedupe isolation, and stream-separation behavior.

### Exit criteria

- Can handle target throughput with p95 latency SLO.
- Replay and dead-letter playbooks validated.
- `SimCoreTests` phase-3 suites and `SimCoreDBValidation` phase-3 checks both pass.

## Phase 4 - Hardening

- Recovery/reconciliation policy finalization.
- Add replay-safe guards for all side-effect handlers.
- Incident drills: delayed events, duplicated events, out-of-order events.
- Add phase-4 `SimCoreDBValidation` recovery drill checks aligned with runbooks.

### Exit criteria

- Runbook complete.
- On-call checklist complete.
- `SimCoreTests` recovery matrix and `SimCoreDBValidation` hardening checks both pass.

## Consistency checks (run each iteration)

1. **State machine consistency:** every emitted terminal event maps to exactly one orchestration step terminal transition.
2. **Idempotency consistency:** each handler has explicit dedupe key and persistence strategy.
3. **Ownership consistency:** one owner for readiness, one owner for completion, one owner for transition decision.
4. **Schema consistency:** event catalog + payload resolver + migration files are in sync.
5. **Observability consistency:** each stage emits lag and failure metrics.
6. **Granularity consistency:** each workflow step remains single-job-set oriented unless explicitly overridden and documented.
7. **Completion-gate consistency:** step completion and transition decisions only occur after all jobs in the step job set are terminal.

## Implementation questions for this doc

1. None for current iteration.

## Resolved questions

1. Integration mode for first milestone: DB-outbox-only pub/sub.
2. First milestone optimization target: do not optimize for throughput/latency yet; prioritize correctness and working flow.
3. Priority order after MVP: recovery correctness first, then throughput.
4. Minimum recovery test matrix for first milestone:
   - power loss during claimed-job payload materialization,
   - duplicate terminal event replay,
   - partial writer failure (mapper success, writer failure),
   - completion-gate mismatch (`STEP_BLOCKED_COUNT_MISMATCH` path),
   - missing decider-result event on startup (decider rerun path).
