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
