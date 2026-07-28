# Decisions, Risks, and Deferred Work

## How to use this record

These decisions guide implementation until current code or runtime evidence exposes a contradiction or a
genuinely general missing primitive. Revise the affected guidance when that happens. Deferred items remain
outside the implementation slice unless the slice reaches their documented boundary.

## Purpose and non-goals

This document prevents architectural ambiguity from moving downstream to implementers. It records the
chosen boundary, rejected alternatives, known risks, mitigations, and intentionally deferred product or
encoding decisions.

It is not a backlog and does not assign implementation owners or dates.

## Current code evidence

The current decisions respond to specific current pressures:

- `ProgramRegistry.cpp:44-109` uses central `ProgramKind` switches for construction and decoding.
- `PhaseScriptOpcodeTable.inc` mixes language operations with battle, TAS, state, capture, memory, and
  navigation operations.
- `PhaseScriptVM.h:35-150` owns Dolphin access, stop-point state, one snapshot, input-macro state, visual
  debugging, and domain-specific sinks.
- `PSContext.h` uses one closed variant/key map across invocation inputs, locals, and results.
- `IInputMacroPlanDriver.h:49-59` demonstrates a useful `Start`/`Advance` continuation shape, but it is
  hosted inside the current VM as a separate mini-runtime.
- `InputMacroPlan.h` and `InputMacroRuntime.cpp` encode guest-facing ordering around semantic gates,
  input epochs, memory baselines/change waits, poll acknowledgements, and cleanup-once behavior. Their
  historical held-through-hit mechanism is evidence for the semantic intent that must survive:
  same-publication continuation to a declared successor under exact source-receipt suppression.
- `ProbeProfile.*` and `ProbeRuntime.*` define the current `savor.capture.profile/1` parser, address and
  predicate programs, subscriptions including `control`, sampling policies, windows, flight recorders,
  progress, and artifact behavior.
- `ProgramKindDescriptor.h:142-185` combines queue persistence, runtime initialization, result mapping,
  and workflow transitions.
- `DBWorkflowWorkerCoordinator` claims several jobs to fill capacity but records only one in-flight job
  per worker; `ProcessWorker`, `WorkerRuntime`, and `ProgramRuntime` likewise expose one parent submission
  and one active invocation.
- Current job persistence already distinguishes reserved `CLAIMED` work from `RUNNING`, so a coordinator
  can reserve several independent jobs while publishing `JobStarted` only when a child actually begins.
- `WorkflowTransitionDecision::spawn_steps` already proves that durable orchestration can create later
  waves outside the worker.

Navmesh Survey remains unimplemented. Its requirements are a target-design test, not evidence of a
current runtime capability.

## Current target decisions

