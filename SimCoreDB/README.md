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
