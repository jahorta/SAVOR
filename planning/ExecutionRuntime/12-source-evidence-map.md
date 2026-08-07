# 12 - Source Orientation

## How to use this document

This is a lightweight map to the code and research that informed the Execution Runtime guidance. It is
not an audit ledger, a live inventory, or a required implementation checklist.

Current code and executable behavior supersede stale current-state notes. Before changing a seam, open
the relevant source and tests below. Update this document only when doing so will materially help later
work or when implementation evidence changes an architectural conclusion.

## Current runtime entry points

### Worker activation and transport

- `SavorWorker/SavorWorker.cpp`
  - constructs the current `WorkerRuntime`/`EmulationSession` process composition;
  - receives versioned process/session commands through the serialized actor; and
  - exposes program execution only through workset submission.
- `SavorCore/Runner/IPC/Wire.h`
  - defines the current numeric `ProgramKind` catalog and worker messages.
- `SavorWorkflow/Worker/ProcessWorker.cpp`
  - owns the parent-side process and wire operations; and
  - submits only immutable worksets, with streamed per-item terminals and exact acknowledgements.
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
  - configures workers, applies affinity/reuse decisions, and dispatches materialized jobs;
  - `Start` currently calls `RunWorkerCapabilityPreflightForSlot` for each initial slot in one serial
    loop, even though later `ReconcileWorkerPool` already has bounded concurrent startup machinery;
  - `WorkerSlot::in_flight_job_id`, `CollectDispatchableWorkers`, and `ReleaseWorkerByResult` model one
    in-flight job per worker;
  - `DrainResultsLoop` invokes adapter/result/output persistence before `ReleaseWorkerByResult`, directly
    coupling worker availability to result projection; and
  - `WorkerJobCoordinatorLoop` is the workset-specific scheduling seam for active/staged capacity,
    event wakeups, exact-set lease authority, completion acknowledgement, and independent recovery.
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
  - resolves program-kind descriptors and prepares the current worker job/runtime request;
  - already claims batches through the existing execution database operation; and
  - `TrySelectMaterializedJobForWorker` currently scans the full `materialized_jobs_` map for each
    dispatching worker, calls `GetJob` while examining each candidate, and applies
    `BetterMaterializedDispatchCandidate`; those preferences are useful locality evidence but are not
    sufficient proof of an exact `WorkerWorksetExecutionKey`.
- `SavorDb/Execution/Workflow/SqliteExecutionDb.cpp`
  - `ClaimBatchReadyExecutionJobs` currently loops over `ClaimNextReadyExecutionJob`;
  - each `ClaimNextReadyExecutionJob` opens and commits its own `BEGIN IMMEDIATE` transaction; and
  - the pre-6A narrow batch-claim operation may coalesce that work into one ordered transaction while
    retaining an independent claim event, lease, attempt, and recovery identity per job.
- `SavorDb/Execution/Workflow/WorkflowCoordinatorService.cpp`
  - `Loop` uses `wait_for(config_.poll_interval)`, then `AdvanceAvailableWork` scans both ready and
    terminal work;
  - `PollReadyStepsFromDb` calls `ListReadySteps`, and `ReconcileTerminalWorkflowSteps` calls
    `ListTerminalReadyStepSnapshots`;
  - the current default `max_active_materialized_workflows = 30` gate counts active workflows rather
    than coordinator/worker-resident items or negotiated capacity; and
  - these are evidence for event-driven fast-path wakeups plus bounded reconciliation, not for removing
    recovery scans, and for replacing the fixed workflow-count throttle with item-capacity credits.
- `SavorCore/Runner/Runtime/WorkerRuntime.*` and `IProgramRuntimePort.h`
  - admit immutable worksets and enforce one active invocation through the private prepared-item start
    helper;
  - active/staged workset and worker-global completion-ledger ownership belongs in
    `WorkerRuntime`, outside `ProgramRuntime`; and
  - pending, staged, finalizing, and completed workset items must remain invisible to the program port
    and executor.
- `SavorCore/Runner/Runtime/ProgramRuntime/ProgramRuntime.*`
  - `StartInvocation` remains the one-child execution boundary; a workset activates children
    sequentially rather than adding batching or scheduling to `ProgramRuntime`.

### Production process and WorkerWorkset integration boundary

The current source already separates batch claiming, materialization, worker activation, and result
projection. The target uses that evidence narrowly:

- `SubmitWorkset` becomes the sole production program-dispatch path for 1..N immutable item templates;
  one independently durable job is represented by a one-item workset;