| ID | Decision | Consequence |
|---|---|---|
| D01 | One universal typed program executor/VM serves every bounded worker program. | No `NavmeshSurveyRunner`, cutscene controller, overworld controller, or controller-per-phase factory. |
| D02 | `ProgramRuntime` is a subsystem; `ProgramExecutor` is its sole interpreter; `ProgramInstance` is data/state. | These terms cannot be used interchangeably with native phase controllers. |
| D03 | `WorkerRuntime` owns external commands and session lifecycle. | Programs and actions cannot read the worker pipe or change worker assignment. |
| D04 | `ExecutionEngine` alone owns supported post-boot Dolphin advancement. | All run, frame advance, wait, input-synchronized advance, and stop behavior goes through one serialized owner. The private state-load bootstrap is not a program operation. |
| D05 | The canonical IR is a small typed CFG with a single action-await boundary. | Domain behavior never adds an interpreter opcode or switch case. |
| D06 | Native extension uses bounded actions or pure reducers. | A native extension may integrate with services or compute a transition but may not own an entire phase. |
| D07 | All effectful resources are scoped and unwound on every terminal path. | Cleanup failure taints the session and blocks reuse. |
| D08 | Savestate restore creates a new `StateEpoch`. | Guest-derived opaque handles are invalid after restore and must be reacquired. |
| D09 | Program identity is immutable module revision/hash plus entrypoint and exact dependency closure. | Today's compiled definition selected only by `ProgramKind` is insufficient. |
| D10 | Built-in C++ builders and future authored sources compile to the same `ProgramModule`. | There is no special `PK_UserScript` runtime or dual activation ABI. |
| D11 | `ProgramKind` may remain SavorDb job/handler/transition/queue/affinity and semantic metadata, but not worker executor dispatch, worker-side payload decoding, or controller selection. | Existing SavorDb routing/storage remains; worker execution switches are migration targets. |
| D12 | Programs are bounded to one session; existing SavorDb workflows retain durable waves, phase selection, deduplication, and recovery through unchanged contracts. | An invocation may emit candidate artifacts but cannot enqueue jobs or mutate durable workflow state. |
| D13 | Runtime results separate infrastructure, domain, and cleanup/session status and support zero-to-many emissions/artifacts. | Program-kind adapters project them only through existing SavorDb persistence operations. |
| D14 | Runtime compatibility is dependency-scoped, not one global registry hash. | Unrelated action additions do not invalidate modules. |
| D15 | Guest data writes and executable patches use one generic checked `GuestMutationService`. | Survey trigger suppression and battle mutations share audit, restore, and epoch behavior. |
| D16 | Game capabilities are modular packs: `soa.battle`, `soa.field`, `soa.navigation`, `soa.cutscene`, and `soa.overworld`. | The refactor does not create another monolithic game-runtime facade. |
| D17 | The existing session/router analysis is retained as evidence but its "shrink VM" and arbitrary program-factory target is superseded. | Session services are absorbed beneath the universal executor. |
| D18 | The user-script payload plan's authoring/revision separation is retained, while serialized current `PhaseScript`, flat `PSContext`, `PK_UserScript`, and dual worker paths are superseded. | Program IR is independent of any separately approved future persistence or authoring frontend; existing stored job/result formats remain. |
| D19 | Current phases are rebuilt natively on the typed runtime; no compatibility translator or executable legacy differential path is introduced. | Retained source/tests are characterization evidence only. Production gains neither a second controller nor a `soa.battle.legacy_path`/`BattleRunner` target module. |
| D20 | Navmesh Survey is a future architecture stress test with `establish_anchors` and `probe_geometry` entrypoints. | It does not gate this refactor. If implemented, its bounded programs use current workflow operations where sufficient; any new durable two-wave topology is separate workflow work. |
| D21 | Survey evidence is spatial. | Safety deadlines are infrastructure metadata, not inferred probe timing or navigation evidence. |
| D22 | SavorDb stored representations and durable orchestration remain fixed inputs. Runtime-facing adapters and workset-specific coordinator scheduling, claim use, lease maintenance, and capacity bookkeeping may change. | No SavorDb SQL/schema migration, persistent workset record, aggregate durable job/result, unrelated workflow redesign, transaction-boundary change, or artifact-storage-interface change. |
| D23 | Predicates are a reusable composition library that consumes semantic-observation results and lowers pure conditions plus explicit use policies into canonical IR, branches, and declared emissions. | There is no predicate executor, runtime service, domain opcode family, direct emulator ownership, hidden effect channel, or new persistence model. |
| D24 | Semantic observations are a reusable composition library. Capability packs define logical points, typed address/query observations, acquisition modes, baselines, and use policy; the composer lowers them before verification into ordinary IR, actions, router subscriptions, values, branches, and emissions. | There is no observation runtime, query VM, observation opcode family, direct emulator ownership, filesystem/database access, or hidden post-step timing; post-instruction evidence uses an explicit semantic successor. |
| D25 | Static and adaptive guest interactions are a reusable composition library. Versioned typed definitions, pure initialization/advancement reducers, and a finite verifier-known segment set lower before verification into subprogram CFG, semantic observations, input/execution actions, branches, and emissions. | There is no interaction runtime, peer macro scheduler, controller-like segment action, dynamically constructed effect, or second cancellation/cleanup model. |
| D26 | Existing `savor.capture.profile/1` representation and semantics remain intact behind passive `CaptureService` during this refactor. `StopPointRouter` and `ExecutionEngine` retain wake/control authority, while capture observes the same routed event and identity. | Capture profiles are neither lowered into program IR nor replaced. Existing control flags/metrics and control-triggered windows/recorders remain observable without granting a profile execution control. |
| D27 | Public guest-opcode stepping is not part of `ExecutionEngine`, WRMS, program actions, or migrated module imports. | DolphinQt's debugger behavior remains historical evidence, not a target operation. Any future ProgramRuntime IR/source-level step is a distinct deferred facility. `DolphinWrapper::stepBootCoreForStateLoadBlocking` remains a private state-load bootstrap exception whose transient boot execution is overwritten by the loaded state and is never exposed as a program command. |
| D28 | Execution budgets count active time and freeze during structured suspension. | Handler parents and nested children retain explicit remaining wall-clock and VI-stall budgets instead of expiring while another operation owns execution. |
| D29 | Requested interruption handlers use trusted descriptors, a maximum stack depth of eight, and only `ResumeParent` or `AbortParent` policy outcomes. | They cannot become a second runtime, dynamically invent effects, or directly complete a program invocation. |
| D30 | Slice 3's visual-intent seam is a Ready-session execution substate and is validated without rendering. | WRMS controls serialize through the actor, while DB-backed visual replay, GUI validation, and invocation-owned interactive policy remain deferred. |
| D31 | `StateService` is the sole authority for boot/reboot/restore and guest `StateEpoch`. | Other services participate in its transaction and mirror the committed epoch; a recoverable replacement failure does not advance it, while failed post-replacement integrity keeps the new epoch and taints the session. |
| D32 | State artifacts use caller-declared new paths and immutable SHA-256, compatibility, and lineage receipts. Read-only movie continuation is paired with exact embedded, hash-verified DTM history. | There is no ambient/latest artifact selection. Restore materializes the embedded history, requires any active read-only DTM identity to match, and verifies the prepared identity/mode plus any known cursor. External read-only import supplies no invented cursor: Dolphin restores it from the savestate and `MovieService` records the observed position. Recording file-artifact capture/import/restore is unsupported; recording rewind is limited to a same-session memory handle. |
| D33 | `InputArbiter` owns every pad publication through epoch-bound leases and supports both explicit interruption-borrow policies. | The engine sees only opaque input bindings. A neutral-required borrow consumes a fresh typed witness issued by the arbiter for the exact parent lease/publication/epoch; neutral publication and guest-observed release remain distinct, and movie playback holds an unsuspendable exclusive reservation. |
| D34 | Guest data mutations restore unless explicitly committed; executable patches are always reversible. | Data commit is an explicit lifetime decision. Code cannot be committed and always requires symmetric JIT/cache invalidation and readback on apply and restore. |
| D35 | One `EmulationSession` owns at most one opaque capture attachment, and that attachment rebinds across state restore. | Capture retains existing profile semantics and one routed identity without becoming a physical-site or wake owner. Mandatory finalization failure taints the service/session, blocks a new attachment and reuse, and requires a full rebuild. |
| D36 | Scoped cleanup uses a standalone actor-owned `SessionResourceLedger`, not an executor-private ledger. | Slice 4 enforces session cleanup before programs exist; Slice 5 binds program resource identities onto the same typed receipts, epoch policies, unwind continuations, and taint disposition. |
| D37 | Screenshot and telemetry are narrow session services. | Screenshot requests are synchronous actor-owned, epoch-correlated, bounded, and non-advancing; active in-flight cancellation is deferred until nonblocking backend/actor ingress. Telemetry is bounded/coalescing, coalesced replacements retain their fresh chronological sequence position, and required-event overflow fails closed through the serialized publisher. |
| D38 | Game capability packs layer on generic `runtime.session`; the initial source-backed set is `soa.field`, `soa.battle`, and `soa.navigation`. | Generic services remain game-neutral. `soa.cutscene` and `soa.overworld` wait for concrete migrated clients rather than appearing as placeholders or a broad interim facade. |
| D39 | Canonical program bytes are versioned little-endian `SPRM`, `SPRI`, and `SPRR` version 1; module identity is SHA-256 over canonical `SPRM` bytes with the declared hash omitted. | Canonical encoding and hashing are no longer deferred or ambient. A future incompatible model requires an explicit new version. |
| D40 | Program actions cross an actor-queued request/completion seam and bind resources to the existing session ledger. | The concrete internal `SessionProgramActionHost` owns service binding while `ProgramRuntime` obtains no `EmulationSession`, backend, service thread, or inline completion path; production runtime/host construction remains a separate cutover step. |
| D41 | Semantic-observation, interaction, and predicate frontends plus `soa.battle.materialize_turn_input` are compile-time/pure composition facilities. | They lower or compute through ordinary typed runtime contracts and do not restore macros, domain opcodes, peer runtimes, or persistence. |
| D42 | Battle end remains two native programs: Battle Completion and Battle Results Screen. | There is no monolithic `BattleEndResults` target module. Results Screen begins from the Completion manifest/field-return handoff rather than recreating the old combined controller. |
| D43 | Battle Completion publishes neutral after paused restore, requires host publication success, and advances causally from `0x8006F554` to `0x8006F558` or from `0x8006F590` to `0x8006F594`. | It does not require a guest-neutral poll/release witness and does not import guest-opcode stepping; exact receipt suppression plus the semantic successor preserves the behavior that matters. |
| D44 | `WorkerWorkset` is the sole production dispatch envelope for one or more independent invocation templates. It is finite, static, ordered, bounded, worker-resident, and non-durable. | A singleton is a one-item workset. `ProgramRuntime` remains unaware and only one child `ProgramInvocation`/`ProgramInstance` may be active. |
| D45 | A multi-item workset requires one exact `WorkerWorksetExecutionKey` covering module/entrypoint/dependency/runtime/state/movie/service compatibility and one workset-owned immutable baseline. | Persisted affinity is only a hint. The first child uses the freshly prepared state; every later child restore advances `StateEpoch`, and WorkerRuntime binds the authoritative session/epoch immediately before admission. |
| D46 | Every child retains independent job, claim, lease, invocation, attempt, budget, cancellation, result, retry, and transition identity. Per-item terminals are non-lossy and retained behind bounded acknowledgement backpressure until durable projection succeeds. | There is no aggregate domain result or persisted workset. The worker may begin later children without a coordinator scheduling round trip while its acknowledgement window has capacity. |
| D47 | Workset scheduling is domain-neutral. The coordinator fixes item order; the worker cannot refill, reorder, inspect results for continuation, or retain invocation resources between items. | Winner-sensitive work uses small worksets plus asynchronous cancellation, without a result-dependent worker predicate or new coordinator gate. Dependencies and durable fan-out remain program/workflow composition. |

