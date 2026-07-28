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
  - keeps production `ProgramInvocation` unavailable during the hard-cutover interval.
- `SavorCore/Runner/IPC/Wire.h`
  - defines the current numeric `ProgramKind` catalog and worker messages.
- `SavorWorkflow/Worker/ProcessWorker.cpp`
  - owns the parent-side process and wire operations;
  - currently exposes one encoded-invocation submission at a time; and
  - is the parent transport seam where the pre-6A `SubmitWorkset`, streamed per-item terminal, and
    acknowledgement protocol replaces that unactivated scalar surface.
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
  - configures workers, applies affinity/reuse decisions, and dispatches materialized jobs;
  - `WorkerSlot::in_flight_job_id`, `CollectDispatchableWorkers`, and `ReleaseWorkerByResult` currently
    model one in-flight job per worker; and
  - `WorkerJobCoordinatorLoop` is the workset-specific scheduling seam for resident capacity, lease
    renewal, per-item start/result acknowledgement, and independent recovery.
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
  - resolves program-kind descriptors and prepares the current worker job/runtime request;
  - already claims batches through the existing execution database operation; and
  - `BetterMaterializedDispatchCandidate` already treats savestate, program kind, and runtime affinity as
    scheduling preferences, which are evidence for locality but not sufficient proof of an exact
    `WorkerWorksetExecutionKey`.
- `SavorCore/Runner/Runtime/WorkerRuntime.*` and `IProgramRuntimePort.h`
  - currently admit one encoded invocation and enforce one active invocation through
    `RequireReadyProgramRuntime`;
  - the pre-6A workset owner belongs in `WorkerRuntime`, outside `ProgramRuntime`; and
  - pending workset items must remain invisible to the program port and executor.
- `SavorCore/Runner/Runtime/ProgramRuntime/ProgramRuntime.*`
  - `StartInvocation` remains the one-child execution boundary; a workset activates children
    sequentially rather than adding batching or scheduling to `ProgramRuntime`.

### WorkerWorkset integration boundary

The current source already separates batch claiming from scalar worker activation. The target uses that
evidence narrowly:

- `SubmitWorkset` becomes the sole production program-dispatch path for 1..N immutable item templates;
  one independently durable job is represented by a one-item workset;
- only already claimed and independently materialized jobs with the same exact runtime-only
  `WorkerWorksetExecutionKey` may share a multi-item envelope;
- workset acceptance keeps unstarted items `CLAIMED`; immediately before effects the worker publishes
  an ordered item-start event without waiting, and the coordinator appends the existing `JobStarted`
  event before processing that child's later ordered terminal;
- the coordinator renews every resident nonterminal claim lease and includes worker-resident,
  coordinator-buffered, outbound, and active items in its bounded capacity calculation;
- each child fully unwinds and publishes its ordinary result immediately; current result/artifact and
  terminal-transition operations project it independently before the protocol acknowledgement is
  returned;
- `WorkerRuntime` owns the static order, exact prepared state, multi-item reusable workset baseline,
  current child, bounded unacknowledged-result window, and drain state, while `ProgramRuntime` sees
  only one child invocation; a one-item workset skips reusable-baseline capture;
  and
- worker loss, cancellation, transport failure, or taint recovers each nonterminal durable job through
  the current per-job operations. Completed items are not rolled back and taint prevents a later child
  from starting.

This is a coordinator/worker locality optimization, not a persistent scheduler. There is no workset row,
membership table, durable cursor, workset attempt/result, aggregate transition, or worker access to
SavorDb. Any source change outside the worker transport, runtime actor, materialization, coordinator
bookkeeping, and focused tests requires separate evidence; unrelated database, workflow, queue, claim,
transaction, and artifact contracts remain fixed.

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
  - `ClaimBatchReadyExecutionJobs` already reserves multiple independently durable jobs and remains the
    unchanged claim operation used before workset grouping.
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

The refactor may change runtime-facing handler implementations or adjacent adapters. It does not change
SavorDb schema, stored representations, database-service interfaces, queues, claims, workflow
persistence, transaction boundaries, or artifact-storage interfaces.

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

This helper remains private to state replacement. It is not exposed through WRMS, `ExecutionEngine`,
`ProgramRuntime`, an action, a module import, or an interactive control, and verification treats only the
typed state-load receipt as public behavior. Focused state-loading tests must cover the bounded-wait
failure, and the backend must propagate it before production cutover. Correcting that private failure
path does not create a guest-step contract; replacing the bootstrap mechanism remains outside the phase
slices.

