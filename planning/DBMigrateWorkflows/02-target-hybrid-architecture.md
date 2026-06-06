# 02 - Target Hybrid Architecture

## Architecture goals

- Keep workflow execution asynchronous and event-driven.
- Preserve existing orchestration and outbox investments.
- Allow RAM-backed in-flight assembly state (non-recoverable in-flight acceptable).
- Keep durable milestones for auditability and replay.

## Core components

## 1. Workflow Control Plane

- Owns workflow run lifecycle and orchestration decisions.
- Supervises per-run/per-step in-memory state contexts.
- Invokes orchestration command service for authoritative step state transitions.

### Workflow composition contract

Workflow composition is a design-time/control-plane concern, not an Analysis DB writer.

- The Qt2 Workflow Builder and SimCoreDB composition registry describe reusable workflow units, their required input ports, their possible output ports, and their internal step kinds.
- Composition validation answers only whether a downstream unit can potentially be satisfied by an upstream unit or by an external input binding.
- Composition does not create Analysis DB, State DB, or UIRead rows.
- Authored records such as battle plans, predicates, and workflow templates should exist before workflow submission. Source artifacts may already exist in State/Archive, but selecting them for a workflow is an instance-specific submission binding.
- Authoring DB owns reusable workflow graph templates as logical graph identities with immutable revisions. A revision contains graph nodes, required input port definitions, possible output port definitions, edges, guards, and optional refs to other authored records.
- External input values are not part of the authored workflow graph. Source artifacts, savestates, prior analysis output refs, and other run-specific bindings are supplied at submission time and belong to the workflow instance/runtime path.
- Execution DB owns submitted workflow instances and only the concrete runtime steps that have actually been instantiated from a specific authored graph revision.
- Program descriptors/adapters own step-specific materialization and result mapping. Any Analysis DB rows needed for a step are created lazily by the descriptor path when that step is materialized or when its results are mapped.
- Runtime transition handling advances downstream steps only when actual produced output refs exist. A unit's possible output is not a promise that the output will be produced.

Example: a battle chain may have a possible terminal savestate output that can feed a dungeon explorer. If no tested battle branch reaches victory, the dungeon explorer remains blocked or skipped and no dungeon-analysis rows are created.

### Current workflow graph direction

The target model is authored graph templates plus per-instance runtime bindings, not static compiled workflows or authored launch payloads.

- Authoring DB stores reusable workflow graph identities and immutable graph revisions.
- Authored graph nodes describe workflow unit kind, required input ports, possible output ports, edges/guards, and optional refs to authored records such as seed-probe specs, TAS specs, battle specs, battle plans, and predicate sets.
- External input values are never stored in the authored graph. Examples include selected DTM artifacts, selected savestates, prior analysis output refs, or any other concrete source artifact chosen for one run.
- Execution DB stores workflow instances, concrete instantiated steps, external input bindings, and instance scalar arguments.
- Instance scalar arguments are values that shape one launch without changing the reusable authored graph. Current examples are TAS RTC value and battle fake-attack min/max overrides.
- Qt2 Workflow Builder is authoring-only. It must not launch workflow instances.
- Qt2 Workflow Launcher is the only UI surface that selects external inputs and instance arguments. Launching a TAS RTC range should create one workflow instance per RTC value.
- Program descriptors and the coordinator materialize concrete execution steps from the authored graph revision plus the workflow instance's bindings and arguments.
- Program descriptors/adapters create Analysis rows lazily when a step actually runs or maps results. The graph validator can prove potential compatibility, but it must not allocate speculative analysis state.
- UIRead is read-only from Qt2. UIRead rows are refreshed only from source-context outboxes and projectors.

Legacy implementation cleanup status follows from this direction:

- Static workflow registries such as `SEED_PROBE_CHAIN` and the static workflow engine/builder/validator path have been removed from the active runtime.
- Remove launcher behavior from authoring/editor surfaces.
- `exec_workflow_input_event`, instance-level `input_ref_kind` / `input_ref_id`, and static initial-input launch paths have been removed from the active schema/runtime. Step-level `input_ref_kind` / `input_ref_id` remains valid for actual descriptor-produced refs.
- Retire remaining `exec_trigger` behavior where it appears.
- Move TAS DTM and RTC range values out of authored TAS specs; selected DTM is an instance input binding and each RTC value is an instance argument.
- Move battle fake-attack ranges out of authored battle specs when they are launch-time exploration bounds.
- Remove hard-coded graph transition names and scenario-only graph ids after descriptor-driven graph instancing is complete.

## 2. Step Input Aggregation Service (new)

- Subscribes to step input requests.
- Requests/collects `*_spec` fragments from source DB adapters.
- Publishes fragment-ready and input-complete events.
- Enforces strict all-required fragment readiness.
- Retries input-timeout collection exactly once before terminal step failure.

### Internal state model (RAM)

```text
StepAssemblyContext
- workflow_instance_id
- workflow_step_id
- required_inputs[]
- fragments_by_source
- pending_requests_by_id
- deadline_utc
- state {COLLECTING|READY|FAILED|TIMED_OUT}
```

## 3. Program Kind Adapter Facade

Per program kind:

- `IJobPersistenceAdapter`
- `IRuntimeInitAdapter`
- `IResultMapper`
- `IWorkflowTransitionHandler`

Control plane invokes adapters in this order:

