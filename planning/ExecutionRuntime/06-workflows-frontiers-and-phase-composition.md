# 06 - Workflow Boundary and Phase Composition

## Scope

This document records how bounded worker programs meet the existing SavorDb workflow system. It does not
define or schedule a SavorDb redesign. Current repository code defines the persisted workflow, queue,
claim, artifact, and domain-storage contracts this refactor must preserve. Separate DB migration
planning sets describe other work and are not requirements for this refactor. The pre-6A timing cutover
changes public authoring DTOs and generated runtime arguments, but not migrations or DDL.

## Purpose and non-goals

The runtime boundary must let every phase use one `ProgramRuntime` while keeping durable orchestration
outside the worker. Existing SavorDb workflows continue to own:

- job scheduling, claiming, retry, cancellation, and recovery;
- sequential composition and current dynamic-step fan-out;
- persisted workflow steps, edges, bindings, attempts, outputs, and terminal state;
- domain result persistence and transition handling; and
- current transaction, outbox, idempotency, and restart behavior.

A program invocation is a structurally bounded interaction with one emulation session. It may call subprograms, await
registered actions, emit records or artifacts, and return one runtime result. It may not create or claim
jobs, write workflow state, keep durable topology only in worker memory, or decide that its own result
must become another job.

**SavorDb boundary:** this refactor does not change the SavorDb database schema or migrations; durable
job, claim, lease, result, artifact, workflow, affinity, or domain lifecycle semantics;
result-projection transaction boundaries; artifact interfaces; or workflow/frontier persistence.
Obsolete authoring timing fields and newly generated timing arguments are intentionally removed. The
six physical authoring timing columns remain ignored; three private insert paths write neutral `0,0`
values until a separate database migration removes the columns and shims. Existing program-kind
handlers and adjacent adapters derive timing-free runtime inputs/results in memory. Narrow
execution-facing database-service interfaces may additionally be added or adapted only
for real bounded batch claim/reservation, exact-set lease renewal, claim/start authority validation, and
targeted advancement of an exact known terminal. Those operations use the same existing rows, commands,
idempotency rules, and per-item semantics. They do not persist a workset, completion ledger, capacity
credit, active-workset baseline handle, staged package, or new lifecycle state.

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
- `IExecutionDb::ClaimBatchReadyExecutionJobs` and `JobMaterializationService::ClaimJobsDetailed`
  already provide a batch-shaped starting point, but the current SQLite implementation loops scalar
  claims and transactions; current lease maintenance likewise calls `RenewExecutionJobLease` per item.
- `WorkflowTerminalAdvancementService::AdvanceForTerminalJob` already provides exact known-job
  advancement, while `WorkflowCoordinatorService` retains terminal snapshot scanning for reconciliation.
- Battle Single Turn and SeedProbe already use current transition handlers to create later waves.
- Existing tests cover dynamic insertion, terminal advancement, graph routing, retries, restart, and
  program-kind materialization.

These are migration assets and compatibility requirements, not rewrite targets.

### Implemented production-composition prelude

`SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.*` is now the single production composition
source for the current DB-facing catalog. It registers TAS Movie, SeedProbe, Battle Context, Battle
Single Turn, Battle End, and Navigation Context in that order and validates the canonical numeric and
workflow-step mappings before publishing the registry. SavorQt and all DB-backed SavorE2E scenarios use
the full catalog; scenario-specific runtime paths, TAS settings, and Navigation working directories
remain explicit configuration overrides.

SavorE2E continues to share one `DBService`. Isolation is enforced at scenario and repeat boundaries
through the existing workflow query interface: no Pending or Running workflow, Ready step, active
materialized workflow, or terminal step awaiting reconciliation may cross the boundary. Historical
Completed, Failed, and Canceled records remain in the shared database. This is a harness invariant, not
a coordinator filter or persistence change. `battle_macro_probe` remains direct-worker-only and is not
part of the production DB descriptor catalog.

### Mandatory application coordinator composition

`SavorWorkflow/Execution/CoordinatorRuntime` is the sole production
application boundary for starting DB-backed coordination. It owns the workflow
coordinator, blob cleanup, program-result processing, worker fleet, and job
execution coordinator as one lifecycle. Startup installs both cancellation
callbacks before result processing begins and opens job cancellation admission
only after every component is ready. Pause and resume always affect both worker
admission and job dispatch. Shutdown quiesces claims, stops workers, performs
post-worker recovery, and then tears down execution, result processing, cleanup,
and workflow coordination in the canonical order.

