# Stage 3c Startup Compatibility Matrix (Stage 3b Foundations)

Purpose: provide the concrete source-of-truth mapping requested by Stage 3c item 1 so startup can verify that Stage 3b prerequisites are present before vertical-slice orchestration begins.

## Required contracts and concrete sources

| Requirement | Concrete source file(s) | Notes |
|---|---|---|
| `exec_workflow_instance`, `exec_workflow_step`, `exec_workflow_edge`, `exec_workflow_event` tables + workflow indexes | `SavorDb/migration/Execution/202604051200_execution_stage3b_workflow.sql` | Stage 3b execution workflow schema baseline. |
| `ui_workflow_instance`, `ui_workflow_step`, `ui_workflow_edge`, `ui_workflow_alert` + indexes | `SavorDb/migration/UIRead/202604051200_uiread_stage3b_workflow.sql` | Stage 3b visibility baseline for projector targets. |
| Workflow orchestration query/command contracts | `SavorDb/Execution/Workflow/WorkflowOrchestration.h` | Defines query records, command records, and service interfaces. |
| `IExecutionDb` contract for workflow services | `SavorDb/Execution/IExecutionDb.h` | Defines orchestration service accessors used by Stage 3c wiring. |
| Program descriptor decomposition contracts | `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h` | Defines `IJobPersistenceAdapter`, `IRuntimeInitAdapter`, `IResultMapper`, `IWorkflowTransitionHandler`, and `ProgramKindDescriptor`. |
| Program-kind registry abstraction | `SavorDb/Execution/ProgramDB/ProgramKindRegistry.h` | Minimal registry and required-adapter check. |
| Legacy compatibility shim bridge point | `SavorDb/Execution/ProgramDB/ProgramKindLegacyCodecShim.h` + `.cpp` | Allows legacy monolithic codec behavior to be surfaced through decomposed adapters during dual-path operation. |
| Workflow lifecycle event identifiers | `SavorDb/Common/Events/EventCatalog.h` | Includes workflow lifecycle event constants for outbox/projector routing. |
| Startup schema guard for Stage 3b migration floor | `SavorDb/SavorDb.h` + `SavorDb/SavorDb.cpp` (`Stage3cWorkflowSliceReady`) | Checks `Execution` and `UIRead` schema versions are at least `202604051200`. |

## Stage 3c bootstrap guard contract

- Required migration contexts:
  - `Execution >= 202604051200`
  - `UIRead >= 202604051200`
- Startup behavior:
  - fail-fast if migration tracking tables cannot be read,
  - fail-fast if required context version is missing/older,
  - include explicit context/version mismatch in startup error text.

## Follow-on work owned by Stage 3c

This matrix only verifies Stage 3b foundations. It does not imply Stage 3c runtime completeness for:
- concrete repository implementations,
- runner adapter integration,
- terminal callback idempotency handling,
- projector replay parity evidence,
- end-to-end dual-path reconciliation tests.
