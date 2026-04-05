# SimCoreDB

Database-layer scaffolding for SOASim bounded contexts.

## Current scope

- Stage 1/2 migration scaffolding and baseline schemas.
- Stage 3b workflow-orchestration foundations:
  - execution workflow tables and indexes,
  - UIRead workflow visibility tables,
  - archive item-kind catalog entries for workflow export/rehydrate,
  - program-kind descriptor/adapters contracts,
  - legacy codec -> descriptor compatibility shim scaffolding,
  - workflow orchestration query/command contracts,
  - Stage 3c startup readiness guard for required schema versions,
  - workflow mode provider contracts for dual-path runtime selection,
  - event catalog constants including workflow lifecycle events.
- Stage 3c vertical-slice runtime baseline:
  - concrete `ExecutionDb` composition root for `IExecutionDb` workflow services,
  - sqlite-backed workflow query/command services with command precondition checks,
  - lifecycle event emission to `exec_workflow_event` + transactional outbox writes,
  - SeedProbe (`SEED_PROBE_CHAIN`) workflow definition registry + graph validation,
  - deterministic workflow-engine helpers for readiness resolution and startup reconciliation,
  - recovery reconciliation service for in-flight steps bound to terminal `job_set` outcomes,
  - UIRead workflow projector that upserts instance/step/edge visibility rows from execution state,
  - parity-diagnostics utility for comparing legacy trigger outcomes vs workflow outcomes,
  - static mode-provider implementation for runtime mode selection integration.