`SavorDbRuntime` owns only database infrastructure and the immutable production
program registry. Starting database access alone does not authorize workflow
materialization or result processing. SavorQt and SavorE2E both construct the
shared coordinator runtime explicitly; production applications must not compose
or start its low-level services independently. Work authored while the runtime
is stopped remains durable and is reconciled when the coordinator next starts.

### Workflow-unit exposure and launch presentation

Workflow-unit composition and standalone launching are separate catalog
concerns. A public composable unit may appear in the graph editor independently
of whether it is also advertised by the standalone launcher. The canonical
`seed_probe` unit supports both forms: it accepts an exact savestate input and
authored SeedProbe specification from either an upstream edge/graph binding or
the standalone typed selectors.
The retired `seed_probe_chain`, `battle_seed_probe`, `dungeon_seed_probe`, and
`overworld_seed_probe` names are not current launch contracts.

Standalone presentation families are a Qt/catalog projection over otherwise
independent typed units. `tas_movie_validate_root` and
`tas_movie_validate_tree` retain their distinct ports, arguments, program
descriptors, and persistence, while both declare the
`tas_movie_validation` presentation family. Qt renders one **TAS Movie
Validation** launcher entry, records the exact selected member and typed source
reference, and validates only that member's original workflow contract. No
union reference kind or generic validation backend exists.

Current workflow launches have no user-selected root scope. SavorQt always
persists the historical storage field as `manual` with a null scope ID. The
graph revision plus its exact node/port input bindings are the launch authority;
a single scope ID cannot represent a modular multi-input graph. Historical
Execution, UI-read, and archive rows retain their stored scope fields as
evidence only.

## Core integration constraints

### Fixed persistence boundary

SavorDb remains the durable owner and its stored/durable contracts remain unchanged. The Execution Runtime introduces
no new database tables, columns, migrations, persisted runtime-invocation rows, typed workflow-binding
records, frontier records, queue records, or artifact-store interfaces.

The runtime may have richer in-memory types than the current persisted representation. That difference
is resolved at the program-kind integration boundary, not by migrating stored data. Workset-specific
coordinator/runtime interfaces may change, and the narrow execution database interface may expose:

- one bounded batch-claim/reservation operation over existing job rows and individual claim tokens;
- one exact-set lease-renewal operation over those exact tokens with an independent disposition for
  every requested item;
- one exact-set claim/start-authority validation for the complete finite package before
  `SubmitWorkset`, without re-querying each child between admissions; and
- exact targeted terminal advancement for a known workflow/step/job after its existing result
  projection succeeds.

These are hot-path access shapes over existing semantics, not new persistence contracts. Existing
single-job/reconciliation operations remain valid for recovery, and every result projection, terminal
transition, outbox write, retry, and workflow mutation retains its current transaction and idempotency
boundary.

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
4. commits through the existing result writers. The coordinator then acknowledges that item and
   publishes its known workflow/step/job plus commit sequence to the targeted terminal-advancement path.

This remains an implementation change inside the existing integration seam. It does not require
splitting `ProgramKindDescriptor` or changing persistence/workflow semantics. Adjacent coordinator and
the narrowly scoped execution database interfaces may change only for ordered batch claim, exact-set
lease renewal, claim/start validation, and targeted terminal advancement. Deterministic assembly,
transient credits, staging, ordered completions, and acknowledgements remain coordinator/worker
bookkeeping rather than database-interface responsibilities.

The public Battle workflow unit is `battle`, with one exact authored reference
of kind `authoring.battle_plan`. Continuation is not an authored wrapper: each
launch must supply the saved-contract Choice argument `continuation_mode` as
either `manual_selection` or `automatic_best_per_ending_rng`. `battle.start`
freezes that token, the exact plan ID/fingerprint, the fake-attack range, and
workflow provenance into the BattleSet. Later coordination reads only the
frozen BattleSet contract. Battle Chain Specs, Battle Run Specs, and
Explorer/Plan Settings have no current workflow or authoring surface.

Battle Plan turns reference only published Predicate Group revisions. Coordination resolves their exact
Execution Bindings and Predicate Definitions into a canonical Predicate Execution Package; the shared
library lowers every group member before module verification, and the resulting hashes cover generated
IR and exact dependencies. There is no pre-cut predicate adapter or payload decoder.