## Implemented Slice 4 service boundary

Current generic session-service code is under `SavorCore/Runner/Runtime/Services`:

- `State/StateTypes.h`, `StateService.*`, and `SessionStateBackendAdapter.*`
  - make `StateService` the sole `StateEpoch` authority;
  - own bounded immutable memory handles and caller-declared immutable state artifacts with SHA-256,
    compatibility, lineage, and exact embedded/hash-verified read-only DTM history; and
  - require explicit external no-movie/read-only import, verify the active DTM identity during restore,
    let Dolphin restore the cursor for a cold read-only state/DTM pair, require exact cursor matching
    only for internally captured checkpoints, reject recording file-artifact capture/import/restore,
    and permit only same-session memory-handle recording rewind.
- `Movie/MovieService.*` and `InputMovieReservationAdapter.*`
  - materialize and hash-validate exact embedded DTM history before an initial single boot, post-open
    reboot, or restore; carry a DTM starting savestate into the state transaction; reject a mismatched
    already-active DTM identity; reconcile the resulting mode/cursor; hold one unsuspendable
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
  - owns one opaque passive profile attachment, actor-side group reconciliation, state-restore rebind,
    and exactly-once detach/finalization while preserving the existing `ProbeRouterAdapter` semantics;
    mandatory finalization failure taints, blocks another attachment, and prevents session reuse.
- `Screenshot/ScreenshotService.*` and `Telemetry/TelemetryBus.*`
  - own one synchronous actor-thread bounded non-advancing screenshot call and bounded/coalescing
    diagnostics whose fresh replacement sequence preserves chronological drain order, with
    authoritative-overflow reporting. Active in-flight screenshot cancellation remains deferred until
    nonblocking backend/actor ingress.
- `Resources/SessionResourceLedger.*`
  - owns the actor-sequenced session/synthetic scope tree, typed resource receipts, promotion,
    reverse-order unwind, cleanup continuations, state epoch policies/rebind, and final cleanup
    disposition independently of `ProgramRuntime`.
- `EmulationSession.*`, `IDolphinBackend.h`, and `DolphinWrapperBackend.*`
  - compose those services behind narrow private backend facets and drive state replacement as one
    participant transaction.

Focused guards live in:

- `SavorTests/test_state_service.cpp`;
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
  - communicate effects only through actor-queued `ProgramActionRequest`/`ProgramActionCompletion`.
- `Actions/ProgramActionProtocol.h`, `SessionResourceBindingTable.*`,
  `SessionProgramActionHost.*`, and WorkerRuntime ingress
  - keep service dispatch and completions on the worker actor and bind program resource identities to the
    existing Slice 4 ledger;
  - provide the concrete internal bridge from canonical requests to session-owned services without
    exposing `EmulationSession` or a backend to `ProgramRuntime`.
- `Registry/CanonicalActionCatalog.*` and `Capabilities/SourceCapabilityPacks.*`
  - register generic `runtime.session` plus source-backed `soa.field`, `soa.battle`, and
    `soa.navigation` for the supported USA executable/address-map compatibility;
  - pin that compatibility to game `GEAE8E`, executable `soal-usa.GEAE8E`, and address map
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
Current phases have not yet been implemented natively, no live program smoke or production-worker
SavorE2E has been established for Slice 5, and no SavorDb contract changed.

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
- `SavorTests/test_state_service.cpp`;
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
- `StateService` is the only guest-epoch authority. State/movie continuation is exact verified evidence,
  not an ambient Dolphin query, and external import cannot infer movie mode.
- Slice 4 generic services are not capability packs. Slice 5 layers `runtime.session` and the
  source-backed field/battle/navigation inventories over them without reopening a broad game facade.

## Deferred boundaries

These questions are intentionally left to later migration or future work that needs them:

- production runtime/action-host construction and capability advertisement;
- any future incompatible canonical envelope version beyond implemented `SPRM`/`SPRI`/`SPRR` version 1;
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
or a change to SavorDb storage/interfaces inside this refactor.

## Research and domain references

- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
- `planning/NavigationPhase/NavigationContextWorkflow/07-open-research-questions.md`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt`
