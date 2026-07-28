# 06 - Workflow Boundary and Phase Composition

## Scope

This document records how bounded worker programs meet the existing SavorDb workflow system. It does not
define or schedule a SavorDb redesign. Current repository code defines the persisted workflow, queue,
claim, artifact, and domain-storage contracts this refactor must preserve. Separate DB migration
planning sets describe other work and are not requirements for this refactor.

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
persistence and workflow operations. Coordinator grouping, claim use, resident lease/capacity
accounting, and runtime acknowledgements may also change only where the transient workset contract below
requires them.

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

## Core integration constraints

### Fixed persistence boundary

SavorDb remains the durable owner and its contracts remain unchanged. The Execution Runtime introduces
no new database tables, columns, migrations, persisted runtime-invocation rows, typed workflow-binding
records, frontier records, queue records, or artifact-store interfaces.

The runtime may have richer in-memory types than the current persisted representation. That difference
is resolved at the program-kind integration boundary, not by migrating stored data. Workset-specific
coordinator runtime interfaces and bookkeeping may change, but no SavorDb database-service or durable
queue/claim/workflow interface change is planned.

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
4. returns an immutable invocation template plus the exact runtime-only `WorkerWorksetExecutionKey` and
   eligibility facts to the coordinator without changing the stored job.

`SubmitWorkset` is the sole production program-dispatch path: one independent job is a one-item
workset, and compatible independently claimed jobs may share a larger transient `WorkerWorkset` as
described below. The old direct-invocation discriminator remains reserved and rejects before session
mutation. Workset construction is adjacent worker integration behavior, not another responsibility of
the program-kind descriptor or a new persisted program shape.

For completion, the handler or adjacent adapter:

1. consumes `ProgramResult`;
2. performs runtime contract validation;
3. projects the domain result and artifact references into the current SavorDb representation; and
4. calls the existing result writers and transition operations.

This is an implementation change inside the existing integration seam. It does not require splitting
`ProgramKindDescriptor` or changing its persistence, workflow, database-service, queue, or claim
interfaces. Adjacent coordinator/worker runtime interfaces may change only as needed for transient
workset admission, ordered events, resident accounting, and acknowledgements.

Existing battle predicate records and payload fields remain compatibility inputs. The program-kind
handler translates them in memory into predicate-composition inputs; the shared library lowers each
`Check` before module verification, and the resulting module hash covers the generated IR and exact
dependencies. Completion adapters map typed condition summaries into the existing battle result
representation.

Predicate composition is worker-program composition, not workflow composition. SavorDb does not store
lowered predicate IR, router subscriptions, `ProgramInstance`, or `ConditionObservation` records as part
of this refactor.

Existing macro plans/providers and address-program-bearing payloads are also compatibility inputs. The
program-kind adapter translates them in memory into interaction and semantic-observation composition
inputs; those libraries lower finite segments, pure reducers, awaits, typed observations, checks, and
emissions before module verification. SavorDb does not store the lowered IR, semantic-point receipts,
observation baselines, interaction state, input receipts, or composition definitions.

Existing `savor.capture.profile/1` documents remain unchanged opaque inputs. Program-kind adapters pass
the current profile/configuration or artifact reference into runtime activation as they do today;
`CaptureService` preserves the profile's parser and behavior rather than translating its internals into
program IR. It observes routed hits passively, while `StopPointRouter` and `ExecutionEngine` own
wake/control authority. No profile field, storage representation, or artifact interface is added or
migrated.

### Runtime identity is not stored identity

Every worker activation uses exact runtime module identity, entrypoint, dependency closure, runtime
profile, state policy, and typed input. Those values are constructed at dispatch from existing records
plus compiled or packaged runtime configuration. This refactor does not add them as SavorDb fields.

Existing workflow/job identity, idempotency, attempt, affinity, and lineage representations continue to
define durable behavior.

### Transient worker-resident worksets