Predicate composition is worker-program composition, not workflow composition. SavorDb stores authored
Definition, Execution Binding, and Group revisions plus exact package/result lineage; it does not store
lowered predicate IR, router subscriptions, or `ProgramInstance` state.

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
  provenance; the worker binds its authoritative session and current `WorksetEpoch` immediately before
  activation;
- one independent invocation root scope and complete unwind;
- one ordinary `ProgramResult`; and
- the current program-kind result mapper and workflow-transition behavior.

The workset itself has only transient request identity, one exact in-memory
`WorkerWorksetExecutionKey`, the
fixed item order, bounded item-count/encoded-byte/item-credit limits, one exact
`ProgramBaselineKey`, and execution/cancellation state. The corresponding required
`ProgramBaselineDefinition` contains either an exact savestate artifact with its optional exact DTM
sidecar or an exact read-only DTM with its optional startup savestate. The key covers exact module,
entrypoint, dependency closure, runtime/session profile, source-artifact identity/hash/lineage, and
execution/input/capture/movie/mutation/relevant-service compatibility. The workset is never a SavorDb
row, workflow step, queue record, claim token, attempt, result, artifact, or recovery object. The worker
cannot add items, reorder them, select later durable work, or inspect SavorDb.

The `ReadOnlyMovie` form is an explicit option available only to TAS Movie phases that intentionally
begin at the DTM-declared origin; it is not their default. A later movie-paired checkpoint is always the
`Savestate` form with its exact DTM continuation sidecar, even when a TAS Movie phase consumes it.

One workset owns the session at a time, but the worker may also hold one immutable staged successor
package. Staging is limited to transport decoding, complete definition/input/dependency validation,
verified-module pinning, and host-only immutable artifact validation. It cannot mutate the session, load
state, capture a baseline, publish item start, or create a `ProgramInstance`. The worker-global
completion/acknowledgement ledger is separate from both packages: it retains promoted immutable state
captures, finalization state, authoritative terminals, and acknowledgements from executed items even
after their workset session scope releases.

The initial configurable envelope is fixed at 16 items and 32 MiB encoded bytes per workset; 64 total
worker item credits and 32 active-plus-staged items; two finalizer
threads with eight pending captures/256 MiB; and 32 retained terminals/128 MiB. Coordinator buffering
is at most one additional workset per negotiated Ready worker. No workset carries an elapsed guest
execution budget.

Admission and accounting are fixed:

An accepted workset is an uninterrupted worker-resident execution envelope: clean ordered children
continue under local resource, baseline, and credit checks without per-item coordinator authorization.
Durable leases remain independently authoritative, and later loss/supersession is enforced by the exact
asynchronous cancellation path below rather than an authorization roundtrip.

1. Each ready worker publishes negotiated item-capacity credits and count/byte limits. A credit covers
   one item from assignment through staged/resident/active/asynchronously-finalizing/terminal retention
   until exact durable acknowledgement. The coordinator derives claim demand from currently
   unreserved credits rather than configured process slots.
2. The coordinator performs one real bounded batch claim/reservation over existing jobs, preserving
   each job's individual claim token, attempt, priority, and durable lifecycle. The claim limit is
   bounded by available credits plus explicitly bounded coordinator-buffer capacity; it is not
   implemented as a scalar claim loop disguised as a batch.
3. The coordinator materializes the claimed items independently, derives each exact runtime-only key,
   and assembles worksets deterministically. It chooses the highest-priority, oldest eligible anchor,
   searches only that priority class within a configured count/byte window, and selects exact-key peers
   in durable claim order, then resolves otherwise-equal choices by queue time, job ID, worker ID, and
   item ordinal. The finite lookahead prevents affinity
   clustering from starving an older incompatible job. Leftovers remain individually claimed, leased,
   and capacity-accounted rather than being silently reordered.
4. A multi-item workset contains only adapter-declared eligible items with one exact
   `WorkerWorksetExecutionKey`. Persisted affinity is a hint, never compatibility proof. A singleton is
   valid whenever no peer fits. Before submission, the coordinator validates claim/start authority for
   the whole finite membership in one exact-set operation. The worker atomically validates the whole
   immutable package, execution key, and credits before any session mutation.
5. If no workset owns the session, the accepted package becomes active. Otherwise at most one package
   may enter the immutable staged-successor slot under exact staged correlation. Acceptance keeps every
   not-yet-started item in its existing `CLAIMED` state and does not append `JobStarted`.