## Rejected alternatives

### Native controller per phase

Rejected because it creates competing execution, cancellation, debugger, cleanup, and resource-ownership
models. Existing phases already exhibit multiple control-flow shapes inside one VM. Survey adds actions
and composition needs, not a fundamentally different controller category.

### Keep the present VM and add more opcodes

Rejected because the opcode table and dispatch already mix language semantics with domain integration.
Every domain addition would modify the central executor and permanently couple authored-program
compatibility to internal game operations.

### Make a whole phase one native action

Rejected because `RunNavmeshSurvey`, `ExploreOverworld`, or `FastForwardAllCutscenes` would recreate the
controller-per-phase architecture behind an action descriptor. An action must represent one bounded,
reusable capability transaction.

### Keep `InputMacroEngine` as a peer executor

Rejected. The useful adaptive `Start`/`Advance` shape becomes the general continuation/reducer contract.
Input leases and the effects within each verifier-known segment become ordinary actions beneath
`ProgramExecutor`; the reusable interaction composer preserves their temporal ordering without surviving
as a scheduler.

### Make an interaction segment or whole macro one native controller action

Rejected. A controller-like action would hide input-before-departure publication, exact source
suppression, successor-point identity, request/release acknowledgement where required, baselines,
branches, cancellation, and cleanup from verification and tracing. Interaction composition lowers these
steps into visible ordinary IR and bounded actions. A pure reducer may select only a declared segment.