A `WorkerWorkset` is the finite, static, ordered, bounded, worker-resident, and non-durable production
dispatch envelope over one or more independently durable jobs. It improves locality and amortizes
parent/worker traffic without changing the unit of claim, attempt, result, retry, transition, or
recovery. `ProgramRuntime` sees only the currently activated child and cannot inspect or schedule the
pending items.

Each item retains:

- its existing job, job-set, workflow-step, claim, and attempt identities;
- one immutable invocation template with its own typed input, limits, cancellation identity, and
  provenance; the worker binds its authoritative session and current `StateEpoch` immediately before
  activation;
- one independent invocation root scope and complete unwind;
- one ordinary `ProgramResult`; and
- the current program-kind result mapper and workflow-transition behavior.

The workset itself has only transient request identity, one exact in-memory
`WorkerWorksetExecutionKey`, the
fixed item order, bounded item-count/encoded-byte/aggregate-child-budget/resident-item limits, an
immutable reusable baseline only when it has multiple items, a bounded
unacknowledged-result window, and drain/cancellation state. The key covers exact module, entrypoint,
dependency closure, runtime/session profile, source-state identity/hash/lineage or reusable-baseline
identity, movie-continuation policy, and execution/input/capture/movie/mutation/relevant-service
compatibility. The workset is never a SavorDb row, workflow step, queue record,
claim token, attempt, result, artifact, or recovery object. The worker cannot add items, reorder them,
select later durable work, or inspect SavorDb.

Admission and accounting are fixed:

1. The coordinator claims and reserves a finite compatible set through the existing claim operation,
   preserving stable normal priority/claim order, and materializes each payload independently.
2. A multi-item workset may contain only items whose adapters declare workset eligibility and whose
   exact `WorkerWorksetExecutionKey` matches. Existing persisted affinity remains a scheduling hint; it
   is not sufficient proof of exact compatibility. A one-item workset remains valid even when no
   compatible peer is available.
3. Acceptance by a worker reserves the items while they remain in their existing `CLAIMED` durable
   state. Workset admission does not append `JobStarted` and does not make every resident item
   `RUNNING`.
4. The worker prepares the key's exact source state once. A multi-item workset captures one immutable
   in-memory baseline; a one-item workset skips that unnecessary capture. The first child begins from
   the prepared state; before each later child the worker restores the multi-item baseline, advances
   `StateEpoch` through the existing `StateService` contract, and binds the child's invocation to the
   resulting current epoch.
5. Immediately before that child's first effect, the worker publishes an ordered item-start event and
   activates exactly one `ProgramInvocation`/`ProgramInstance` without waiting for a coordinator
   decision or acknowledgement. The single outbound ordering guarantees that the coordinator consumes
   this event and appends the existing per-job `JobStarted` lifecycle event before it processes that
   child's later terminal.
6. The worker publishes that item's ordinary terminal result as soon as its unwind completes. The
   coordinator immediately validates and projects it through the existing per-job result/artifact
   operations, performs the existing terminal/transition handling, and returns a runtime-protocol item
   acknowledgement. The worker may activate a later resident item without another scheduling decision
   only while the bounded unacknowledged-result window has capacity; otherwise acknowledgement
   backpressure stops admission.
7. The coordinator renews the existing claim lease for every resident nonterminal item, including an
   item accepted by a worker but not yet started.
8. Active, coordinator-buffered, outbound, worker-resident nonterminal, and unacknowledged-terminal
   items all count against bounded coordinator capacity. Claim capacity is derived from the available
   resident capacity of ready workset-capable workers rather than configured slot count alone.
   Unacknowledged terminals are additionally bounded by negotiated count and bytes and may not be
   dropped.

No successful item waits for the whole workset before result projection. A repeated terminal after
transport loss is handled by the current per-job attempt/idempotent-publication rules and then
acknowledged; the workset does not create another completion authority.

### Bounded invocation

