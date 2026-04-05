# Stage 3b - Workflow Orchestration and Codec Decomposition

## Objective
Transition from trigger-driven, codec-owned phase chaining to explicit workflow-step orchestration while preserving worker execution semantics (`job_set`/`job`) and enabling arbitrary step ordering.

This stage introduces:
- decomposed program codec responsibilities,
- workflow instance/step/edge persistence,
- orchestrator-driven step readiness and materialization,
- compatibility bridge so legacy triggers can coexist during migration.

## Exit Criteria
- Program-kind logic is split into focused interfaces with no new behavior regressions.
- Execution DB can persist workflow instances, steps, and dependency edges.
- At least one real workflow (SeedProbe chain) runs end-to-end using workflow-step transitions.
- Legacy trigger path and workflow-step path can run in controlled dual mode.
- Operators can inspect workflow progress, blocked reasons, and produced `job_set` links.

---

## 3b.0 Migration Principles

1. **Keep `exec_job_set` and `exec_job` as worker scheduling primitives.**
2. **Move transition intent to workflow-step rows** (not `action_kind` codec dispatch).
3. **Use dual-path compatibility first** (legacy triggers + explicit workflow steps) before hard cutover.
4. **Make every transition inspectable and resumable** after crash/restart.
5. **Prefer typed columns over opaque blobs for orchestration state.**

---

## 3b.1 Program Codec Decomposition

### Current Pain Point
A single program codec interface currently combines:
- job persistence,
- runtime init requirements,
- result mapping,
- trigger-time next-phase setup.

### Deliverables
Create focused contracts and a per-program descriptor:

- `IJobPersistenceAdapter`
  - encode/decode job, progress, results persistence.
- `IRuntimeInitAdapter`
  - required savestate resolution and PS init construction.
- `IResultMapper`
  - PR result -> typed result/artifact payload.
- `IWorkflowTransitionHandler`
  - transition policy hook used by orchestrator bridge.

`ProgramKindDescriptor` (registry unit):
- `program_kind`
- pointers/refs to the four adapters above
- optional feature flags/capabilities

### Ordered Tasks
1. Add new interfaces under `SimCore/DB/ProgramDB/`.
2. Add descriptor registry replacing direct monolithic codec lookup.
3. Implement adapter shim from old codec implementations to new contracts.
4. Update worker coordinator call sites to consume focused adapters.
5. Keep legacy interface callable until all usage sites are migrated.

### Verification
- Existing job enqueue/decode/execute path still passes smoke tests.
- Per-program registry resolves all required adapter capabilities.

---

## 3b.2 Execution DB Schema Additions for Workflows

### New Tables

#### 1) `exec_workflow_instance`
- `workflow_instance_id` (PK)
- `workflow_kind` (text)  
  Example: `SEED_PROBE_CHAIN`, `BATTLE_TURN_WAVES`, `DUNGEON_TO_BATTLE`
- `state` (text enum: `PENDING`, `RUNNING`, `COMPLETED`, `FAILED`, `CANCELED`)
- `root_scope_kind` (text enum: `job_set`, `run`, `manual`)
- `root_scope_id` (int nullable)
- `input_ref_kind` (text nullable)
- `input_ref_id` (int nullable)
- `created_by` (text nullable)
- `created_at_utc` (int)
- `started_at_utc` (int nullable)
- `completed_at_utc` (int nullable)
- `failure_code` (text nullable)
- `failure_text` (text nullable)