- the process composition installs the one implemented production `ProgramRuntime` and
  `SessionProgramActionHost`, reports the exact currently installed module/dependency manifest, and
  transfers one canonical test-only module for an unattended one-item `Partial` process smoke.
  The test module is never one of the two production modules;
- `Partial` cannot open the coordinator data plane. `CompleteExact` requires exactly SeedProbe and TAS
  Movie Validation, their dependency manifest, no extra installed module, and negotiated
  active/staged/finalizer/ledger limits;
- only already claimed and independently materialized jobs with the same exact runtime-only
  `WorkerWorksetExecutionKey` may share a multi-item envelope;
- one workset may mutate the session while at most one immutable successor package performs host-only
  staging. Staging may decode/verify, resolve definitions, and read/hash artifacts,
  but may not restore state, bind an epoch, capture a baseline, acquire session effects, create a
  `ProgramInstance`, or advance Dolphin;
- workset acceptance keeps unstarted items `CLAIMED`; immediately before effects the worker publishes
  an ordered item-start event without waiting, and the coordinator appends the existing `JobStarted`
  event before processing that child's later ordered terminal. The event is bookkeeping, not a permit
  for another clean child;
- each ready worker publishes item-capacity credits consumed from assignment through exact
  acknowledgement; claim demand uses unreserved credit plus at most one coordinator-buffered additional
  workset per negotiated Ready worker;
- the coordinator uses one real ordered batch claim, exact-set lease renewal, one pre-submission exact-
  set claim/start-authority validation, stable indexed selection, and targeted terminal reconciliation
  while preserving each per-job
  lifecycle. Highest durable priority/claim order anchors selection, locality breaks only equivalent
  ties, and bounded lookahead/age prevents starvation;
- accepted children run in immutable order without per-item coordinator authorization pauses. Later
  lease loss, supersession, or user cancellation arrives as exact asynchronous item/workset
  cancellation, and staged promotion does not synchronously revalidate with the coordinator;
- the coordinator includes active, staged, buffered, outbound, finalizing, resident, and
  unacknowledged work in negotiated capacity;
- each child fully unwinds before another child executes. Immutable state output may finalize on a
  bounded host-only path, but its terminal is not authoritative until hash/publication/validation
  succeeds;
- one worker-global completion/acknowledgement ledger retains exact terminals across worksets. Current
  result/artifact operations project each independently before acknowledgement. After projection, an
  in-process notification carries the affected workflow-step identity and commit sequence to targeted
  advancement, which processes commit-sequence/stable-ID order while periodic scanning remains the
  recovery authority. A clean staged successor may promote without waiting for older acknowledgements
  if global credit remains;
- `WorkerRuntime` owns active/staged order, exact prepared state, multi-item
  `ProgramBaselineDefinition`/`ProgramBaselineKey`, current child, global completion ledger, one outbound
  sequence, and drain state. Every definition carries either an exact savestate with an optional exact
  DTM sidecar or an exact read-only DTM with an optional startup savestate. An active multi-item
  savestate workset owns one private handle and restores it before later children; a movie child
  independently establishes playback. No handle or guest state survives workset completion;
- every dependent successor is a new artifact-atomic workset and materializes its own declared baseline;
- the coordinator negotiates one `CompleteExact` worker as the data-plane gate, then starts the remaining
  desired pool with at most two concurrent startups; and
- worker loss, cancellation, transport failure, or taint recovers each nonterminal durable job through
  the current per-job operations. Completed items are not rolled back and taint prevents a later child
  from starting.

This is a coordinator/worker locality and pipeline optimization, not a persistent scheduler. There is no
workset/ledger row, membership table, durable cursor, workset attempt/result, aggregate transition,
or worker access to SavorDb. Narrow ordered-batch claim, exact-set lease, pre-submission exact-set
claim/start validation, and
targeted-reconciliation interfaces are permitted; unrelated database, workflow, durable queue-state,
per-item transaction semantics, and artifact formats remain fixed.

The fixed configurable defaults are 16 items/32 MiB per workset; 64 total and 32 active-plus-staged item
credits; two finalizer threads with eight
pending captures/256 MiB; 32 retained terminals/128 MiB; two concurrent startups; and one coordinator-
buffered successor per negotiated Ready worker. WRMS stays at version 1 and program execution is
workset-only.

### Read-only legacy comparison evidence

`C:\Users\jahor\.codex\worktrees\e4f9\SAVOR` is a read-only legacy comparison repository. It must
never be modified or used for build output. Its TAS Movie flow is retained only as source evidence for
the wrapper-preserving lifecycle: movie startup replaced the guest core while keeping the live
`DolphinWrapper`, controller infrastructure, user directory, render surface, and emulation session.
That evidence informs the current narrowly named wrapper core-restart primitive. Normal wrapper
destruction remains explicit and belongs only to `Close()` or backend destruction.

