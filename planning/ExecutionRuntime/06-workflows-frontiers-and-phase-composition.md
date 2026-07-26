# 06 - Workflow Boundary and Phase Composition

## Status and authority

**Status:** Authoritative ownership and integration boundary for the Execution Runtime refactor.

This document explains how bounded worker programs meet the existing SavorDb workflow system. It does
not define, schedule, or authorize a SavorDb redesign. Current repository code defines the persisted
workflow, queue, claim, artifact, and domain-storage contracts this refactor must preserve. Separate DB
migration planning sets are orientation for their own projects, not requirements for this refactor.

## Purpose and non-goals

The runtime boundary must let every phase use one `ProgramRuntime` while keeping durable orchestration
outside the worker. Existing SavorDb workflows continue to own:

- job scheduling, claiming, retry, cancellation, and recovery;
- sequential composition and current dynamic-step fan-out;
- persisted workflow steps, edges, bindings, attempts, outputs, and terminal state;
- domain result persistence and transition handling; and
- current transaction, outbox, idempotency, and restart behavior.

A program invocation is a bounded interaction with one emulation session. It may call subprograms, await
registered actions, emit records or artifacts, and return one runtime result. It may not create or claim
jobs, write workflow state, keep durable topology only in worker memory, or decide that its own result
must become another job.

**SavorDb boundary:** this refactor does not change the SavorDb database schema or migrations; stored
job, result, artifact, workflow, affinity, or domain representations; database-service, queue, claim, or
workflow interfaces; transaction boundaries; or workflow/frontier persistence. Existing SavorDb
program-kind handlers and adjacent runtime integration adapters may change only to derive
`ProgramInvocation` from existing persisted data and project `ProgramResult` through existing
persistence and workflow operations.

In this document, **schema** means a runtime program/type schema unless explicitly qualified as a
database schema.

## Current code evidence

The current repository already establishes the integration surface this refactor must preserve:

- `SavorDb/Execution/Workflow/WorkflowOrchestration.h` defines persisted workflow instances, unit
  activations, steps, edges, input bindings, arguments, attempts, output records, and terminal snapshots.
- `SqliteWorkflowOrchestrationCommandService::AppendDynamicSteps` inserts current dynamic children
  transactionally and enforces the existing idempotency rules.
- `WorkflowTerminalAdvancementService` applies terminal results, dynamic successors, output routing, and
  workflow completion/failure through current commands and transactions.
- `WorkflowGraphRoutingService::RouteTerminalStep` applies the existing output and input-binding model.
- `ProgramKindDescriptor` combines job persistence, runtime initialization, result mapping, result
  writing, and transition handling under the current program-kind integration seam.
- Battle Single Turn and SeedProbe already use current transition handlers to create later waves.
- Existing tests cover dynamic insertion, terminal advancement, graph routing, retries, restart, and
  program-kind materialization.

These are migration assets and compatibility requirements, not rewrite targets.

### Implemented production-composition prelude

`SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.*` is now the single production composition
source for the current DB-facing catalog. It registers TAS Movie, SeedProbe, Battle Context, Battle
Single Turn, Battle End, and Navigation Context in that order and validates the canonical numeric and
workflow-step mappings before publishing the registry. SavorQt and all DB-backed SavorE2E scenarios use
the full catalog; scenario-specific runtime paths, TAS settings, and Navigation timeout remain explicit
configuration overrides.

SavorE2E continues to share one `DBService`. Isolation is enforced at scenario and repeat boundaries
through the existing workflow query interface: no Pending or Running workflow, Ready step, active
materialized workflow, or terminal step awaiting reconciliation may cross the boundary. Historical
Completed, Failed, and Canceled records remain in the shared database. This is a harness invariant, not
a coordinator filter or persistence change. `battle_macro_probe` remains direct-worker-only and is not
part of the production DB descriptor catalog.

## Locked target decisions

### Fixed persistence boundary

SavorDb remains the durable owner and its contracts remain unchanged. The Execution Runtime introduces
no new database tables, columns, migrations, persisted runtime-invocation rows, typed workflow-binding
records, frontier records, queue records, or artifact-store interfaces.

The runtime may have richer in-memory types than the current persisted representation. That difference
is resolved at the program-kind integration boundary, not by migrating stored data.

### Program-kind integration seam

`ProgramKind` may continue to:

- identify stored SavorDb jobs;
- select current program-kind handlers;
- participate in current queue, affinity, and claim behavior;
- route current workflow transitions; and
- remain historical, semantic, or UI metadata.

It may not select a worker interpreter, controller class, or worker-side program-specific decoder.

For activation, the current program-kind handler or an adjacent adapter:

1. reads the existing job/domain records through existing interfaces;
2. derives the exact runtime module, entrypoint, runtime type input, and state policy in memory;
3. constructs `ProgramInvocation`; and
4. submits it through the new worker-facing protocol without changing the stored job.

For completion, the handler or adjacent adapter:

1. consumes `ProgramResult`;
2. performs runtime contract validation;
3. projects the domain result and artifact references into the current SavorDb representation; and
4. calls the existing result writers and transition operations.

This is an implementation change inside the existing integration seam. It does not require splitting
`ProgramKindDescriptor` or changing its persistence, workflow, database-service, queue, or claim
interfaces.

### Runtime identity is not stored identity

Every worker activation uses exact runtime module identity, entrypoint, dependency closure, runtime
profile, state policy, and typed input. Those values are constructed at dispatch from existing records
plus compiled or packaged runtime configuration. This refactor does not add them as SavorDb fields.

Existing workflow/job identity, idempotency, attempt, affinity, and lineage representations remain
authoritative for durable behavior.

### Bounded invocation

