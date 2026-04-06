# 05 - Open Questions (Need Answers Before Locking Design)

This file is meant to be actively updated each iteration.

## Resolved in this iteration

1. **Step readiness semantics**
   - Decision: all-inputs-required (no partial threshold in phase-1).
2. **Transition ownership**
   - Decision: `IWorkflowTransitionHandler` invoked asynchronously by subscriber path.
3. **Materialization cardinality**
   - Decision: design each workflow step to be granular and effectively one job set per step.
4. **Failure policy**
   - Decision: on input timeout, retry once before terminal failure.
5. **Payload family**
   - Decision: use new `workflow_input_event` payload family for workflow input orchestration events.
6. **Step aggregate identity**
   - Decision: encode `workflow_step_id` as aggregate id string for step-scoped events.
7. **Seed Probe split boundary**
   - Decision: split at `ProgramKindDescriptor` layer.
8. **Claimed-job staging model**
   - Decision: in-memory DB + underlying payload map lifecycle:
     claim -> materialize payload -> mark materialized -> dispatch from materialized claimed-job subset.
9. **Claim heuristics**
   - Decision priority: savestate affinity first, then program/runtime affinity, then fairness.
10. **Result mapping ownership**
   - Decision: `IResultMapper` sends payloads to context-owned writers.
11. **Dedupe persistence**
   - Decision: dedupe keys live in dedicated tables per service.
12. **Step completion trigger**
   - Decision: completion/transition decision triggered only when all jobs in step job set are terminal; target invariant is no job terminal events after step completion.
13. **Completion-gate mismatch semantics**
   - Decision: mismatch -> block with `STEP_BLOCKED_COUNT_MISMATCH`, run one reconciliation pass, then terminal-fail if still mismatched.
14. **Seed Probe extraction method**
   - Decision: use 3-pass split (neutral interfaces, grid/unique dependency split, descriptor contract enforcement).
15. **Decider evidence retrieval mode (milestone 1)**
   - Decision: DB-query contracts only; event-snapshot evidence deferred.
16. **Recovery test baseline**
   - Decision: adopt minimum 5-scenario recovery matrix (power loss, duplicate terminal replay, partial writer failure, completion mismatch, missing decision-result replay).

## Priority A (blockers)

1. **Seed Probe decomposition scope**
   - Start split from `SimCore/DB/ProgramDB/SeedProbeDBCodec.h/.cpp` to produce `SeedProbe.Neutral`, `SeedProbe.Grid`, `SeedProbe.Unique` descriptors.
   - The module is currently monolithic/intertwined; likely requires manual-assisted extraction and review checkpoints.
   - Compatibility with older persisted formats is not a phase-1 requirement (fresh structure rollout).

2. **Composable workflow definition source**
   - Definitions source is **code registry** for now.
   - Add a dedicated validation service to gate workflow definitions before use.
   - We should add per-step contracts (`required_inputs`, `provided_outputs`) and validator rules to ensure downstream requirements are satisfiable by prior outputs.
   - We should support grouped workflows for common sequential pipelines (e.g., grouped Seed Probe workflow).

## Priority B (important)

3. **Claimed-job staging implementation details**
   - What exact schema/index layout should the in-memory DB use for fast matching + dispatch selection?
   - What telemetry do we require from staged jobs (materialization latency, stale claims, dispatch miss rate)?

4. **Context writer contracts**
   - What should the stable contract be between `IResultMapper` output payloads and context-owned writers?
   - Do we require versioned writer contracts from phase-1?

5. **Dedupe table lifecycle**
   - What retention/cleanup strategy should each service use for its dedicated dedupe table?

## Priority C (later but define soon)

6. **Out-of-order tolerance policy**
   - No additional out-of-order policy question for milestone 1 (resolved to repair-job path). Track only operational tuning once implemented.

7. **Observability baseline**
    - Which metrics are required for phase-1 go/no-go?

## Suggested answer format

For each question:

- **Decision:** ...
- **Owner:** ...
- **Date:** ...
- **Follow-up action:** ...

## Known contradictions to resolve next iteration

- Split direction is chosen; remaining question is implementation boundary timing (Phase 2 vs Phase 3) and migration safety checks.
- For one-job-set-per-step guidance, what is the explicit exception policy (if any) and where is it enforced?

## Resolved inconsistencies

- Definitions source ambiguity is resolved for initial rollout: use code registry + validation service.
- Seed Probe split entry point module is identified (`SimCore/DB/ProgramDB/SeedProbeDBCodec.h/.cpp`), reducing uncertainty on where decomposition starts.
- Claimed-job ownership path and matching order are now explicitly defined.
- Result mapping ownership and dedupe persistence strategy are now explicitly defined.
- Decider evidence contract is standardized (`evidence_ref`) and milestone retrieval mode is DB-query-only.
- Completion invariant remediation path is standardized through pause + invariant event + reconciliation/repair + reopen-or-fail policy.
