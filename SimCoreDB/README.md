# SimCoreDB

Database-layer scaffolding for SOASim bounded contexts.

## Current scope

- Stage 1/2 migration scaffolding and baseline schemas.
- Stage 3b workflow-orchestration foundations:
  - execution workflow tables and indexes,
  - UIRead workflow visibility tables,
  - archive item-kind catalog entries for workflow export/rehydrate,
  - program-kind descriptor/adapters contracts,
  - descriptor-owned materialization/result mapping replacing legacy codec shims,
  - workflow orchestration query/command contracts,
  - Stage 3c startup readiness guard for required schema versions,
  - workflow mode provider contracts for dual-path runtime selection,
  - event catalog constants including workflow lifecycle events.
- Stage 3c vertical-slice runtime baseline:
  - concrete `SqliteExecutionDb` composition root for `IExecutionDb` workflow services,
  - sqlite-backed workflow query/command services with command precondition checks,
  - lifecycle event emission to `exec_workflow_event` + transactional outbox writes,
  - Authoring-owned workflow graph storage and graph validation,
  - Execution-owned workflow instance external input bindings and scalar instance arguments,
  - deterministic workflow-engine helpers for readiness resolution and startup reconciliation,
  - recovery reconciliation service for in-flight steps bound to terminal `job_set` outcomes,
  - UIRead workflow projector that upserts instance/step/edge visibility rows from execution state,
  - outbox-driven workflow projector replay with checkpointing for idempotent catch-up,
  - parity-diagnostics utility for comparing legacy trigger outcomes vs workflow outcomes,
  - promotion-gate decision helper for go/no-go artifact generation,
  - static mode-provider implementation for runtime mode selection integration.

## Current direction

- Authoring DB owns reusable workflow graph identities and immutable revisions.
- Authored graph revisions store graph shape, node contracts, edges, guards, and refs to reusable authored records only.
- External input values are instance-specific and live in Execution DB `exec_workflow_instance_input_binding` rows.
- Scalar launch choices are instance-specific and live in Execution DB `exec_workflow_instance_argument` rows. Current examples include TAS RTC and battle fake-attack bounds.
- Qt2 workflow authoring must not launch workflow instances. A separate launcher selects external inputs and instance arguments, then submits Execution workflow instances.
- Program descriptors/adapters own lazy Analysis row creation during step materialization or result mapping. Authored graph composition must not pre-create speculative Analysis rows.
- Qt2 treats UIRead as read-only. UIRead updates come from source-context outboxes and projectors.
- Static workflow registries, trigger-driven launch paths, `workflow_input_event` as launch-input storage, and instance-level `input_ref_kind` / `input_ref_id` bootstrap paths are legacy cleanup targets.