The runtime invocation ends before a worker lease, workflow restart, user-selection wait, or later job.
`ProgramInstance` is never stored in SavorDb. After worker loss, the existing coordinator and
program-kind handler rematerialize a fresh invocation from the same existing persisted records.

### Existing workflow composition

Phase-to-phase composition continues through the current workflow outputs, bindings, domain references,
artifacts, and transition handlers. Program-kind adapters may validate runtime types and translate them
in memory, but this refactor does not introduce a persisted typed-binding model.

Current dynamic-step fan-out, survivor selection, barriers that already exist, retry, outbox, and
recovery behavior remain unchanged. A program returns bounded results; the existing transition handler
decides whether current SavorDb operations create later work.

### State and session policy

Runtime state policy makes boot, restore, baseline use, and guarded session continuation explicit to the
worker. It does not change how SavorDb stores savestate or artifact references. Program-kind handlers map
the existing references into the runtime state-policy object in memory.

Worker reuse remains an optimization. `ProgramResult` cleanup/session status governs whether the current
worker session may be reused, without requiring a new persisted cleanup-status field.

### Generalized frontiers are separate work

Arbitrary-depth DFS/BFS/best-first frontier storage, node/edge/lease records, persisted deduplication,
typed workflow policies, new barrier models, and new workflow transaction shapes are not part of the
Execution Runtime refactor.

Future navigation, collision, cutscene, or overworld work may require a separately planned and approved
SavorDb project. The Execution Runtime requirement is only that each worker invocation remain bounded and
not own durable topology. A future orchestration project must not require another worker executor.

### Phase switching

An `A -> B -> A` composition continues to use existing workflow operations. Each program-kind handler
constructs the corresponding exact runtime invocation when its existing job activates. Runtime
verification proves that A and B fully unwind their session resources; no new persisted phase-edge or
binding representation is introduced here.

## Interfaces and ownership affected

| Surface | Refactor treatment |
|---|---|
| SavorDb program-kind handler implementations | May translate existing records to `ProgramInvocation` and `ProgramResult` back to existing writes |
| Adjacent worker integration code | May carry the new worker-facing protocol and runtime types |
| `ProgramRuntime` | Verifies and executes one invocation; has no SavorDb or workflow mutation access |
| SavorDb database-service interfaces | Unchanged |
| Queue, claim, affinity, and coordinator persistence contracts | Unchanged |
| Workflow definitions, bindings, transition commands, outbox, and recovery | Unchanged |
| Stored jobs, results, artifacts, domain records, and historical payloads | Unchanged; no conversion |

## Failure and cleanup behavior

- Runtime verification failure rejects worker activation before guest-state mutation.
- Domain results are projected through the existing domain result operations.
- Cleanup/session status is used immediately to retire or rebuild a dirty worker session; it does not
  require a new SavorDb column.
- Existing SavorDb retry, idempotency, terminal advancement, outbox, and recovery behavior remains
  authoritative.
- A failed worker never commits workflow state directly. Only existing SavorDb handlers and commands can
  advance durable work.
- Existing queued jobs remain consumable through the compatibility translation in program-kind handlers;
  they are not rewritten into a new stored invocation format.

## Dependencies and migration implications

Migration must:

1. preserve the current SavorDb schema, migrations, stored representations, services, queues, claims,
   workflow commands, transactions, and artifact interfaces;
2. implement exact runtime invocation/result translation within program-kind handlers or adjacent
   adapters;
3. preserve all current workflow lifecycle, outbox, recovery, fan-out, and transition tests;
4. keep current persisted payload/result codecs where handlers need them to read or write the existing
   representation, without linking those codecs to the legacy worker interpreter;
5. avoid storing `ProgramInstance`, canonical runtime IR, dependency closures, or new runtime-only status
   fields in SavorDb; and
6. treat any generalized workflow/frontier work as a separate project.

Navmesh Survey may use current workflow and dynamic-step facilities where they are sufficient. If its
desired topology requires new persistence or workflow interfaces, that integration is not delivered by
this refactor and must be planned separately.

## Acceptance criteria

- No SavorDb migration or database-schema change is added.
- No SavorDb database-service, queue, claim, affinity, workflow-persistence, transaction, or
  artifact-storage interface changes.
- Existing persisted jobs materialize the correct `ProgramInvocation` through program-kind handlers.
- `ProgramResult` is projected through existing result/domain writers without changing stored
  representations.
- Existing queued jobs and historical payload/result records remain valid; no data conversion is
  required.
- Current workflow restart, retry, idempotency, outbox, dynamic-step, fan-out, survivor-selection, and
  transition tests continue to pass.
- `ProgramRuntime`, actions, reducers, and capability packs cannot access workflow persistence.
- Adding a phase that uses existing runtime capabilities changes only its runtime module/type definitions
  and program-kind integration implementation; it does not change SavorDb storage or interfaces.

## Separate future work

The following are explicitly outside this refactor and require separate planning and approval:

- generalized persisted frontier specifications, nodes, edges, leases, strategies, and deduplication;
- new typed workflow-binding or phase-edge persistence;
- changes to workflow-authoring UI or workflow policy storage;
- persisted module source, canonical IR, dependency catalogs, or verification caches;
- changes to artifact retention, storage backends, or SavorDb artifact interfaces; and
- distributed frontier scheduling or sharding.

## Source references

- `SavorDb/Execution/Workflow/WorkflowOrchestration.h`
- `SavorDb/Execution/Workflow/SqliteWorkflowOrchestration.cpp`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorDb/Execution/Workflow/WorkflowTerminalOutboxSubscriber.cpp`
- `SavorDb/Execution/Workflow/WorkflowGraphRoutingService.cpp`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
- `SavorTests/test_savordb_fixture_sqlite.cpp`
- `SavorTests/test_savordb_phase3_nonfixture.cpp`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