### Separate observation runtime or query VM

Rejected. Semantic waits, bounded hit-time samples, paused reads, registered coherent queries,
baselines, and evidence policies compose into the existing router, engine, actions, IR values, and
emissions. A separate runtime, query instruction language, or implicit post-step mode would recreate
hidden control flow and timing semantics.

### Separate predicate runtime

Rejected. A predicate interpreter, controller, or independently scheduled service would recreate a
mini-runtime with hidden control flow and resource ownership. Reusable predicate definitions are a
composition frontend only; their generated behavior executes through the universal program executor and
existing session services.

### Replace capture profiles during runtime extraction

Rejected for the initial refactor. Re-authoring `savor.capture.profile/1` as a generalized capture-plan
language would combine an architecture cutover with a behavior and artifact-format migration. Existing
profile parsing and semantics move intact behind passive `CaptureService`. Capture observes routed
control events but does not acquire wake/control authority.

### Expose Interpreter-backed guest stepping as a production operation

Rejected. DolphinQt's debugger implements its instruction Step by selecting Interpreter and then
restoring the previous core. Savor shall not hide that CPU-mode transition behind an
`ExecutionEngine`, WRMS, action, or guest-opcode operation. The private
`stepBootCoreForStateLoadBlocking` helper is narrower: it exists only to unblock a paused boot-state
replacement, cannot be called by a program, and its transient execution is discarded by the loaded
state.