Each runtime invocation ends and completes its unwind before a later workset item, workflow restart,
user-selection wait, or later durable phase begins. A resident workset does not merge its items into
one invocation and is not a continuation checkpoint. `ProgramInstance` and workset drain state are
never stored in SavorDb. After worker loss, the existing coordinator and program-kind handler recover
or rematerialize every nonterminal job independently from the same existing persisted records.

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
worker session may be reused, without requiring a new persisted cleanup-status field. Workset locality
does not permit child-invocation-scoped input, router, capture, movie, mutation, pending-action, or
epoch-bound resources to cross an item boundary. The immutable baseline belongs to the workset scope,
not a child invocation; only existing clean session/module caches survive according to their
established contracts. Every child still begins from the exact prepared or restored state declared by
its matching key.

### Generalized frontiers are separate work

Arbitrary-depth DFS/BFS/best-first frontier storage, node/edge/lease records, persisted deduplication,
typed workflow policies, new barrier models, and new workflow transaction shapes are not part of the
Execution Runtime refactor.

Future navigation, collision, cutscene, or overworld work may require SavorDb changes, but those changes
remain outside this refactor. The Execution Runtime requirement is only that each worker invocation
remain bounded and not own durable topology. Follow-on orchestration work must not require another worker
executor.

### Phase switching

An `A -> B -> A` composition continues to use existing workflow operations. Each program-kind handler
constructs the corresponding exact runtime invocation when its existing job activates. Runtime
verification proves that A and B fully unwind their session resources; no new persisted phase-edge or
binding representation is introduced here.

## Interfaces and ownership affected

| Surface | Refactor treatment |
|---|---|
| SavorDb program-kind handler implementations | May translate existing records to `ProgramInvocation` and `ProgramResult` back to existing writes |
| Predicate composition library | Pure module-building facility with no SavorDb, workflow, Dolphin, or session-service access |
| Existing battle predicate storage and codecs | Unchanged; translated in memory at the program-kind boundary |
| Semantic-observation and interaction composition libraries | Pure module-building facilities with no SavorDb, workflow, Dolphin, filesystem, or session-service access |
| Existing macro and address-program representations | Unchanged; translated in memory at the program-kind boundary |
| Existing `savor.capture.profile/1` inputs | Unchanged and consumed opaquely by passive `CaptureService`; no replacement capture language |
| Adjacent worker integration code | May carry the new worker-facing protocol and runtime types |
| `ProgramRuntime` | Verifies and executes one invocation; has no SavorDb or workflow mutation access |
| Coordinator workset scheduling | Uses one `SubmitWorkset` path for singleton or grouped claimed/materialized jobs, renews every resident item lease, and projects/acknowledges each item immediately |
| SavorDb database-service interfaces | Unchanged |
| Queue, claim, affinity, and coordinator persistence contracts | Durable contracts unchanged; only workset-specific grouping, claim use, resident lease/capacity accounting, and runtime acknowledgements change |
| Workflow definitions, bindings, transition commands, outbox, and recovery | Unchanged |
| Stored jobs, results, artifacts, domain records, and historical payloads | Unchanged; no conversion |

## Failure and cleanup behavior

- Runtime verification failure rejects worker activation before guest-state mutation.
- Domain results are projected through the existing domain result operations.
- Cleanup/session status is used immediately to retire or rebuild a dirty worker session; it does not
  require a new SavorDb column.
- Existing SavorDb retry, idempotency, terminal advancement, outbox, and recovery behavior remains
  unchanged.
- Workset admission leaves every not-yet-started item `CLAIMED`. Immediately before effects, the worker
  emits the ordered item-start event without waiting; the coordinator appends `JobStarted` before
  processing that child's later terminal. Each terminal result is projected and acknowledged
  immediately, while a negotiated non-lossy window provides bounded backpressure if acknowledgements
  lag.