#### 2) `exec_workflow_step`
- `workflow_step_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `step_key` (text)  
  Stable key within definition (e.g., `SEEDPROBE_NEUTRAL`)
- `step_kind` (text)  
  Runtime handler selector
- `state` (text enum: `WAITING`, `READY`, `MATERIALIZED`, `RUNNING`, `COMPLETED`, `FAILED`, `SKIPPED`)
- `guard_kind` (text nullable)
- `guard_value` (text nullable)
- `priority` (int default 0)
- `attempts` (int default 0)
- `max_attempts` (int default 1)
- `job_set_id` (int nullable; FK -> `exec_job_set.job_set_id`)
- `input_ref_kind` (text nullable)
- `input_ref_id` (int nullable)
- `output_ref_kind` (text nullable)
- `output_ref_id` (int nullable)
- `blocked_reason` (text nullable)
- `ready_at_utc` (int nullable)
- `started_at_utc` (int nullable)
- `completed_at_utc` (int nullable)
- `failed_at_utc` (int nullable)
- `created_at_utc` (int)

#### 3) `exec_workflow_edge`
- `workflow_edge_id` (PK)
- `workflow_instance_id` (FK -> `exec_workflow_instance.workflow_instance_id`)
- `from_step_id` (FK -> `exec_workflow_step.workflow_step_id`)
- `to_step_id` (FK -> `exec_workflow_step.workflow_step_id`)
- `condition_kind` (text nullable)
- `condition_value` (text nullable)
- `created_at_utc` (int)
- UNIQUE(`workflow_instance_id`,`from_step_id`,`to_step_id`)

#### 4) `exec_workflow_event` (optional but recommended)
- `workflow_event_id` (PK)
- `workflow_instance_id` (FK)
- `workflow_step_id` (nullable FK)
- `event_kind` (text)
- `event_ts_utc` (int)
- `message` (text nullable)
- `detail_ref_kind` (text nullable)
- `detail_ref_id` (int nullable)

### Indexes
- `exec_workflow_instance(state, created_at_utc)`
- `exec_workflow_step(workflow_instance_id, state, priority DESC, ready_at_utc ASC)`
- `exec_workflow_step(job_set_id)`
- `exec_workflow_edge(workflow_instance_id, to_step_id)`

### Constraints
- `step_key` unique per workflow instance.
- `job_set_id` unique per workflow step (one materialized execution set per step).

---

## 3b.3 Workflow Definition and Materialization Layer

### Definition Contract
Create typed workflow definitions (C++ or strict JSON schema) with:
- ordered/graph step declarations,
- dependency edges,
- transition guards,
- retry policy,
- materializer binding per `step_kind`.

### Engine Responsibilities
1. Evaluate dependency + guard satisfaction.
2. Promote `WAITING` -> `READY`.
3. Materialize `READY` step into execution work (`exec_job_set` + `exec_job`).
4. Track linkage `workflow_step.job_set_id`.
5. Observe terminal job events and complete/fail step.
6. Advance dependent steps.

### Required Guards (minimum)
- all-jobs-finished in step `job_set`
- all-jobs-succeeded in step `job_set`
- winner threshold met (for battle/selection flows)
- predicate-based branch condition

### Crash Recovery Rules
- On process restart, reconcile any `RUNNING` step with linked `job_set` terminal status.
- Resume from persisted step state; no in-memory-only orchestration state.

---

## 3b.4 Compatibility Bridge and Cutover Strategy

### Dual-Path Modes
- `LegacyOnly`  
  Existing trigger flow only.
- `DualWriteObserve`  
  Legacy triggers still authoritative; workflow rows are written/updated for visibility.
- `WorkflowPrimary`  
  Workflow steps authoritative; legacy triggers optional fallback.
- `WorkflowOnly`  
  Legacy trigger phase-setup disabled for migrated workflow kinds.

### Bridge Behavior
- When legacy trigger would call phase setup, write/advance corresponding workflow step.
- When workflow step materializes work, optionally emit equivalent trigger event for monitoring parity.

### Pilot Workflow
Migrate SeedProbe phase chain first:
1. Neutral
2. Grid
3. Unique
4. Done (+ optional battle schedule)

Then migrate one battle exploration chain after SeedProbe stabilizes.

---

## 3b.5 Event and Projector Extensions

Add execution-side workflow events:
- `Execution.WorkflowInstanceCreated.v1`
- `Execution.WorkflowStepReady.v1`
- `Execution.WorkflowStepMaterialized.v1`
- `Execution.WorkflowStepCompleted.v1`
- `Execution.WorkflowStepFailed.v1`
- `Execution.WorkflowInstanceCompleted.v1`

UI Read model additions:
- `ui_workflow_instance`
- `ui_workflow_step`
- `ui_workflow_edge`
- `ui_workflow_alert` (blocked/failed status)

Projector requirements:
- idempotent by event id,
- stable upsert keys by workflow IDs,
- include blocked reason and linked `job_set`/`job` counts.

---

## 3b.6 Operational Interfaces

Add query APIs for operators/dev tooling:
- list workflow instances by state/date/kind
- fetch full step graph with statuses
- fetch blocked reasons and unresolved dependencies
- fetch mapping from workflow step -> job_set -> jobs

Add command APIs:
- retry failed step
- skip step (if policy allows)
- cancel workflow instance
- resume suspended workflow instance

---

## 3b.7 Testing and Validation Matrix

### Unit Tests
- guard evaluator correctness for all built-in guard kinds
- step readiness resolver with branching and joins
- retry policy transitions (`FAILED` -> `READY`)

### Integration Tests
- SeedProbe chain end-to-end in `DualWriteObserve`
- parity test: legacy trigger outputs == workflow materialization outputs
- crash recovery: restart midway and continue without duplicate materialization
- idempotency: duplicate terminal events do not duplicate step completion

### Data Integrity Tests
- every `MATERIALIZED` step has exactly one `job_set_id`
- every completed workflow instance has all terminal steps terminal
- no dangling edges to missing step rows

### Performance Tests
- readiness scan latency under large step counts
- no regression in claim/dispatch throughput for existing workers

---

## 3b.8 Rollout Checklist

- [ ] Schema migrations applied for workflow tables and indexes.
- [ ] Adapter decomposition merged with compatibility shim.
- [ ] SeedProbe workflow definition and materializer implemented.
- [ ] Dual-path mode defaulted to `DualWriteObserve` in non-prod.
- [ ] Parity dashboards and alerts in place.
- [ ] Promote to `WorkflowPrimary` after parity acceptance window.
- [ ] Remove legacy trigger phase-setup path for migrated workflow kinds.

---


## 3b.9 Handoff to Stage 3c Vertical Slice

Stage 3b should deliver schema + contracts + compatibility hooks sufficient to enable a focused Stage 3c validation slice.

Stage 3b is considered implementation-complete for handoff when:
1. Workflow schema migrations and indexes are merged.
2. Program descriptor/adapters contracts are in place with compatibility shim support.
3. Workflow orchestration query/command contracts are defined for concrete service implementation in Stage 3c.
4. Dual-path mode wiring points exist so Stage 3c can run `DualWriteObserve` with runner integration and parity checks.

Stage 3c then owns real runner integration, end-to-end execution validation, parity diagnostics, and crash-recovery proof for the SeedProbe pilot chain before broad Stage 3 expansion.

---

## 3b.10 Dependencies and Sequencing

- Depends on Stage 2 schema baseline and migration infrastructure.
- Should begin before or alongside Stage 3 event/projector implementation so workflow events are first-class.
- Stage 5 UI cutover should consume workflow read models for orchestration visibility.

Recommended execution order update:
1. Stage 1
2. Stage 2
3. **Stage 3b (this stage)**
4. **Stage 3c (workflow validation vertical slice)**
5. Stage 3
6. Stage 4
7. Stage 5
