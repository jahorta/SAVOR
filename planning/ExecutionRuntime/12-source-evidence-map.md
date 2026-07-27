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
  - owns the parent-side process and wire operations.
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
  - configures workers, applies affinity/reuse decisions, and dispatches materialized jobs.
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
  - resolves program-kind descriptors and prepares the current worker job/runtime request.

### Current program execution

- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
  - selects a fixed `PhaseScript` and program-specific payload decoder from numeric `ProgramKind`.
- `SavorCore/Runner/Script/PhaseScriptProgram.h`
  - defines the current flat script, job, initialization, and result structures.
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
  - mixes generic control operations with phase- and game-specific operations.
- `SavorCore/Runner/Script/PhaseScriptVM*.cpp`
  - retains the legacy interpreter corpus as translation/characterization evidence;
  - its advancement, input, physical-stop, state, capture, movie, and mutation escape hatches are
    hard-disconnected from production and fail locally.
- `SavorCore/Runner/Script/PSContext.h` and `PSContextCodec.cpp`
  - define the current flat shared context and its wire codec.
- `SavorCore/Runner/InputMacro/InputMacroPlan.h`, `IInputMacroPlanDriver.h`, `IInputMacroHost.h`, and
  `InputMacroRuntime.cpp`
  - define the current finite segment vocabulary, adaptive `Start/Advance` boundary, stop/input receipts,
    held-through-hit choice, local baselines, failure distinctions, and cleanup-once behavior that
    interaction composition must preserve.
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

### SavorDb integration boundary

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
- legacy multi-turn battle path;
- Battle Context;
- Battle Single Turn;
- Battle Macro Probe;
- Battle Completion;
- Battle Results Screen; and
- Navigation Context.

`ProgramRegistry.cpp`, `Wire.h`, the phase builders/payloads, and the corresponding SavorDb adapters are
the useful starting points for each migration slice. Document 07 gives the target module mapping and
practical parity checks.

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

The Slice 3 contract consequently reports exact guest-instruction step as unsupported on the concrete
JIT64 backend. It neither changes CPU mode nor calls a JIT-block step an instruction. This conclusion is
source-derived and requires no DolphinQt launch, rendered window, desktop control, or user inspection.

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
    disposition independently of the future `ProgramRuntime`.
- `EmulationSession.*`, `IDolphinBackend.h`, and `DolphinWrapperBackend.*`
  - compose those services behind narrow private backend facets and drive state replacement as one
    participant transaction.

Focused guards live in:

- `SavorTests/test_state_service.cpp`;
- `SavorTests/test_movie_service.cpp`;
- `SavorTests/test_session_services.cpp`;
- `SavorTests/test_capture_service.cpp`; and
- `SavorTests/test_session_resource_ledger.cpp`.

These are development guards for the generic boundary. Capability packs and production actions are
Slice 5 work. Live state-plus-DTM continuation and live post-write capture remain deferred until
deterministic program execution/input can reach authoritative headless witnesses.

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
- `SavorTests/test_battle_end_results.cpp`;
- `SavorTests/test_battle_end_results_db.cpp`;
- `SavorTests/test_navigation_context_codec.cpp`;
- `SavorTests/test_navigation_context_framework.cpp`;
- `SavorTests/test_navigation_context_db.cpp`; and
- `SavorTests/test_savordb_fixture_sqlite.cpp`.

Predicate-related fixtures in `test_savordb_fixture_sqlite.cpp` cover current stored predicate records
that remain fixed migration inputs.

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
- “Shrink `PhaseScriptVM`” is superseded by replacing the legacy interpreter with the typed runtime.
  A translator may exist temporarily for migration comparisons.
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
- Existing `savor.capture.profile/1` semantics remain opaque behind passive `CaptureService`.
  `StopPointRouter` and `ExecutionEngine` own wake/control authority; capture observes the same routed
  sequence/snapshot/epoch identity so profile-visible control, window, recorder, progress, queue, and
  artifact behavior remains compatible.
- `StateService` is the only guest-epoch authority. State/movie continuation is exact verified evidence,
  not an ambient Dolphin query, and external import cannot infer movie mode.
- Slice 4 generic services are not capability packs. `soa.*` semantic points, queries, actions, and
  reducers arrive with Slice 5's typed runtime surface.

## Deferred boundaries

These questions are intentionally left to the implementation slice or future work that needs them:

- concrete C++ signatures, module binary encoding, canonical hash algorithm, and worker wire bytes;
- authored source syntax, editor, publishing, and any future persisted module catalog;
- any generalized replacement for the existing `savor.capture.profile/1` language;
- detailed Survey trigger behavior beyond the first bounded path;
- final teleport/settle implementation and tolerances;
- collision-oddity objectives and refinement policy;
- cutscene strategy;
- overworld movement, goals, pruning, and durable frontier rules; and
- external consumers of the legacy battle-path program that are not visible in this repository.

None of these permits a second executor, unscoped emulator mutation, direct program access to SavorDb,
or a change to SavorDb storage/interfaces inside this refactor.

## Research and domain references

- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
- `planning/NavigationPhase/NavigationContextWorkflow/07-open-research-questions.md`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt`