### Put arbitrary-depth search inside a worker program

Rejected because worker loss would lose search topology and because one worker would become a scheduler.
Existing durable workflow behavior remains in SavorDb. Any generalized frontier state, deduplication,
retry policy, or arbitrary-wave generation requires a separate workflow project.

### Freeze current `PSContext` and `ProgramKind` as public ABI

Rejected because current inputs, locals, results, and domain-specific values are conflated. Exact replay,
many artifacts, typed schemas, and independent capability evolution require explicit contracts.

### Compatibility translator or runtime

Rejected. Current phases are implemented directly as native typed modules. The old providers, codecs,
tests, and VM sources remain readable characterization evidence, but neither a temporary translator nor
a second executable comparison path is introduced.

### Retain a legacy BattleRunner or monolithic BattleEndResults module

Rejected. The deprecated multi-turn BattleRunner does not become `soa.battle.legacy_path`, and the old
combined battle-end controller is not recreated. Battle Completion and Battle Results Screen remain
separate modules with an explicit manifest/state handoff.

### Make a phase accept a large list only for throughput

Rejected. A domain may legitimately define one atomic list-valued operation, but transport efficiency
does not change a scalar phase contract into one aggregate invocation, attempt, cancellation, retry, or
result. Independent items remain independent `ProgramInvocation`s inside `WorkerWorkset`.

### Persist a workset or aggregate several jobs into one result

Rejected. Durable workset membership would create a new recovery and retry cardinality, and an aggregate
terminal would delay otherwise complete results and increase the worker-loss blast radius. Existing jobs
remain independently claimed, started, completed, retried, and transitioned.

### Use a refillable or worker-reordered queue

Rejected for the initial contract. Dynamic refill becomes an open-ended worker scheduler; reordering can
silently alter priority and cancellation behavior. A workset is a finite immutable coordinator-ordered
list. Numeric sizing remains configurable.

### Let the worker stop from domain result content or retain child resources

Rejected. `WorkerRuntime` cannot interpret result schemas or carry input, router, capture, movie,
mutation, continuation, observation, or epoch-bound state into another invocation. Supersession uses
small worksets and asynchronous exact item/workset cancellation.

## Risks and mitigations