6. Active common preparation materializes and verifies the exact required `ProgramBaselineDefinition`.
   A savestate workset restores its artifact baseline before its first child. If it contains multiple
   children, the active workset captures one private in-memory handle and restores that handle before
   every later child. A TAS Movie workset that explicitly opts into a DTM-origin `ReadOnlyMovie`
   baseline stages its artifacts without starting playback; every child independently establishes the
   declared movie through `MovieStartPlayback`. All workset-owned handles are released when that workset
   terminates.
7. Immediately before a child's first effect, the worker publishes its ordered item-start event and
   activates the sole `ProgramInvocation`/`ProgramInstance` without waiting for a coordinator decision.
   The coordinator consumes that event and appends the existing per-job `JobStarted` event before
   processing that child's later terminal. Recording the start is durable lifecycle bookkeeping, not
   permission for the worker to continue.
8. When execution ends, the invocation fully unwinds. If a state artifact was requested, Dolphin's native
   savestate file has already been written and read back into immutable bytes together with its exact
   movie metadata. The active item's output transaction begins bounded host-only finalization, while the
   item remains active and blocks every later child from restoring or advancing guest state.
9. The actor assembles and publishes the authoritative per-item result only after mandatory finalization
   completes. It then retains the terminal and only afterward releases the item so another child or
   workset may begin.
10. On receipt of an authoritative terminal, the coordinator immediately validates and projects it
    through the existing per-job result/artifact transaction and acknowledges the exact item after that
    durable projection succeeds.
11. After durable result projection, the coordinator publishes an in-process notification containing
    the affected workflow-step identity and commit sequence. Targeted advancement processes notifications
    in commit-sequence then stable-ID order. This hot path does not delay the item acknowledgement;
    broad terminal scanning remains restart/reconciliation fallback if a notification is missed.
12. The coordinator renews every resident/staged/active/finalizing nonterminal claim through the real
    grouped lease operation. A lease-loss, supersession, or user-cancellation disposition sends an exact
    asynchronous item/workset cancellation. In the absence of that notice, the authority established
    before acceptance persists through staged promotion and ordered item admission; there is no
    synchronous coordinator check between children.
13. Acknowledgement releases the completion-ledger entry and returns that item's worker credit.
    Coordinator-buffered claimed jobs remain separately bounded. Pending finalization,
    ready-but-order-blocked terminals, unacknowledged terminals, active/resident items, and staged items
    may not be dropped or double-counted.
14. After every child has entered terminal preparation or is classified unstarted, the active workset
    releases its baseline lease and session scope. Its finalizations/acknowledgements may continue in
    the global ledger while the staged package is locally validated and promoted unless an exact
    cancellation notice arrived. The old bookkeeping summary
    waits for its acknowledgements but does not keep the session idle.

The fixed active-workflow-count throttle is removed from the production hot path. Item-capacity credits
and the separately bounded coordinator buffer determine how far workflow materialization may run ahead;
the periodic workflow scan remains the recovery authority rather than a second competing capacity
model.

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

Runtime state policy distinguishes an already restored artifact baseline from an explicitly opted-in
TAS Movie invocation that must establish its declared DTM-origin read-only movie artifact.
Infrastructure boot is not a phase baseline. This does not change how SavorDb stores savestate or
artifact references; program-kind handlers map the existing references into the runtime artifact
baseline in memory.

Worker reuse remains an optimization. `ProgramResult` cleanup/session status governs whether the current
worker session may be reused, without requiring a new persisted cleanup-status field. Workset locality
does not permit child-invocation-scoped input, router, capture, movie, mutation, pending-action, or
epoch-bound resources to cross an item boundary. A private multi-item baseline handle belongs only to
the active workset. No serialized guest state, baseline handle, or epoch is retained for a later
workset. Every later workset rematerializes and establishes its own declared artifacts regardless of
worker placement.

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
binding representation is introduced here. Each resulting workset carries and materializes its own
complete artifact baseline; worker placement cannot change that requirement.

### Progressive worker startup

Process launch, WRMS-v1 negotiation, production catalog verification, session readiness, and capacity-
credit publication occur independently for each configured worker, with at most two concurrent
startups. The coordinator may open its data-plane loops once at least one worker has passed
`CompleteExact`: exactly the two production Full Phase module IDs/hashes, their exact dependency manifest,
no extras, all required production capabilities, and nonzero usable credits. It does not wait for every
configured process to finish booting. Later compatible workers add their credits atomically when ready.