- `SavorCore/Core/DolphinWrapper.cpp::startMoviePlayback` in that repository calls `Core::Stop`, waits
  for core uninitialization, prepares `Movie::PlayInput`, and calls `BootManager::BootCore` on the same
  `Core::System` and window-system state; it does not tear down the wrapper or UI/controller layer.
- `SavorCore/Phases/Programs/PlayTasMovie/TasMovieScript.h` shows the legacy TAS phase invoking that
  movie-start operation directly as its state-establishing action.

### Current program execution

- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
  - selects a fixed `PhaseScript` and program-specific payload decoder from numeric `ProgramKind`.
- `SavorCore/Runner/Script/PhaseScriptProgram.h`
  - defines the current flat script, job, initialization, and result structures.
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
  - mixes generic control operations with phase- and game-specific operations.
- `SavorCore/Runner/Script/PhaseScriptVM*.cpp`
  - retains the legacy interpreter corpus as source-characterization evidence only;
  - its advancement, input, physical-stop, state, capture, movie, and mutation escape hatches are
    hard-disconnected from production and fail locally.
- `SavorCore/Runner/Script/PSContext.h` and `PSContextCodec.cpp`
  - define the current flat shared context and its wire codec.
- `SavorCore/Runner/InputMacro/InputMacroPlan.h`, `IInputMacroPlanDriver.h`, `IInputMacroHost.h`, and
  `InputMacroRuntime.cpp`
  - define the current finite segment vocabulary, adaptive `Start/Advance` boundary, stop/input receipts,
    historical held-through-hit choice, local baselines, failure distinctions, and cleanup-once behavior
    from which native interaction semantics are reconstructed.
- `SavorCore/Runner/Breakpoints/Predicate.*`
  - defines the current compact battle predicate records and table construction.
- `SavorCore/Runner/Script/PhaseScriptVMPredicates.cpp`
  - combines predicate breakpoint arming, baseline capture, guest-memory observation, comparison,
    progress reporting, and abort-state mutation.
- `SavorCore/Runner/Breakpoints/BPRegistry.*`, `SavorCore/Core/Memory/Soa/SoaAddrRegistry.*`,
  `SavorCore/Core/Memory/Soa/SoaAddrProgram.*`, and `SavorCore/Core/Memory/KeyHostRouter.h`
  - provide current logical breakpoint names, symbolic/derived addresses, and paused-read paths that
    inform semantic-point and typed-observation composition.
- `SavorProbe/ProbeProfile.*`, `AddressProgramEvaluator.h`, and `ProbeRuntime.*`
  - define the current `savor.capture.profile/1` parser and profile behavior, including subscriptions,
    filters, address programs, PC/memory sampling, windows, flight recorders, queues, progress, control
    publications, and artifacts. Preserve those semantics behind passive `CaptureService`; do not treat
    these files as a new general observation language.