| Risk | Failure mode | Required mitigation |
|---|---|---|
| Actions become hidden controllers | Long-running action owns loops, branching, or private state machines | Enforce descriptor review, bounded effects, cancellation points, trace visibility, and maximum budgets |
| Action granularity becomes too small | Programs become noisy and encode emulator mechanics | Provide reusable composite IR subprograms and transaction-sized actions |
| Action granularity becomes too large | Programs cannot observe, cancel, replay, or compose behavior | Split at observable capability transactions; keep orchestration in IR or workflow |
| Cleanup is best-effort only | Inputs, patches, subscriptions, movies, or captures leak into the next job | Structured scopes, restoration receipts, fault injection, and mandatory session taint |
| State restore leaves stale handles | Guest pointers or router assumptions are reused after restore | Epoch-tag every opaque handle and verify at action boundaries |
| Version checks are globally fragile | Adding an unrelated registry entry rejects all cached modules | Verify exact imported dependency closure and signature hashes |
| Legacy execution is accidentally reconstructed | A native module shadows a legacy controller/provider, or source-stop timing is smuggled back as a hidden guest step | Implement each phase through common typed composition, require semantic-successor/no-guest-step architecture guards, and use permanent native characterization plus final E2E |
| Replay is overstated | Matching host timing is mistaken for deterministic game behavior | Compare exact inputs plus action/branch/observation traces; declare nondeterministic action fields |
| `ContinueSession` is used implicitly | A phase consumes unknown patches, input, or guest state | Require expected session lineage, epoch, and clean resource ledger |
| Domain packs become another monolith | Unrelated phase changes force one broad capability version | Namespace and version packs and action imports independently |
| Semantic observation hides timing or coherence | A read silently occurs after an undeclared advance, hit-time work blocks the CPU thread, or separate scalar reads are treated as one snapshot | Make acquisition mode explicit, require an explicit semantic successor for post-instruction evidence, restrict hit-time samples, and use registered coherent queries |
| Observation absence is conflated with a domain value | Missing evidence silently becomes false or zero | Keep optional `Unavailable` distinct, fail required evidence structurally, and trace availability |
| Adaptive interaction timing drifts | Input is published after leaving the source, duplicate re-entry occurs, a semantic successor runs under the wrong input, or host neutral publication is confused with a phase that requires guest-observed release | Characterize and test publication-before-departure, exact receipt suppression, successor point/PC/sequence/epoch matching, and phase-specific release policy; forbid guest-step imports |
| Interaction composition becomes a hidden macro engine | A segment action or reducer owns service calls, loops, dynamic effects, or private cleanup | Restrict reducers to pure selection of verifier-known segments and lower every effect/cancellation/unwind edge into ordinary visible IR |
| Predicate composition hides effects or conflates false with unavailable | A reusable check silently owns stop points, fails open on unreadable state, or changes failure classification | Require typed observations, explicit use-site policy, exact imports, scoped subscriptions, declared emissions, and trace-visible evaluation outcomes |
| Capture extraction changes profile semantics or steals control | Sampling/window/artifact behavior drifts, or a profile creates a foreground wake path | Run profile compatibility characterization, keep one routed-hit identity, and enforce passive observation with router/engine as control owners |
| State and movie continuation drift apart | A savestate restores with the wrong DTM, frame/input position, disc, runtime, or recording generation | Pair exact embedded/hash-verified DTM history with immutable read-only state evidence; require any active DTM identity to match; for cold external read-only import, stage the DTM and reconcile the cursor observed after restore; for an internally captured checkpoint, require its known cursor to match; reject implicit external movie mode and all recording file-artifact restore |
| Input borrowing corrupts held or neutral state | A handler overwrites an unsuspendable owner or resumes a parent before the guest observed release | Make borrow policy explicit, preserve held input until borrower publication or require a fresh typed arbiter-issued witness bound to the exact parent lease/publication/epoch, and correlate every publication/acknowledgement |
| A data commit or code patch escapes its scope accidentally | A later operation inherits an unintended write or executable patch | Require explicit data commit, forbid code-patch commit, record original/replacement/readback, and use ledger-driven reverse restoration with taint on uncertainty |
| Resource cleanup has two authorities | Executor-local and session-local stacks disagree about order, epoch, or taint | Use the standalone session ledger as the one receipt/disposition source and project future invocation scopes onto it |
| Workset compatibility is too weak | Jobs with different state, module, movie, capture, or service policy share a baseline | Require and verify one exact execution key before session mutation; persisted affinity is never sufficient |
| Workset state leaks between children | A later child inherits input, subscriptions, capture, mutation, continuation, or stale epoch evidence | Nest a fresh invocation root under the workset scope, fully unwind it, restore the baseline, advance epoch, and reacquire every child resource |
| Resident work monopolizes a worker | Large bundles create head-of-line blocking or affinity starvation | Bound count, encoded bytes, aggregate budgets, resident capacity, and unacknowledged result bytes; preserve coordinator priority order |
| Resident claims expire | Pending children are requeued or duplicated while still inside the worker | Renew every active and resident-pending lease and cancel/drop an item immediately if its authority is lost |
| Per-item results are lost or unbounded | Worker runs ahead of durable persistence or buffers unlimited terminal payloads | Use a non-lossy bounded acknowledgement window; stop child admission when full and acknowledge only after existing result projection commits |
| Cancellation has ambiguous scope | A pending sibling, active child, or whole workset is stopped accidentally | Use separate exact item and workset cancellation identities and classify pending children as unstarted |
| Worker loss enlarges the retry unit | Completed children rerun or pending children disappear with transient workset state | Persist and acknowledge each child independently; recover unacknowledged and unstarted jobs through existing attempt/lease/idempotency behavior |
| Visual verification becomes a manual dependency | A slice cannot be completed without a window, desktop automation, screenshot judgement, or user attendance | Use fake visual-intent sessions, protocol/state telemetry, and headless Dolphin guards; defer rendered acceptance |
| Private state-load bootstrap leaks into program execution | A boot workaround becomes an alternate guest-step API | Keep it private to the state backend, expose only state-replacement receipts, and reject any WRMS/action/module import for it |
| Planning drifts from code | Current-state statements become obsolete | Re-read the affected current symbols and revise guidance when implementation evidence changes an architectural conclusion |

## Deferred work

### Contract encodings

- Production worker construction of the implemented `ProgramRuntime` and `SessionProgramActionHost`,
  plus required `WorkerWorkset`/program capability advertisement.