- Workset rejection, cancellation, transport loss, worker loss, or session taint returns every
  nonterminal item to the current per-job recovery/requeue path. A tainted worker starts no later
  resident item.
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
3. replace the unactivated scalar production submission with one bounded coordinator/worker
   `SubmitWorkset` path for 1..N independently claimed/materialized jobs, with exact compatibility,
   workset-baseline state flow, resident-capacity and existing-lease accounting, ordered per-item start
   publication mapped to current `JobStarted`, immediate per-item projection/acknowledgement, and
   current requeue/recovery behavior;
4. preserve all current workflow lifecycle, outbox, recovery, fan-out, and transition tests;
5. keep current persisted payload/result codecs where handlers need them to read or write the existing
   representation, without linking those codecs to the legacy worker interpreter;
6. translate current predicate records through the shared in-memory composition path without changing
   their storage or interfaces;
7. translate current macro and address-program inputs through interaction and semantic-observation
   composition without changing their storage or interfaces;
8. pass existing capture profiles to passive `CaptureService` unchanged and preserve their current
   profile-visible behavior;
9. avoid storing `WorkerWorkset`, workset drain/acknowledgement state, `ProgramInstance`, canonical
   runtime IR, lowered predicate/observation/interaction definitions, receipts, baselines, dependency
   closures, or new runtime-only status fields in SavorDb; and
10. keep generalized workflow/frontier work outside this refactor.

Navmesh Survey may use current workflow and dynamic-step facilities where they are sufficient. If its
desired topology requires new persistence or workflow interfaces, that integration is not delivered by
this refactor.

## Boundary checks

- No SavorDb migration or database-schema change is added.
- No SavorDb database-service, durable queue/claim/affinity/workflow-persistence, transaction, or
  artifact-storage interface changes; workset-specific coordinator/worker runtime interfaces and
  bookkeeping may change.
- Existing persisted jobs materialize the correct `ProgramInvocation` through program-kind handlers.
- `ProgramResult` is projected through existing result/domain writers without changing stored
  representations.
- A one-item workset and a multi-item workset produce the same per-job lifecycle, result, artifact,
  transition, idempotency, and retry observations. Workset residency never appears as a new durable
  state.
- Claim budgeting counts coordinator-buffered and worker-resident items, and the existing claim lease is
  renewed for every resident nonterminal item.
- Workset admission does not mark queued resident items started. The worker's ordered item-start event
  precedes effects without a coordinator round trip, and the coordinator appends `JobStarted` before
  processing the ordered terminal. Each result is projected and acknowledged immediately, and the
  negotiated result window bounds any temporary acknowledgement lag.
- Existing queued jobs and historical payload/result records remain valid; no data conversion is
  required.
- Current workflow restart, retry, idempotency, outbox, dynamic-step, fan-out, survivor-selection, and
  transition tests continue to pass.
- `ProgramRuntime`, actions, reducers, and capability packs cannot access workflow persistence.
- Adding predicates to another program changes its module composition, runtime schemas, and existing
  adapter translation only; it adds no predicate-specific persistence, bindings, queues, or claims.
- Adding semantic observations or interactions changes only module composition, exact runtime
  dependencies, and existing adapter translation; it adds no point, observation, baseline, interaction,
  input-receipt, binding, queue, or claim persistence.
- Existing capture profiles retain their identity and representation behind passive `CaptureService`;
  the refactor adds no capture-profile migration or replacement capture-plan storage.
- Adding a phase that uses existing runtime capabilities changes only its runtime module/type definitions
  and program-kind integration implementation; it does not change SavorDb storage or interfaces.

## Out-of-scope future work

The following are outside this refactor and do not gate its completion:

- generalized persisted frontier specifications, nodes, edges, leases, strategies, and deduplication;
- new typed workflow-binding or phase-edge persistence;
- changes to workflow-authoring UI or workflow policy storage;
- persisted module source, canonical IR, dependency catalogs, or verification caches;
- persisted workset identities, membership, drain cursors, acknowledgements, or workset-level attempts
  and results;
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