A failed, mismatched, or still-starting worker contributes no claim capacity and cannot receive a
staged package. The pre-6A transferred test-only module may prove a `Partial` catalog process path while
the data plane stays closed; it is never one of the two production Full Phase modules. There is no scalar or
partial-catalog DB fallback, and startup is a structured failure when
no required worker becomes ready. Progressive availability changes utilization only: deterministic
claim priority, exact-key assembly, per-item attempts, and durable workflow behavior remain the same.

## Interfaces and ownership affected

| Surface | Refactor treatment |
|---|---|
| SavorDb program-kind handler implementations | May translate existing records to `ProgramInvocation` and `ProgramResult` back to existing writes |
| Predicate composition library | Pure module-building facility with no SavorDb, workflow, Dolphin, or session-service access |
| Predicate authoring and execution package | Definitions, Execution Bindings, and Groups resolve into one exact package; no pre-cut codec remains |
| Semantic-observation and interaction composition libraries | Pure module-building facilities with no SavorDb, workflow, Dolphin, filesystem, or session-service access |
| Existing macro and address-program representations | Unchanged; translated in memory at the program-kind boundary |
| Existing `savor.capture.profile/1` inputs | Unchanged and consumed opaquely by passive `CaptureService`; no replacement capture language |
| Adjacent worker integration code | May carry the new worker-facing protocol and runtime types |
| `ProgramRuntime` | Verifies and executes one invocation; has no SavorDb or workflow mutation access |
| Coordinator workset scheduling | Uses deterministic exact-key assembly, worker credits, one active plus one immutable staged package, grouped lease authority, exact cache hints, and one `SubmitWorkset` path |
| Worker-global completion/acknowledgement ledger | Transiently owns promoted immutable state captures, ordered terminal assembly/retention, acknowledgements, and credit release; never writes SavorDb |
| Narrow SavorDb execution interfaces | May support ordered batch claim/reservation, exact-set lease renewal, claim/start authority validation, and targeted known-terminal advancement over existing records |
| Queue, claim, affinity, and coordinator persistence contracts | Durable semantics and records unchanged; execution access shapes and transient workset accounting may change narrowly |
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
  processing that child's later terminal. The worker does not pause for that append.
- A state-producing item may unwind after promoting its immutable capture while background
  finalization continues. No result is projected until the actor assembles its one authoritative
  terminal in actor-assigned completion order. The coordinator then performs the unchanged per-job
  result transaction and acknowledges the item; targeted terminal advancement follows the ordered
  in-process notification and does not delay that acknowledgement.
- Exhausted item/completion credits stop later restore/admission or staged promotion without advancing
  Dolphin. Credits return only after the exact item terminal/disposition is durably acknowledged.
- Exact staged-package cancellation or lease-loss notice prevents session mutation. An accepted staged
  package otherwise promotes without synchronous coordinator reauthorization. Active item failures use
  ordinary unwind, and out-of-order background finalizer events cannot reorder terminals.
- Workset rejection, cancellation, transport loss, worker loss, or session taint returns every
  nonterminal item to the current per-job recovery/requeue path. A tainted worker starts no later
  resident item and cannot promote its staged successor.
- A failed worker never commits workflow state directly. Only existing SavorDb handlers and commands can
  advance durable work.
- Existing queued records are not rewritten. Program-kind handlers consume recognized semantic fields;
  obsolete timing keys are ignored and old legacy payload revisions are not silently reinterpreted as
  native invocations.

## Dependencies and migration implications

Migration must:

1. preserve the current SavorDb schema and migrations, durable queue/claim/lease lifecycle, workflow
   commands, result/transition transactions, and artifact interfaces; remove obsolete public authoring
   timing fields and generated timing arguments while private neutral insert shims satisfy the unchanged
   physical columns;
2. implement exact timing-free runtime invocation/result translation within program-kind handlers or adjacent
   adapters;
3. implement the narrow execution-interface allowance as real bounded batch claim/reservation, exact-set
   lease renewal, one pre-submission exact-set claim/start-authority validation, and targeted terminal
   advancement over existing records, rather than repeated scalar calls, per-item authorization
   roundtrips, or a new persistent workset abstraction;
4. replace the unactivated scalar production submission with one bounded coordinator/worker
   `SubmitWorkset` path for 1..N independently claimed/materialized jobs, with deterministic exact-key
   assembly, one immutable staged successor, item-capacity credits, composite
   artifact `ProgramBaselineDefinition` preparation, active-workset-only restoration, ordered per-item start,
   asynchronous immutable state-artifact finalization, globally ordered
   terminals, per-item projection/acknowledgement, targeted post-commit advancement, and current
   requeue/recovery behavior;