- Any future incompatible canonical envelope version beyond implemented `SPRM`/`SPRI`/`SPRR` version 1.
- Optional compression, numeric workset limits, sizing heuristics, and negotiation extensions beyond the
  required non-lossy per-item terminal/acknowledgement flow.

These are deferred runtime encodings, not permission to alter the logical fields or ownership model.
SavorDb SQL/schema, migrations, stored representations, persistent workset identity, unrelated
interfaces/workflows, and artifact-store interfaces remain out of scope. Workset-specific coordinator,
claim-use, lease, and capacity changes are part of the pre-6A prelude.

### Cutover and live validation

- Native implementation and migration of the current phases.
- The pre-6A unified `SubmitWorkset` path and production `ProgramRuntime` installation.
- Required workset/program/catalog capability advertisement at Slice 7.
- A headless live game-program smoke after production runtime/action-host construction.
- Production-worker SavorE2E after migrated programs and handler adapters restore the complete path.

These remain downstream cutover work. Their absence does not permit a temporary compatibility executor,
scalar production fallback, persistent workset, or unrelated SavorDb redesign.

### Authoring experience

- JSON, DSL, graphical, or other authored source syntax.
- Source editor, validation UI, debugger UI, publication, ownership, and permission model.
- Any future ProgramRuntime IR/source-level stepping facility; it is distinct from guest-opcode
  advancement and requires its own source/debugger design.
- A generalized capture-plan authoring language or replacement for `savor.capture.profile/1`.

Every frontend must compile to the same verified `ProgramModule`.
The capture item is different: until separately designed, existing profiles remain opaque versioned
inputs consumed by `CaptureService` rather than a frontend that compiles into module IR.

### Game-specific research and algorithms

- Generalized `eventhook` trigger behavior and trigger allowlisting.
- Overworld movement, encounter, branching, and game-state rules.
- Collision-oddity objective generation and refinement heuristics.
- Cutscene fast-forward optimization strategy and choice schedule.
- Navigation replay verification tolerances.
- Survey spatial probe spacing, settle tolerances, and reduction algorithms beyond the defined first-slice
  semantics.
- Live state-plus-DTM continuation and live post-write capture witnesses once deterministic program
  execution/input can drive them headlessly.

These items may add capability-pack actions, schemas, and programs. They may not create a new executor.

### Separate future projects

- User-facing workflow composer changes.
- Module revision promotion and rollback UI.
- Artifact retention policy and storage backend.
- Persisted authored source/module catalogs or queryable normalized source operations.
- Generalized workflow/frontier persistence, policies, and scheduling.
- Operational dashboards and performance targets.

These are not deferred implementation choices in the Execution Runtime refactor.

## Interfaces and ownership affected

The decisions require replacement or decomposition of:

- scalar worker invocation submission, protocol activation, and result envelopes;
- coordinator worker-slot, resident-capacity, claim/lease, item-start, result-acknowledgement, and recovery
  bookkeeping needed by unified `SubmitWorkset`;
- `ProgramRegistry` construction/decoding switches;
- `PhaseScriptVM` and its central opcode dispatch;
- `PSContext` as a public invocation/result contract;
- VM-owned macro, stop-point, savestate, input, and Dolphin services; and
- runtime-facing program-kind handler implementation and adjacent worker integration.

They preserve existing SavorDb descriptors, stored jobs/results, durable workflow meaning, artifact
contracts, and transaction boundaries. Workset-specific coordinator scheduling and claim/lease use may
change; no persistent workset or aggregate result is added. They also retain current phase behavior and
the useful `Start`/`Advance` adaptive pattern. Existing stop/address/query/baseline and input-macro
inputs are reconstructed through semantic-observation and interaction composition in memory. Existing
`savor.capture.profile/1` artifacts remain unchanged behind the one session-owned `CaptureService`.
Slice 4 has already moved state/epoch, input, guest mutation, movie, screenshot, telemetry, and resource
cleanup behind their narrow session-owned services; Slice 5 now adds the canonical program/action
surface, resource-binding seam, initial packs, and composition frontends over them. Native phase
implementation, production activation, and program-kind adapters remain later cutover work.

## Failure and cleanup behavior

No deferred implementation detail may weaken these rules:

- cancellation and timeout unwind the same resource stack as normal return;
- mandatory restoration failure taints the session;
- a tainted session cannot run another invocation;
- state restore invalidates epoch-bound handles;
- semantic-point receipts, observations, and baselines cannot cross `StateEpoch`;
- optional observation unavailability remains distinct from false or zero, while required missing
  evidence fails structurally;
