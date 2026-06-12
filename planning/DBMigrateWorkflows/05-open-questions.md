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
   - Decision: `workflow_input_event` was a phase-0 input-orchestration event family, but it is no longer part of the active schema/runtime. External launch inputs use typed `exec_workflow_instance_input_binding` rows and scalar launch choices use `exec_workflow_instance_argument` rows.
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
   - Decision: `IResultMapper` is step-specific and must support legacy-like boundaries:
     - build `ResultINI` from `PRResult`,
     - consume that same `ResultINI` for terminal semantics,
     - then persist via mapper-owned writes or context-owned writer depending on migration mode.
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
17. **Step materialization vs payload materialization boundary**
   - Decision: workflow-step materialization enqueues jobs via `EncodeForQueueing(...)`; claimed-job payload materialization is a separate stage via `BuildRuntimeInit(job_id)`.
18. **Worker-aware dispatch targeting**
   - Decision: coordinator dispatches per open worker slot and passes worker-loaded `savestate_id` affinity hint into dispatch selection.
19. **Claim scope**
   - Decision: claiming is global across in-flight workflows and not restricted to the currently dequeued workflow step.
20. **Workflow composition output semantics**
   - Decision: composition contracts use `possible_outputs`, not guaranteed/provided outputs. Downstream compatibility is design-time only; runtime advancement requires actual produced refs.
21. **Analysis row creation ownership**
   - Decision: Qt2/workflow composition must not pre-create Analysis DB rows. Program descriptors/adapters create Analysis rows lazily during materialization or result mapping for steps that actually run.
22. **Workflow graph storage boundary**
   - Decision: reusable workflow graph templates live in Authoring DB as logical graph identities with immutable revisions. External input values are instance-specific and must not be stored in authored workflow graphs.
23. **Workflow launch boundary**
   - Decision: Qt2 Workflow Builder is authoring-only. Workflow instances are launched only from a separate launcher surface that selects external inputs and instance arguments.
24. **Instance argument ownership**
   - Decision: scalar launch values that vary per run live in Execution DB as workflow instance arguments, not in authored specs or graphs. Current examples are TAS RTC value and battle fake-attack min/max.
25. **RTC fan-out**
   - Decision: a TAS RTC range selected at launch produces one workflow instance per RTC value, so each instance has one concrete RTC argument.

## Priority A (blockers)

1. **Seed Probe decomposition scope**
   - Start split from `SavorCore/DB/ProgramDB/SeedProbeDBCodec.h/.cpp` to produce `SeedProbe.Neutral`, `SeedProbe.Grid`, `SeedProbe.Unique` descriptors.
   - The module is currently monolithic/intertwined; likely requires manual-assisted extraction and review checkpoints.
   - Compatibility with older persisted formats is not a phase-1 requirement (fresh structure rollout).

2. **Composable workflow definition source**
   - Definitions source is Authoring-owned workflow graph templates.
   - Keep code descriptors as the source for program kind contracts and materialization behavior.
   - Use a dedicated validation service to gate authored graph revisions before use.
   - Per-node contracts (`required_inputs`, `possible_outputs`) and validator rules ensure downstream requirements are potentially satisfiable by prior possible outputs or external input bindings.
   - Grouped workflows for common sequential pipelines are authored graph templates, not separate static workflow definitions.

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

8. **Future savestate-output workflow fanout**
   - Future work: when a workflow step produces one or more savestates and an authored edge consumes that savestate in another workflow step, create one child workflow instance per produced savestate instead of routing all outputs inside the same workflow instance.
   - Persist provenance from source workflow instance, source workflow step, source job output, and savestate id to each child workflow instance.
   - This is intentionally out of scope for the current TAS RTC cleanup, which only fans out launch-time TAS RTC ranges.

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

- Definitions source ambiguity is resolved for the target architecture: use Authoring-owned workflow graph templates + descriptor-provided program contracts + validation service.
- Seed Probe split entry point module is identified (`SavorCore/DB/ProgramDB/SeedProbeDBCodec.h/.cpp`), reducing uncertainty on where decomposition starts.
- Claimed-job ownership path and matching order are now explicitly defined.
- Result mapping ownership and dedupe persistence strategy are now explicitly defined.
- Decider evidence contract is standardized (`evidence_ref`) and milestone retrieval mode is DB-query-only.
- Completion invariant remediation path is standardized through pause + invariant event + reconciliation/repair + reopen-or-fail policy.
- Workflow composition output semantics are standardized around `possible_outputs`, and Analysis rows are created lazily by program descriptors/adapters instead of by the composition layer.
- Workflow graph storage is standardized around Authoring-owned graph identities, immutable graph revisions, and instance-specific submission bindings.
- Workflow launch ownership is standardized around a separate launcher surface. Authoring editors create reusable records only; launch-time external inputs and scalar arguments are Execution-owned instance data.
- Compatibility with removed early workflow structures is not required for this cleanup pass; prefer deleting stale schema/code paths over carrying adapters for `workflow_input_event` or instance-level bootstrap refs.