5. start at most two workers concurrently and contribute coordinator capacity only from workers that
   have completed the full WRMS-v1 `CompleteExact` catalog/session/credit gate, without admitting
   partial-capability workers;
6. preserve all current workflow lifecycle, outbox, recovery, fan-out, result-projection transaction,
   and transition tests;
7. keep only persisted payload/result codec fields that carry current semantic data, without linking
   those codecs to the legacy worker interpreter or recognizing obsolete timing keys as policy;
8. resolve current Predicate Groups, Execution Bindings, and Definitions through the shared composition
   path and reject any missing or unpublished dependency before dispatch;
9. translate current macro and address-program inputs through interaction and semantic-observation
   composition without changing their storage or interfaces;
10. pass existing capture profiles to passive `CaptureService` unchanged and preserve their current
   profile-visible behavior;
11. avoid storing `WorkerWorkset`, staged correlation, completion/acknowledgement state, capacity
   credits, `ProgramInstance`, canonical runtime IR, lowered
   predicate/observation/interaction definitions, receipts, baselines, dependency closures, or new
   runtime-only status fields in SavorDb; and
12. keep generalized workflow/frontier work outside this refactor.

Navmesh Survey may use current workflow and dynamic-step facilities where they are sufficient. If its
desired topology requires new persistence or workflow interfaces, that integration is not delivered by
this refactor.

## Boundary checks

- No SavorDb migration or database-schema change is added; physical timing-column deletion is a separate
  refactor.
- No durable queue/claim/lease/affinity/workflow-persistence, result-projection transaction, or
  artifact-storage semantic change is added. Public authoring interfaces additionally lose obsolete
  timing fields; execution-facing interfaces may change only for the bounded batch claim/lease,
  capacity, and targeted-terminal operations enumerated above.
- Existing persisted jobs materialize the correct `ProgramInvocation` through program-kind handlers.
- `ProgramResult` is projected through existing result/domain writers without changing stored
  representations.
- A one-item workset and a multi-item workset produce the same per-job lifecycle, result, artifact,
  transition, idempotency, and retry observations. Workset residency never appears as a new durable
  state.
- Claim budgeting starts from negotiated worker item-capacity credits, counts coordinator-buffered,
  staged, resident, active, finalizing, order-blocked, and unacknowledged items exactly once, and renews
  every nonterminal claim through the grouped lease operation.
- Real batch claim preserves an individual claim token/lifecycle per row and deterministic exact-key
  assembly preserves durable priority with bounded affinity lookahead and starvation protection.
- One exact-set operation validates claim/start authority for the complete finite workset before
  submission. The accepted worker then makes no authorization call between children; grouped lease
  dispositions produce exact asynchronous cancellation when authority is later lost.
- Workset admission does not mark queued resident items started. The worker's ordered item-start event
  precedes effects without a coordinator round trip, and the coordinator appends `JobStarted` before
  processing the ordered terminal.
- Synchronous paused state capture may finalize in the background only after immutable promotion.
  Actor-assigned completion order prevents terminal reordering; each final result is projected and
  acknowledged through its existing durable transaction, then its exact known workflow step is
  advanced through the ordered notification path.
- One immutable staged successor performs no session mutation and promotes only after active-scope
  release plus local capacity/cleanliness checks and absence of an exact cancellation notice. It does
  not wait for coordinator reauthorization. Prior acknowledgements may drain globally without retaining
  the old session scope.
- Progressive startup contributes capacity only from `CompleteExact` Ready workers containing exactly
  the two production Full Phase modules and no extras; a test-only `Partial` catalog never activates
  DB work or changes claim priority.
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
  and program-kind integration implementation; it does not change SavorDb storage or require an
  interface beyond the shared narrow execution operations above.

## Out-of-scope future work

The following are outside this refactor and do not gate its completion:

- generalized persisted frontier specifications, nodes, edges, leases, strategies, and deduplication;
- new typed workflow-binding or phase-edge persistence;
- changes to workflow-authoring UI or workflow policy storage;
- persisted module source, canonical IR, dependency catalogs, or verification caches;
- persisted workset identities, membership, drain cursors, acknowledgements, or workset-level attempts
  and results;
- persisted staged-package correlation, capacity credits, completion-ledger entries, active-workset
  handles, or background-finalization state;
- changes to artifact retention, storage backends, or SavorDb artifact interfaces; and
- distributed frontier scheduling or sharding.

## Source references

- `SavorDb/Execution/IExecutionDb.h`
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
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
