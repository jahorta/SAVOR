# 04 - Implementation Phases (Hybrid)

## Phase 0 - Contract and schema prep

- Define additive workflow-input event types.
- Add payload tables and resolvers for new event families.
- Add/update event catalog entries.
- Define migration scripts for any new execution workflow metadata.

### Exit criteria

- Event contracts documented and reviewed.
- Backward compatibility checklist approved.

## Phase 1 - Step input aggregation MVP

- Implement `StepInputAggregationService` in-process with control plane.
- Emit:
  - `step.input_requested`
  - `step.input_fragment_ready`
  - `step.input_complete`
- Add timeout policy with **single retry** before terminal failure event emission.
- Enforce strict all-required fragments.

### Exit criteria

- One workflow step can gather from at least two sources (one sync + one async simulation).
- Idempotent duplicate fragment handling verified.

## Phase 2 - Adapter invocation chain

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

### Exit criteria

- One end-to-end workflow (2 steps) transitions successfully.
- Failed-step semantics tested (FAILED terminal path).

## Phase 3 - Pub/sub extraction and scaling

- Move input providers and completion/transition handlers to independent subscribers where valuable.
- Split coordinator responsibilities into:
  - `WorkflowMaterializationService`
  - `WorkflowDispatchCoordinator`
- Implement in-memory DB claimed-job staging with payload-map lifecycle:
  - claim -> materialize payload -> mark materialized -> dispatch from materialized subset
- Implement claim-time selection heuristics in priority order:
  1. savestate affinity
  2. program/runtime affinity
  3. fairness
- Tune topic partitions and consumer groups.
- Add lag/dead-letter operational dashboards.

### Exit criteria

- Can handle target throughput with p95 latency SLO.
- Replay and dead-letter playbooks validated.

## Phase 4 - Hardening

- Recovery/reconciliation policy finalization.
- Add replay-safe guards for all side-effect handlers.
- Incident drills: delayed events, duplicated events, out-of-order events.

### Exit criteria

- Runbook complete.
- On-call checklist complete.

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