- interaction unwind neutralizes input, proves release when required, and releases nested subscriptions
  and the interaction-wide input lease exactly once;
- timeout, unexpected point, unacknowledged input/release, unsatisfied check, infrastructure failure,
  cancellation, and cleanup failure remain distinguishable;
- capture finalization preserves existing profile behavior, but a capture profile never owns wake or
  advancement; mandatory finalization failure taints and blocks another attachment/session reuse;
- state and movie continuation restore together from exact embedded/hash-verified evidence; active DTM
  identity must match, external mode is never inferred, recording file-artifact restore is rejected,
  and recording rewind is limited to a same-session memory handle;
- data mutation restores unless explicitly committed, executable patches always restore, and any
  unproven patch cleanup taints the session;
- a domain failure can be a clean infrastructure completion;
- an unsatisfied predicate follows its explicit check policy, while inability to obtain required evidence
  remains a distinct action or infrastructure failure;
- existing SavorDb retry, idempotency, and lineage behavior remains unchanged; runtime trace identity is
  not a new persistence requirement;
- a workset child completes full invocation unwind before the next child is admitted;
- a clean child failure may continue, while cleanup uncertainty or taint halts the workset;
- per-item terminal results remain non-lossy until durably acknowledged, and full acknowledgement
  backpressure stops admission without advancing Dolphin; and
- pending children cancelled or abandoned after worker failure remain individually unstarted and
  recoverable rather than receiving an aggregate workset outcome.

## Dependencies and migration implications

Document 09 gives the dependency direction. An implementation slice that appears to require a second
executor, phase controller, unscoped mutation, direct Dolphin access, or durable in-worker frontier must
stay within the current constraints or revise the relevant guidance from concrete implementation evidence
before broadening the design.

## Consistency checks

- Single-owner execution, scoped cleanup, typed runtime, and no-dual-runtime constraints remain intact.
- Deferred items have boundaries that keep them out of the universal executor and current-phase migration.
- Risk mitigations are implemented and tested with the seam that makes them relevant.
- No SavorDb migration, stored-representation change, persistent workset, aggregate durable job/result,
  unrelated workflow/interface change, transaction-boundary change, or artifact-storage-interface change
  is part of the refactor.
- Workset-specific coordinator grouping, resident capacity, claim-use, and lease maintenance are
  permitted only within the transient per-item contract.
- Semantic-observation, interaction, and predicate composition lower completely before verification and
  leave no peer runtime, scheduler, query VM, opcode family, or hidden controller.
- Existing `savor.capture.profile/1` semantics remain behind passive `CaptureService`; a generalized
  replacement language remains deferred.
- `StateService` remains the sole epoch authority; immutable state/movie evidence, explicit external
  import policy, same-session recording rewind, input borrowing, mutation lifetime, and standalone
  ledger disposition cannot be reimplemented in capability packs.
- Slice 5 capability packs consume the generic runtime catalog and do not reopen a broad Dolphin/game
  facade; deferred cutscene/overworld packs must preserve that boundary.
- No compatibility translator, `soa.battle.legacy_path`/`BattleRunner` module, monolithic
  `BattleEndResults` module, or public guest-opcode step enters the cutover.
- `SubmitWorkset` is the only production program-dispatch path; a one-item workset covers singleton and
  direct-worker execution without a scalar fallback.
- Worksets remain outside IR and `ProgramRuntime`, use one active child, preserve exact per-item
  durability, and never interpret result content or retain child resources.

## Source references

- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
- `SavorCore/Runner/Script/PhaseScriptVM.h:35-150`
- `SavorCore/Runner/Script/PSContext.h`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:49-59`
- `SavorCore/Runner/InputMacro/InputMacroPlan.h`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp`
- `SavorProbe/ProbeProfile.h`
- `SavorProbe/ProbeProfile.cpp`
- `SavorProbe/ProbeRuntime.h`
- `SavorProbe/ProbeRuntime.cpp`
- `SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/ProgramRuntime.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/Actions`
- `SavorCore/Runner/Runtime/ProgramRuntime/Capabilities`
- `SavorCore/Runner/Runtime/ProgramRuntime/Composition`
- `SavorCore/Runner/Runtime/WorkerRuntime.*`
- `SavorWorkflow/Worker/ProcessWorker.*`
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.*`
- `SavorWorkflow/Execution/JobMaterializationService.*`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:121-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router`