1. Input complete -> `IJobPersistenceAdapter`
2. Job claimed -> `IRuntimeInitAdapter` (pre-warm)
3. Job terminal -> `IResultMapper`
4. Step terminal -> `IWorkflowTransitionHandler`

Descriptor division of duties:

- `IJobPersistenceAdapter` may create or resolve step-specific Analysis rows required to queue the concrete jobs for that step.
- `IResultMapper` persists produced facts through the owning context DBs after execution has produced real results.
- `IWorkflowTransitionHandler` decides which possible outputs became actual outputs and whether dependent steps can be readied.
- The composition layer never pre-allocates downstream Analysis rows for speculative branches.

## 4. Execution Coordinator / Worker Pool

- Claims queued jobs from execution DB.
- Emits execution lifecycle events.
- Maintains worker availability and optional runtime cache/warming.
- Claim strategy should be aware of worker-local runtime state, including:
  - savestate residency (`savestate_id` currently loaded/pinned in workers),
  - currently loaded program/runtime kind,
  - queue pressure / fairness controls.
- Pre-warm only for claimed jobs (not all queued jobs), and maintain a claimed-job staging queue/cache for fast handoff to workers.
- Claimed-job staging pipeline (resolved):
  1. claim job
  2. `WorkflowMaterializationService` materializes claimed-job payload (`BuildRuntimeInit(job_id)`)
  3. mark payload materialization complete in claimed-job store
  4. `WorkflowDispatchCoordinator` dispatches from **materialized claimed-job subset** only
- Step materialization vs job materialization (resolved):
  - step materialization path uses `IJobPersistenceAdapter::EncodeForQueueing(...)` to enqueue required job(s),
  - job claiming is coordinator-loop-owned and global (not limited to the currently processed workflow step),
  - payload materialization is per claimed job.
- Claimed-job matching priority (resolved): **savestate affinity first**, then **program/runtime affinity**, then **fairness**.
- Dispatch targeting detail (resolved): coordinator iterates all open worker slots each loop and passes worker-loaded `savestate_id` hint into dispatch selection.

## 5. Result Mapping Pipeline

- Subscribes to job terminal events.
- `IResultMapper` performs **two step-specific operations**:
  1. build `ResultINI` from `PRResult` for the current workflow step/program descriptor,
  2. consume that `ResultINI` to apply terminal semantics (state decisions, DB row writes, artifact resolution) for that same step.
- This mirrors legacy `build_results_ini_from_prresult -> encode_results_into_db` boundaries, but now at step-descriptor granularity.
- Context-owned writers persist mapped payloads into source-of-truth analysis/state stores.

## 6. Transition Service (can start in-process)

- Evaluates whether next step should advance.
- Writes next-step readiness through orchestration command path.
- Runs as an **async subscriber** to step-terminal events (not inline in control plane).

## Seed Probe pilot decomposition (first implementation target)

- Use Seed Probe as the first pilot workflow.
- Keep migration case-by-case; do not migrate other workflows until Seed Probe structure validates.
- Prefer composable workflow definitions over hardcoded branching in compiled code.
- Decompose Seed Probe into explicit workflow steps/program kinds:
  1. `SeedProbe.Neutral`
  2. `SeedProbe.Grid`
  3. `SeedProbe.Unique`
- Each step should materialize to one job set in phase-1.

## 7. Observability / Reliability

- Per-stage lag metrics, timeout counters, dead-letter counts.
- Event correlation via `correlation_id` + `causation_id`.

## Flow (high-level)

1. Workflow submitted.
2. Ready step discovered.
3. Step input requested.
4. Fragments collected (sync/async).
5. Input complete -> materialize job set.
6. Jobs for that job set created and queued.
7. Coordinator claims jobs -> materialize job inputs (with cache/staging optimization).
8. Jobs executed.
9. Results mapped.
10. Completion gate verifies all jobs in step job set are terminal.
11. Step terminal marked.
12. Transition evaluated.
13. Next step readied or workflow completed.

## Inconsistencies to resolve

- None for current iteration.

## Resolved inconsistencies

- **Step completion aggregator owner:** control plane.
- **Initial migration scope:** Seed Probe only (case-by-case migration approach; do not generalize other workflows yet).
- **Claimed-job staging model:** in-memory DB used by dispatch coordinator for worker-affinity-aware claim selection.
- **Completion/transition trigger correctness:** step completion and transition decision are gated on terminal completion of all jobs in the step job set.
- **Completion-gate mismatch semantics:** if expected-vs-discovered job counts mismatch, mark step as blocked (`STEP_BLOCKED_COUNT_MISMATCH`), run one reconciliation pass, then terminal-fail with explicit failure code if mismatch persists.

## Implementation questions for this doc

1. None for current iteration.

## Resolved questions

1. Runtime init policy: **on-claim**, with claimed-job staging in **in-memory DB** and initial heuristics limited to:
   - savestate affinity
   - program/runtime affinity
2. Codec decomposition boundary: **ProgramKindDescriptor layer split** for Seed Probe (`Neutral`, `Grid`, `Unique`).
3. Composable definition source direction: step input/output contracts + validator-based graph compatibility and grouped workflows for standard sequences.
4. Result mapping ownership: `IResultMapper` returns payloads to context-owned writers.
5. Seed Probe extraction approach: 3-pass split
   - pass A: isolate neutral-phase interfaces,
   - pass B: split grid/unique dependencies,
   - pass C: enforce descriptor contracts and remove bridging shims.