- `SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h` and
  `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
  - consume current predicate state and project predicate rejection, passed/total counts, and
    survivor-selection behavior that migration must preserve.
- `SavorCore/Runner/InputMacro/Providers/BattleCompletionInputMacroProvider.cpp` and
  `BattleResultsScreenInputMacroProvider.cpp`
  - provide source characterization for the two separate battle-end programs;
  - Battle Completion's target contract replaces the old held-through-hit/guest-poll mechanism with
    exact source-receipt suppression and the causal successors `0x8006F554 -> 0x8006F558` or
    `0x8006F590 -> 0x8006F594`; and
  - Results Screen begins from the field-return reseed/completion-manifest handoff rather than a
    monolithic victory-to-results controller.

### SavorDb integration boundary

- `SavorDb/Execution/IExecutionDb.h`
  - `ClaimBatchReadyExecutionJobs` already exposes batch-shaped reservation of independently durable
    jobs, but its SQLite implementation currently delegates to one transaction per item;
  - the pre-6A boundary may narrow or extend execution interfaces for one ordered batch transaction,
    exact-set lease renewal, claim/start authority validation, and targeted terminal reconciliation.
- `SavorDb/Execution/Jobs/JobEventOrchestration.*`
  - defines the existing `JobStarted` lifecycle event and remains the durable per-item transition from
    `CLAIMED` to running.
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
  - combines persistence, runtime initialization/materialization, result mapping, and transition hooks.
- `SavorDb/Execution/ProgramDB/ProgramKindRegistry.h`
  - resolves descriptors by numeric program kind and workflow step kind.
- `SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.*`
  - owns the current complete production descriptor composition.
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
  - applies terminal transitions and dynamic child steps.
- Phase-specific adapters under `SavorDb/Execution/ProgramDB`
  - preserve the existing stored payload/result/domain representations and workflow behavior.

The refactor may change runtime-facing handler implementations, adjacent adapters, the narrow
workset-specific execution operations above, and public authoring DTO/interface fields that previously
exposed the retained timing columns. It does not change SavorDb schema, stored representations, durable
queue states, per-job lifecycle/attempt/recovery semantics, workflow persistence, or artifact formats.
The three private neutral insert shims are the only temporary accommodation for those unchanged
`NOT NULL` columns.

## Current phase corpus

Current fixed programs are under `SavorCore/Phases/Programs`:

- SeedProbe;
- TAS Movie playback/checkpoint;
- TAS frame detector;
- Battle Context;
- Battle Single Turn;
- Battle Macro Probe;
- Battle Completion;
- Battle Results Screen; and
- Navigation Context.

`ProgramRegistry.cpp`, `Wire.h`, the phase builders/payloads, and the corresponding SavorDb adapters are
the useful starting points for each migration slice. The deprecated multi-turn BattleRunner remains
historical source evidence and does not become a target `soa.battle.legacy_path` module. Document 07
gives the target module mapping and practical characterization checks.

Navmesh Survey, collision search, cutscene fast-forward, and overworld expansion are not implemented
current phases. Their descriptions in document 08 are non-gating future examples.

## Implemented composition prelude

The shared production-composition prelude is current code:

- `ProductionProgramKindRegistry.*` builds the complete production descriptor catalog atomically;
- SavorQt and every DB-backed SavorE2E scenario use that factory;
- SavorE2E keeps one shared `DBService` and checks quiescence around DB-backed scenarios and repeats; and
- `battle_macro_probe` remains a direct-worker scenario with no SavorDb descriptor.

The focused SQLite composition test is in `SavorTests/test_savordb_fixture_sqlite.cpp`. Real-worker
coverage is under `SavorE2E`.

## Slice 3 DolphinQt instruction-step evidence

The Slice 3 source review used the exact Dolphin checkout from which the vendored build was produced:

`C:\Users\jahor\source\repos\jahorta\dolphin-2506a`

- `Source/Core/DolphinQt/Debugger/CodeWidget.cpp:497-517`
  - `CodeWidget::Step` first requires the CPU to be in stepping state;
  - saves the current `PowerPC::CoreMode`, switches to `Interpreter`, submits `CPUManager::StepOpcode`,
    waits up to 20 ms on its completion event, and restores the saved mode;
  - does not inspect the timed-wait result before restoring that mode; and
  - therefore does not establish exact instruction stepping while the active core remains JIT64.
- `Source/Core/Core/HW/CPU.cpp:161-210` and `275-293`
  - `StepOpcode` queues one step request and optional synchronization event while the CPU remains in
    `State::Stepping`;
  - the CPU thread calls the currently selected core's `SingleStep`, then flushes the event after that
    call returns; and
  - this event is the debugger's completion witness, while the stepping state remains the pause
    condition.
- `Source/Core/Core/Core.cpp:680-724` and `Source/Core/Core/HW/CPU.cpp:306-321`
  - pausing requests `CPUManager::SetStepping(true)`;
  - that call waits for the CPU thread to become inactive before returning; and
  - the public core state projects `Running + CPU stepping` as `Paused`, so safe-pause confirmation must
    use the synchronized CPU primitive rather than only observing the projected state.
- `Source/Core/Core/HW/CPU.cpp:123-159`, `330-345`, and `417-421`
  - pending CPU-thread jobs are executed only after control reaches the CPU loop's job-drain points;
  - `AddCPUThreadJob` queues work but does not notify a CPU already inside the JIT run loop;
  - `Break` requests stepping without waiting for the CPU thread to become inactive; and
  - the concrete execution backend therefore performs synchronized `Core::SetState(Paused)` on a
    backend-owned joinable helper, acknowledges pause only after that call completes, and joins the helper
    before callbacks detach or the backend is destroyed. The actor and `ExecutionEngine` remain
    nonblocking and threadless.
- `Source/Core/Core/PowerPC/PowerPC.cpp:313-349`
  - `SetMode` swaps the active core pointer between the already initialized Interpreter and JIT;
  - switching back does not explicitly clear or invalidate the JIT cache; and
  - the source says the cache will refill as needed, so the GUI step provides no independent JIT-cache
    invalidation guarantee.
- `Source/Core/Core/PowerPC/Jit64/Jit.cpp:712-720` and `768-789`, plus
  `Source/Core/Core/PowerPC/Jit64/JitAsm.cpp:98-106` and `230-246`
  - `Jit64::SingleStep` enters the compiled-code dispatcher;
  - when debugging, non-profiling, and currently stepping, compilation limits a newly compiled block to
    one instruction and disables block linking/merging for that compilation;
  - the dispatcher exits after the executed block while the CPU state is not Running; but
  - DolphinQt does not rely on that path for exact stepping and instead performs the Interpreter
    transition above.

The DolphinQt review is historical evidence for rejecting a public guest-opcode step. Migrated modules
do not import `runtime.execution.step_instructions`, and the forward verification plan does not require
an exact guest-step contract. Any future ProgramRuntime IR/source-level stepping facility is a distinct
deferred design, not an `ExecutionEngine` opcode advance.

### Private state-load bootstrap exception

The current repository has one intentionally private exception that is not a contradiction:

- `SavorCore/Core/DolphinWrapper.cpp:610-620`
  - `loadSavestate` invokes `stepBootCoreForStateLoadBlocking` only when a state load begins from a paused
    boot core whose time-base register is still zero;
  - the helper exists to let the state-load transaction proceed, after which the requested savestate
    replaces the transient bootstrap state.
- `SavorCore/Core/DolphinWrapper.cpp:1087-1100`
  - the helper temporarily selects Interpreter, submits one `StepOpcode`, waits for its completion event,
    and restores the prior core mode;
  - the current implementation discards the boolean returned by `Common::Event::WaitFor` and then
    returns `true`, so a completion timeout is not currently propagated to `loadSavestate`.
- `SavorCore/Core/DolphinWrapper.h:301-304`
  - the declaration labels this as state-load bootstrap only and explicitly excludes it from
    `IExecutionBackendPort`.

This helper remains private to the Dolphin savestate-load backend. It is not exposed through WRMS,
`ExecutionEngine`, `ProgramRuntime`, an action, a module import, or an interactive control. Focused
state-loading tests must cover the bounded-wait
failure, and the backend must propagate it before production cutover. Correcting that private failure
path does not create a guest-step contract; replacing the bootstrap mechanism remains outside the phase
slices.

## Implemented Slice 4 service boundary

Current generic session-service code is under `SavorCore/Runner/Runtime/Services`:

- `Savestate/SavestateTypes.h`, `SavestateService.*`, and `SessionSavestateBackendAdapter.*`
  - own savestate bytes and records for the one active workset without allocating or changing its
    `WorksetEpoch`;
  - own bounded immutable memory handles and caller-declared immutable state artifacts with SHA-256,
    compatibility, lineage, and exact embedded/hash-verified read-only DTM history; and
  - `CaptureMemoryHandle` already obtains immutable host-owned state bytes, while
    `CaptureFileArtifact` currently performs staging, hashing, sidecar/file publication, and validation
    synchronously. The pre-6A pipeline preserves the paused capture boundary but may move the latter
    host-only work into the bounded completion ledger/finalizer;
  - private memory handles remain bounded and workset-local; there is no cross-workset cache; and
  - require explicit external no-movie/read-only import, verify the active DTM identity during restore,
    let Dolphin restore the cursor for a cold read-only state/DTM pair, require exact cursor matching
    only for internally captured checkpoints, reject recording file-artifact capture/import/restore,
    and permit only same-session memory-handle recording rewind.
- `Movie/MovieService.*` and `InputMovieReservationAdapter.*`
  - materialize and hash-validate the exact read-only DTM baseline, stage its optional startup
    savestate, and restart only Dolphin's guest core while retaining the wrapper, session ID, and
    `WorksetEpoch`; reconcile playback mode/cursor and physical stop points; hold one unsuspendable
    movie-exclusive input reservation; and publish typed finalized recording artifacts.
- `Input/InputArbiter.*`
  - owns epoch-bound leases, sole pad publication, fresh publication/poll receipts, two-phase neutral
    release, typed one-use neutral borrow witnesses issued for the exact parent
    lease/publication/epoch, both interruption-borrow policies, and the `IInputAdvancePort`
    collaboration.
- `Memory/GuestMemory.*` and `GuestMutationService.*`
  - own paused epoch-checked reads and checked data/code mutations;
  - data restores unless explicitly committed, while executable patches are always reversible and use
    symmetric JIT/cache invalidation and readback.
- `Capture/CaptureService.*`
  - owns one opaque passive profile attachment and actor-side group reconciliation,
    and exactly-once detach/finalization while preserving the existing `ProbeRouterAdapter` semantics;
    mandatory finalization failure taints, blocks another attachment, and prevents session reuse.
- `Screenshot/ScreenshotService.*` and `Telemetry/TelemetryBus.*`
  - own one synchronous actor-thread bounded non-advancing screenshot call and bounded/coalescing
    diagnostics whose fresh replacement sequence preserves chronological drain order, with
    authoritative-overflow reporting. Active in-flight screenshot cancellation remains deferred until
    nonblocking backend/actor ingress.
- `Resources/SessionResourceLedger.*`
  - owns the actor-sequenced workset/synthetic scope tree, typed resource receipts, promotion,
    reverse-order unwind, cleanup continuations, and final cleanup
    disposition independently of `ProgramRuntime`.
- `EmulationSession.*`, `IDolphinBackend.h`, and `DolphinWrapperBackend.*`
  - keep the wrapper and infrastructure boot session-long, allocate one epoch and service graph per
    workset, coordinate savestate restoration, and expose a wrapper-preserving movie core restart.

Focused guards live in:

- `SavorTests/test_savestate_service.cpp`;
- `SavorTests/test_movie_service.cpp`;
- `SavorTests/test_session_services.cpp`;
- `SavorTests/test_capture_service.cpp`; and
- `SavorTests/test_session_resource_ledger.cpp`.

These are development guards for the generic boundary. Slice 5 builds its typed runtime/action boundary
over these owners. Live state-plus-DTM continuation and live post-write capture remain deferred until
deterministic migrated program execution/input can reach authoritative headless witnesses.

## Implemented Slice 5 typed runtime boundary

Current Slice 5 code is under `SavorCore/Runner/Runtime/ProgramRuntime`:

- `Model/*` and `Codec/ProgramCodecV1.*`
  - define the typed module/IR/value graph, invocation, three-axis result, and bounded value arena;
  - encode canonical little-endian `SPRM`, `SPRI`, and `SPRR` version-1 envelopes; and
  - compute module SHA-256 over canonical `SPRM` bytes with the declared hash omitted.
- `Store/ProgramDefinitionStore.*`, `Registry/*`, and `Verify/ProgramVerifier.*`
  - own immutable module storage, exact type/action/reducer/capability identities and dependency closure,
    compatibility checks, canonical publication, and deterministic rejection before execution.
- `Execution/ProgramExecutor.*` and `ProgramRuntime.*`
  - own the one typed control-flow executor and invocation continuation state; and
  - communicate effects only through actor-queued `ProgramActionRequest`/`ProgramActionResolution`; the
    actor-only `ActorActionResult` transfers staged output ownership directly to `WorkerRuntime`.
- `Actions/ProgramActionProtocol.h`, `SessionResourceBindingTable.*`,
  `SessionProgramActionHost.*`, and WorkerRuntime ingress
  - keep service dispatch and completions on the worker actor and bind program resource identities to the
    existing Slice 4 ledger;
  - provide the concrete internal bridge from canonical requests to session-owned services without
    exposing `EmulationSession` or a backend to `ProgramRuntime`.
- `Registry/CanonicalActionCatalog.*` and `Capabilities/SourceCapabilityPacks.*`
  - register generic `runtime.session` plus source-backed `soa.field`, `soa.battle`, and
    `soa.navigation` for the supported USA executable/address-map compatibility;
  - pin that compatibility to disc game ID `GEAE8P`, executable identity `soal-usa.GEAE8E`, and address map
    `savor.builtin-soal-usa-addresses/1`;
  - expose exact field/battle/navigation semantic inventories and coherent battle/navigation query
    descriptors; and
  - intentionally register no cutscene or overworld placeholder.
- `Capabilities/SourceReducers.*`
  - adapts the current pure battle command materializer as
    `soa.battle.materialize_turn_input`; it does not execute an input macro.
- `Composition/*`
  - implements semantic-observation, interaction, and predicate frontends that lower to ordinary IR,
    exact imports, scopes, actions, reducers, branches, and emissions before verification.

Focused guards live in:

- `SavorTests/test_program_model.cpp`;
- `SavorTests/test_program_codec_v1.cpp`;
- `SavorTests/test_program_definition_store.cpp`;
- `SavorTests/test_program_registries.cpp`;
- `SavorTests/test_program_verifier.cpp`;
- `SavorTests/test_program_executor.cpp`;
- `SavorTests/test_program_runtime.cpp`;
- `SavorTests/test_program_runtime_action_support.cpp`;
- `SavorTests/test_program_capability_packs.cpp`; and
- the three `test_*_composition.cpp` files.

This source inventory does not claim a live production program. Production composition still constructs
neither the runtime nor its implemented action host and advertises no `ProgramInvocation` capability.
Current phases have not yet been implemented natively, and no live program smoke or production-worker
SavorE2E has been established for Slice 5.

## Pre-6A timing and core-health cutover evidence

The hard cutover removes the elapsed-policy family previously distributed across:

- `ExecutionTypes`, `ExecutionEngine`, `ProgramIr`, `ProgramRuntime`, `ProgramExecutor`,
  `ProgramCodecV1`, semantic/interaction composition, `WorksetTypes`, and `WorksetWireCodec`;
- current phase payloads, PhaseScript timeout context/builders, timed legacy wrapper/macro facades, and
  ProgramDB runtime-init/job/fingerprint projections;
- SavorQt authoring drafts/editors, SavorPredict run/batch options and manifests, and SavorE2E phase
  setup; and
- `IAuthoringDb` commands/snapshots plus `SqliteAuthoringDb` selects and identity comparisons.

The physical constraint is visible in
`SavorDb/migration/Authoring/202604051000_authoring_stage2_schema.sql` and retained by
`202606141400_authoring_launch_tunables.sql`: the SeedProbe, TAS, and Battle Run `run_ms` and
`vi_stall_ms` columns are `NOT NULL` and have no defaults. Those migration files remain unchanged.
The only production references after the cutover are private authoring inserts that bind literal neutral
values; all production reads and public DTOs ignore the columns. A focused migrated fixture may inspect
them to prove old nonzero values are behaviorally inert and new rows receive `0,0`.

Core health is sourced from the execution backend's core state and VI/CoreTiming snapshot plus one
session-owned synchronous host-activity tracker. Stop routing, bounded hit-time sampling, and synchronous
capture observation register the CPU-side activity path without allocation, locks, or exceptions.
WorkerRuntime continues to drain authoritative ingress before maintenance health evaluation.
Background state-artifact finalization remains host-only and does not mask guest liveness.

## Useful tests and live references

Focused current tests include:

- `SavorTests/test_phase_script_opcodes.cpp`;
- `SavorTests/test_input_macro_runtime.cpp`;
- `SavorTests/test_battle_command_input_macro_provider.cpp`;
- `SavorTests/test_battle_macro_probe.cpp`;
- `SavorTests/test_probe_profile.cpp`;
- `SavorTests/test_capture_profile_json.cpp`;
- `SavorTests/test_probe_progress_and_wire.cpp`;
- `SavorTests/test_probe_watchpoint_registry.cpp`;
- `SavorTests/test_savestate_service.cpp`;
- `SavorTests/test_movie_service.cpp`;
- `SavorTests/test_session_services.cpp`;
- `SavorTests/test_capture_service.cpp`;
- `SavorTests/test_session_resource_ledger.cpp`;
- `SavorTests/test_program_model.cpp`;
- `SavorTests/test_program_codec_v1.cpp`;
- `SavorTests/test_program_definition_store.cpp`;
- `SavorTests/test_program_registries.cpp`;
- `SavorTests/test_program_verifier.cpp`;
- `SavorTests/test_program_executor.cpp`;
- `SavorTests/test_program_runtime.cpp`;
- `SavorTests/test_program_runtime_action_support.cpp`;
- `SavorTests/test_program_capability_packs.cpp`;
- `SavorTests/test_semantic_observation_composition.cpp`;
- `SavorTests/test_interaction_composition.cpp`;
- `SavorTests/test_predicate_composition.cpp`;
- `SavorTests/test_battle_end_results.cpp`;
- `SavorTests/test_battle_end_results_db.cpp`;
- `SavorTests/test_navigation_context_codec.cpp`;
- `SavorTests/test_navigation_context_framework.cpp`;
- `SavorTests/test_navigation_context_db.cpp`; and
- `SavorTests/test_savordb_fixture_sqlite.cpp`.

Predicate-related fixtures in `test_savordb_fixture_sqlite.cpp` cover current stored predicate records
that remain fixed migration inputs.

Legacy VM, macro-provider, and phase tests above are source-characterization aids. They do not require a
compatibility translator or executable legacy differential harness. Each migrated phase gains permanent
native-module guards, while final functional acceptance remains the Release solution build plus the
production-worker SavorE2E matrix.

The established final functional validation surface is the Release `SAVOR.sln` build plus the
production-worker SavorE2E matrix. Focused tests are development aids for the seam being changed.

The inspected Navigation Context bootstrap pair remains useful for later Survey work:

- `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`
- adjacent `navigation-context-41.nctx`

Recheck those artifacts on disk before using them. Their existence does not make Survey an implemented
or required refactor phase.

## Important conflict resolutions

- The external breakpoint-router analysis supplies useful current-state and ownership evidence, but its
  separate interpreter/macro-engine target is superseded by one `ProgramRuntime` and one
  `ProgramExecutor`.
- “Shrink `PhaseScriptVM`” is superseded by replacing the legacy interpreter with native typed modules.
  No compatibility translator or executable legacy comparison path is introduced.
- The earlier user-script plan's serialized current `PhaseScript`, `PSContext`, `PK_UserScript`, and
  second activation path are not the target architecture.
- `ProgramKind` may remain SavorDb routing, handler, affinity, semantic, and UI metadata. It must not
  select worker execution after the adapter constructs an exact runtime invocation.
- A future phase does not justify a dedicated controller, another executor, a core domain opcode, or
  durable in-worker scheduling.
- “Predicate composition library” does not mean another interpreter, runtime service, central predicate
  executor, or persistence catalog. Predicate definitions lower into the same verified modules, actions,
  scoped router subscriptions, and emissions used by every other program composition.
- “Semantic-observation composition” does not mean an observation runtime, query VM, or physical
  breakpoint owner. Semantic points, awaits, address expressions, observations, and use policies lower
  before verification into ordinary IR, router subscriptions, registered actions, typed values, and
  emissions.
- “Interaction composition” does not retain `InputMacroRuntime` or create an `InteractionRuntime`.
  Finite segments and pure reducers lower into ordinary subprogram control flow, input/resource scopes,
  semantic awaits, observations, checks, and emissions.
- Interaction migration does not import public guest-opcode stepping. Exact current-receipt suppression
  prevents duplicate re-entry, and a verifier-known semantic successor provides causal post-instruction
  evidence when a phase needs it.
- Phase execution does not import wall-clock deadlines or per-request VI-stall policy under a new name.
  Guest-dependent work ends through semantic completion, explicit cancellation, movie/epoch policy, or
  confirmed centralized core-health failure. Only classified host operations retain finite timeouts.
- Database migration is a separate refactor. Public and behavioral uses of the six authoring timing
  columns are removed now; private neutral inserts are temporary schema compatibility, not domain data.
- The private state-load bootstrap step remains inside `DolphinWrapper`'s state transaction; it is not a
  precedent for an execution action or WRMS command. Future ProgramRuntime IR/source-level stepping is a
  distinct deferred authoring/debugging concern.
- The deprecated multi-turn BattleRunner has no target module, and Battle Completion plus Battle Results
  Screen replace the old combined battle-end flow as two explicit programs.
- Battle Completion publishes neutral while paused after restore and requires host publication success,
  then observes `0x8006F554 -> 0x8006F558` or `0x8006F590 -> 0x8006F594`. It does not require a guest
  neutral poll/release witness.
- Existing `savor.capture.profile/1` semantics remain opaque behind passive `CaptureService`.
  `StopPointRouter` and `ExecutionEngine` own wake/control authority; capture observes the same routed
  sequence/snapshot/epoch identity so profile-visible control, window, recorder, progress, queue, and
  artifact behavior remains compatible.
- `EmulationSession::BeginWorkset` is the only workset-epoch authority. Savestate/movie continuation is
  exact verified evidence, not an ambient Dolphin query, and external import cannot infer movie mode.
- Slice 4 generic services are not capability packs. Slice 5 layers `runtime.session` and the
  source-backed field/battle/navigation inventories over them without reopening a broad game facade.

## Deferred boundaries

These questions are intentionally left to later migration or future work that needs them:

- additive production-catalog expansion beyond the exact SeedProbe and TAS Movie validation modules;
  the production runtime/action-host and workset-only process transport are implemented;
- any future incompatible canonical envelope version beyond implemented `SPRM`/`SPRI`/`SPRR` version 1;
- physical deletion of the six authoring timing columns and removal of their private neutral insert
  shims;
- authored source syntax, editor, publishing, and any future persisted module catalog;
- any generalized replacement for the existing `savor.capture.profile/1` language;
- detailed Survey trigger behavior beyond the first bounded path;
- final teleport/settle implementation and tolerances;
- collision-oddity objectives and refinement policy;
- source-backed cutscene pack and strategy;
- source-backed overworld pack, movement, goals, pruning, and durable frontier rules;
- native current-phase implementation, live program smoke, and production-worker SavorE2E; and
- any future ProgramRuntime IR/source-level stepping design.

None of these permits a second executor, unscoped emulator mutation, direct program access to SavorDb,
or any further SavorDb storage/interface change beyond the narrow workset operations, removed public
timing fields, and private neutral insert shims already called out above.

## Research and domain references

- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
- `planning/NavigationPhase/NavigationContextWorkflow/07-open-research-questions.md`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt`
