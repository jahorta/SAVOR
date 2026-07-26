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
  - constructs the Dolphin host and current `PhaseScriptVM`;
  - receives activation, initialization, job, and visual-control commands; and
  - currently permits visual control to reach VM/host operations outside one serialized command actor.
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
  - implements the current interpreter and directly owns broad Dolphin, input, breakpoint, state,
    capture, movie, memory, and game-context behavior.
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
