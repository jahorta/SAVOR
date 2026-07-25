# 01 - Current System and Pressure Points

## Status and authority

**Status:** Current-state evidence and target-design input.

**Inspected snapshot:** SAVOR commit
`b584920ffad8dbe770f343e532d7f7386c82fadf` (`2026-07-25T14:22:17-05:00`,
`Model combatant auxiliary visual publication`), inspected on 2026-07-25.

Current code is authoritative for everything described as implemented in this document. The rest of the
Execution Runtime package is authoritative for the breaking-change target. Historical planning and the
breakpoint-router analysis are discovery aids; neither can establish that a current capability exists.

The worktree contained unrelated user changes when this snapshot was inspected. The inventory below was
derived from the named source files and symbols and does not treat those unrelated edits as part of this
refactor.

## Purpose and non-goals

This document establishes the execution system that actually exists before replacement work begins. It:

- follows a job from workflow scheduling through worker execution and result mapping;
- inventories the worker, registry, VM, context, macro, wire, descriptor, state, and workflow seams;
- classifies every implemented phase-program family;
- identifies strengths that the replacement must preserve; and
- identifies constraints that the replacement must remove rather than enshrine in a new ABI.

It does not propose compatibility shims, define the target IR in detail, or claim that a planning-only
phase has been implemented. In particular, **Navmesh Survey is not implemented**. Navigation Context is
implemented and has produced a concrete `.sav`/`.nctx` bootstrap, but that is the starting point for the
Survey rather than an implementation of it.

## Current end-to-end execution path

The current DB-backed path is:

1. SavorQt registers a `ProgramKindDescriptor` for supported workflow step kinds.
2. A workflow step adapter persists jobs and constructs a `RuntimeInitRequest`.
3. `JobMaterializationService` resolves the descriptor by step kind, invokes
   `IRuntimeInitAdapter::BuildRuntimeInit`, materializes a `PSJob`, and derives savestate and bootstrap
   affinity strings.
4. `DBWorkflowWorkerCoordinator::EnsureWorkerProgramForJob` compares the claimed job with the program
   kind, runtime affinity, and savestate affinity already cached in the worker slot.
5. When any of those values differ, the coordinator sends `MSG_SET_PROGRAM`, `MSG_RUN_INIT_ONCE`, and
   `MSG_ACTIVATE_MAIN`. The worker constructs the fixed program locally from the numeric `ProgramKind`.
6. For each `MSG_JOB`, the worker asks `ProgramRegistry` to decode the program-specific payload into a
   `PSContext`, adds the game ISO path, and calls `PhaseScriptVM::run`.
7. The VM restores its in-memory baseline, interprets the fixed operation list, and produces `PSResult`.
8. The worker serializes the result context through `PSContextCodec`, places it behind `WireResult`, and
   returns it to the coordinator.
9. The descriptor's result mapper and transition handler persist domain output, publish artifacts, and
   optionally append dynamic workflow steps.

This path already separates durable orchestration from the worker process. Its principal problem is not
the absence of a VM; it is that identity, typing, lifecycle, domain capabilities, and physical emulator
ownership are fused into the present VM and its surrounding switches.

## Current code evidence

### Worker process and command loop

`SavorWorker/SavorWorker.cpp` constructs one `DolphinWrapper`, one runtime breakpoint map, and one
`PhaseScriptVM`. The main thread reads a serial sequence of control and job messages:

- `MSG_SET_PROGRAM` stores `PSInit`, selects `main_kind`, and calls
  `programs::build_main_program`;
- `MSG_RUN_INIT_ONCE` runs `init_prog` only if it contains operations;
- `MSG_ACTIVATE_MAIN` calls `PhaseScriptVM::init`;
- `MSG_JOB` decodes the payload for the active kind and calls `PhaseScriptVM::run`.

The currently declared `init_prog` is never populated from `WireSetProgram::init_kind`; therefore the
current `MSG_RUN_INIT_ONCE` path is normally an acknowledged no-op. Program lifecycle is consequently
split among a wire ceremony, `PhaseScriptVM::init`, script operations, and job-level VM behavior rather
than one explicit invocation contract.

The main pipe loop is serialized, but visual-debug control is not. A separate visual thread calls
`PhaseScriptVM` pause/step methods and `DolphinWrapper` pause/resume/frame-step methods directly. The
target `WorkerRuntime` actor must absorb that control path so external commands cannot race session
execution.

### Fixed program and payload registry

`SavorCore/Phases/Programs/ProgramRegistry.cpp` has two central switches:

- `build_main_program(uint8_t)` maps a wire `ProgramKind` to one compiled `PhaseScript` builder;
- `decode_payload_for(uint8_t, bytes, PSContext&)` maps the same kind to one program-specific decoder.

The payload's first byte must equal the active program kind. Retry-tuning metadata is selected by another
kind switch and is not populated for every executable kind. There is no immutable module revision,
canonical program hash, named entrypoint, imported action closure, or typed input/output schema at this
worker boundary.

This registry is useful evidence that all fixed programs already share one interpreter. It is not a
suitable permanent definition store because adding a program changes central worker code and because
`ProgramKind` simultaneously selects definition, decoder, result expectations, and runtime affinity.

### `PhaseScript` and `PhaseScriptVM`

`PhaseScript` currently contains:

- a canonical breakpoint-key vector;
- a gated breakpoint-key vector; and
- a flat `PSOp` vector.

`PSInit` contains one optional savestate path, a default timeout, and one `DBuf` selector. `PSJob` is a
payload plus `PSContext`. `PSResult` is a Boolean, an 8-bit worker error, and another `PSContext`.

The opcode catalog contains 52 ordinals, `0` through `51`. The central dispatch switch mixes:

- language/control operations such as labels, branches, constants, and return;
- emulator effects such as run-until, stepping, movie control, savestate operations, and memory access;
- breakpoint, watchpoint, predicate, and capture management;
- domain operations such as battle-context extraction, battle-path materialization, battle macro
  materialization/execution, TAS input sampling, seed-override capture, results/completion macro
  materialization, and Navigation Context extraction.

`PhaseScriptVM` is not only an interpreter. It directly:

- loads and snapshots state;
- arms, enables, disables, disarms, and clears physical breakpoints and watchpoints;
- advances Dolphin by frames, opcodes, input tapes, and run-until calls;
- publishes controller input and tracks input acknowledgements;
- starts/stops movies and capture jobs;
- reads and writes guest memory;
- owns macro-host interfaces and the macro-exclusive-session concept;
- performs battle/navigation queries and derived-buffer updates;
- gates visual debugging; and
- constructs transport-facing context results.

This is the central ownership pressure. Adding a more expressive program format without first removing
these responsibilities would preserve the same god object behind a new serializer.

### State bootstrap and reset behavior

`PhaseScriptVM::init` optionally loads one disk savestate, installs the new program's breakpoint sets, and
captures one in-memory snapshot. `PhaseScriptVM::run` restores that snapshot before interpreting the job.
Most state-based built-in scripts then execute `LOAD_SNAPSHOT` near their entrypoint, producing a second
restore. Battle Single Turn can execute another `LOAD_SNAPSHOT` for its local retry.

The coordinator may reuse a warm worker when program, runtime, and savestate affinity match. Affinity is
an optimization hint, not an assertion that the guest or host runtime is clean. There is no public
`StateEpoch`, no collection of immutable state handles, and no general invalidation rule for pointers or
subscriptions after restore.

### `PSContext`, keys, and codec

`PSContext` is an unordered map from a global 16-bit `KeyId` to a closed C++ variant:

- `u8`, `u16`, `u32`, `float`, `double`, and `string`;
- `GCInputFrame`; and
- `soa::battle::actions::BattlePath`.

`CtxRegistry` assembles globally allocated key ranges from VM, SeedProbe, TAS, battle, results,
completion, and Navigation Context registries. `PSContextCodec` serializes the same closed variant into a
generic tagged blob and silently skips an unsupported in-memory variant during encoding.

This gives the current system stable numeric names and a reusable job/result carrier. It also makes the
generic runtime depend directly on battle and controller types, gives inputs, locals, outputs, and
diagnostics the same unscoped map, and requires global key-range/code changes when a new domain needs a
new shape.

### Input macro subsystem

`InputMacroRuntime` is a second execution mechanism hosted inside `PhaseScriptVM`. A validated
`InputMacroPlan` executes breakpoint waits, neutral frames, memory baselines, and memory-change waits.
Adaptive providers use `IInputMacroPlanDriver::Start` and `Advance` to produce the next segment from the
latest stop/result.

The subsystem has valuable behavior:

- provider-declared breakpoint permissions;
- explicit terminal/failure states;
- an adaptive continuation boundary;
- input-poll receipts; and
- cleanup-once semantics that neutralize input, clear macro watchpoints, release the local exclusive
  session, and restore breakpoint state.

Its present ownership is nevertheless local to the VM. The VM implements the host by replacing the
entire enabled-breakpoint set, directly advancing Dolphin, and publishing pad state. `Start/Advance`
therefore becomes the seed for the target continuation/action model, not a peer `InputMacroEngine`.

### Worker wire protocol

`SavorCore/Runner/IPC/Wire.h` exposes a fixed binary protocol:

- lifecycle messages identify `init_kind` and `main_kind` as 8-bit `PK_*` values;
- `WireSetProgram` carries one timeout, one derived-buffer kind, and a fixed 260-byte savestate path;
- `WireJobHeader` carries job ID, epoch, and opaque payload length;
- `WireResult` carries transport `ok`, an 8-bit error, and one context-blob length.

The current enum defines `PK_SeedProbe` through `PK_NavigationContextRunner` with stable ordinals.
`PK_BattleEndResultsRunner` is only a source alias for the split Results Screen kind. The protocol cannot
identify an immutable program revision/hash, named entrypoint, dependency closure, typed schemas,
multiple artifacts, or separate infrastructure/domain/cleanup statuses.

### DB program descriptors and workflow spawning

`ProgramKindDescriptor` combines:

- numeric program kind and display name;
- job and graph persistence;
- runtime initialization and `PSJob` materialization;
- result mapping and result-artifact mapping;
- result payload writing;
- workflow transition behavior; and
- mixed-success policy.

`ProgramKindRegistry` indexes descriptors both by numeric kind and by workflow step kind. This is a useful
adapter seam, but it currently makes worker execution identity and workflow/domain integration look like
one concept.

Durable orchestration already supports dynamic fan-out. `WorkflowTransitionDecision::spawn_steps`
describes children, and `WorkflowTerminalAdvancementService` persists them through
`AppendDynamicSteps`. Battle Context emits initial Battle Single Turn steps; Battle Single Turn chooses
survivors, creates subsequent waves, and emits another set of `battle.single_turn` steps. SeedProbe
similarly expands its chain into grid and unique work.

This is concrete evidence that waves and arbitrary successor scheduling belong above the worker. It does
not yet provide a general persisted frontier record with node lineage, deduplication, strategy, and
terminal policy.

## Implemented phase-program taxonomy

“Implemented” below means the fixed script builder and payload decoder are present in current source. A
DB registration is called out separately because not every compiled worker program is available through
the current workflow registry.

| Family | Current worker identity | Current shape and outputs | DB/workflow exposure |
|---|---|---|---|
| SeedProbe | `PK_SeedProbe` (`1`) | Restores baseline, applies one input, reaches the pre-battle or field-return RNG checkpoint, reads RNG, optionally verifies and materializes a savestate, emits the seed | Registered as `seed_probe_chain`, `seedprobe.neutral`, `seedprobe.grid`, and `seedprobe.unique`; transition handlers create later chain steps |
| TAS playback | `PK_TasMovie` (`2`) | Validates disc identity, starts DTM playback, runs to the configured stop, stops the movie, saves a savestate, and returns movie failure status | Registered as `tas_movie` and `tasmovie.play` |
| Legacy multi-turn battle path | `PK_BattleTurnRunner` (`3`) | Interprets a multi-turn `BattlePath`, materializes and applies input frames, evaluates predicates, and returns victory/defeat/turn-limit/failure outcome | Compiled worker program and payload exist; no active `SavorDb` descriptor registration was found in the inspected tree |
| Battle Context | `PK_BattleContextProbe` (`4`) | Restores a battle entry state, reaches `TurnInputs`, captures and emits `BattleContext` | Registered as `battle.context_probe` and the bootstrap step kind `battle_chain`; successful transitions spawn `battle.single_turn` children |
| Battle Single Turn | `PK_BattleSingleTurnRunner` (`5`) | Optionally overrides starting RNG, reaches turn input, materializes an adaptive command macro, confirms input acceptance, evaluates predicates/watchpoints, records ending RNG/context, and conditionally saves successor state | Registered as `battle.single_turn`; transition code chooses survivors and appends subsequent turn-wave steps |
| TAS input-stream detection | `PK_TasInputStreamDetector` (`6`) | Plays a DTM, samples input once per stepped frame until movie end, then stops and returns status | Compiled worker program and payload exist; no active `SavorDb` descriptor registration was found |
| Battle Macro Probe | `PK_BattleMacroProbe` (`7`) | Materializes a battle macro plan, repeatedly executes adaptive macro segments, and performs an observation tail | Compiled worker program; exercised by direct-worker SavorE2E scenarios; no active `SavorDb` descriptor registration was found |
| Battle Results Screen | `PK_BattleResultsScreenRunner` (`8`) | Validates a completion manifest through its provider, advances the results presentation adaptively, preserves required invariants, and saves the field-return state | Registered as `battle.results_screen`; the old `PK_BattleEndResultsRunner` name is only an alias |
| Battle Completion | `PK_BattleCompletionRunner` (`9`) | Starting from victory, runs the adaptive completion provider, captures pre/post rewards and manifest data, and saves the completion state | Registered as `battle.completion`; participates in the hidden battle-end workflow sequence |
| Navigation Context | `PK_NavigationContextRunner` (`10`) | Reaches or validates the fixed initial-player-input capture point, captures the navigation context, saves the matching savestate, and emits `.nctx` content with explicit failure codes | Registered as `navigation.context_probe`; this is the implemented bootstrap for later Survey work |
| Navmesh Survey | None | No worker kind, fixed program, payload codec, action set, result type, DB descriptor, or executing workflow exists | Planning only. The hidden `dungeon_explorer`/overworld placeholders do not implement Survey behavior |

The implemented Navigation Context bootstrap has an observed output pair at:

`C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`

and:

`C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

The pair is evidence that the bootstrap exists. It does not close any Survey implementation gap.

## Strengths to preserve

The clean-slate replacement must preserve these proven ideas:

1. **One ordinary worker path.** Fixed programs already share one worker and one interpreter rather than
   launching a process type per phase.
2. **Bounded job execution.** Programs consume one job and return; durable continuation is already outside
   the worker.
3. **Declarative control-flow definitions.** Even complex phases expose much of their sequencing in
   builders rather than hard-coding a complete controller in `SavorWorker`.
4. **Adapter-chain separation.** DB materialization, result persistence, and workflow transition logic
   are not performed by the worker.
5. **Typed low-level observations.** Breakpoint keys, address registries, input receipts, capture reports,
   Navigation Context codecs, and state artifact records provide useful reusable evidence.
6. **Adaptive macro continuation.** Provider `Start/Advance` and cleanup-once behavior establish a viable
   model for interruptible effect composition.
7. **Dynamic workflow fan-out.** Current battle and seed workflows prove that the coordinator can append
   durable successor work after reduction.
8. **Warm-worker optimization.** Affinity-aware reuse is valuable as long as it is not confused with
   state reset, correctness, or exact program identity.

## Pressure points that force replacement

| Pressure point | Current consequence | Required replacement property |
|---|---|---|
| Numeric `ProgramKind` is execution identity | Central switches choose program, decoder, result path, and affinity | Immutable `ProgramModule` identity plus named entrypoint and dependency closure |
| Domain behavior is an opcode | Every new domain can grow the central interpreter switch | Small stable IR plus registered typed actions and subprograms |
| `PhaseScriptVM` owns Dolphin facilities | Programs, macros, capture, and visual control cannot compose safely | Session services with one `ExecutionEngine`, router, input arbiter, and scoped resources |
| `PSContext` is one global map | Inputs, locals, output, errors, and domain records share one closed variant and key registry | Entry-point schemas and scoped typed values/records/lists/artifact references |
| Result is `bool + error + context` | Domain failure and cleanup failure can be collapsed into transport success/failure | Separate infrastructure, domain, and cleanup/session status |
| Baseline restore is implicit and duplicated | Init, `run`, script ops, and retries all participate in reset semantics | Declarative invocation state policy and explicit epoch changes |
| Macro execution is a subordinate runtime | Adaptive control has a second scheduler and private physical ownership | Common action-await continuation model under `ProgramExecutor` |
| Breakpoint state is replaced globally | Independent observers/interceptors cannot remain composed | Logical subscriptions and one physical stop-point owner |
| Worker visual thread calls runtime directly | External controls can bypass command serialization | All commands routed through `WorkerRuntime` |
| Descriptor mixes execution with workflow integration | A new phase appears to require another descriptor/controller combination | Separate program catalog/schema registry from workflow adapters and bindings |
| Affinity uses kind/bootstrap strings | Cache locality can be mistaken for exact revision or clean state | Hash/dependency-aware locality plus explicit state/session policy |
| No general mutation ledger | Data writes and future code patches lack one restoration/taint contract | Checked scoped `GuestMutationService` receipts and mandatory unwind |

## Locked target decisions

The current evidence locks these conclusions:

- The replacement remains one universal program runtime; it does not introduce a native phase runner.
- `ProgramInstance` is execution state, not a polymorphic controller.
- `PhaseScript` builders are temporary compiler inputs for parity, not the permanent wire or authored ABI.
- Current domain operations migrate into actions, reducers, queries, or reusable subprograms; they do not
  receive corresponding core IR opcodes.
- `IInputMacroPlanDriver::Start/Advance` is generalized into program continuations and awaited actions.
  `InputMacroRuntime` is not retained as a peer program executor.
- Worker-session services absorb every direct `DolphinWrapper` operation currently performed by the VM
  or visual-control thread.
- Workflow transition and dynamic-step capabilities are retained and generalized into typed bindings and
  first-class frontier orchestration.
- `ProgramKind` may remain for UI/history/compatibility metadata but ceases to select worker execution.
- Navigation Context migrates as an ordinary current phase. Navmesh Survey is the first net-new consumer
  after current behavior reaches the new runtime.

## Interfaces and ownership affected

The replacement crosses these current seams:

- `SavorWorker.cpp` control loop and visual command path;
- `ProgramRegistry` fixed construction, decoding, and retry metadata;
- `PhaseScript`, `PSInit`, `PSJob`, `PSResult`, `PSContext`, and `PSContextCodec`;
- the 52-opcode dispatch and all split `PhaseScriptVM*` host implementations;
- `InputMacroRuntime`, `IInputMacroPlanDriver`, and providers;
- `WireSetProgram`, job/result envelopes, and `ProgramKind`;
- `ProgramKindDescriptor`, `ProgramKindRegistry`, and runtime-init/result adapters;
- worker affinity/materialization and `EnsureWorkerProgramForJob`; and
- dynamic workflow transition and successor-step publication.

The migration must not push workflow persistence into the worker or move emulator ownership into DB
adapters. Those existing boundaries are retained and made more explicit.

## Failure and cleanup behavior

Current code contains several local cleanup guards:

- VM job exit clears memory watchpoints and stops the probe job;
- macro cleanup neutralizes input, clears macro watchpoints, releases its local session, and restores
  breakpoint state;
- VM init cancels a prior macro and disarms the prior program's recorded breakpoint set.

Those are behavior to preserve, but they do not form one complete resource model. Movie state, input
ownership, breakpoint replacement, capture, state restore, guest writes, and future executable patches
do not share one unwind stack or one cleanup status. Some cleanup is global destructive clearing rather
than releasing resources owned by one source.

The target must:

- run the same structured unwind for return, handled domain failure, infrastructure failure, timeout,
  cancellation, and verifier/guard abort after acquisition;
- distinguish a normal domain failure from infrastructure failure;
- report cleanup independently from both;
- neutralize/release input and restore every owned subscription and mutation;
- advance `StateEpoch` on every restore and reject stale handles; and
- taint and retire a session when mandatory cleanup cannot be verified.

## Dependencies and migration implications

The current programs cannot be migrated safely by serializing today's `PhaseScript` first. The
dependency order is:

1. establish session-service ownership and typed execution results;
2. define and verify the small core IR and typed module/invocation/result contracts;
3. expose current capabilities through registered actions and reusable subprograms;
4. add a temporary compiler/translator for current builders;
5. migrate and differentially verify every current phase family;
6. move workflow activation and affinity to exact module identity;
7. remove current worker switches, domain opcodes, and the subordinate macro scheduler; and
8. implement Navmesh Survey only on the new path.

The current dynamic battle-wave implementation is a migration asset: it provides executable examples for
separating bounded worker expansion from durable orchestration.

## Acceptance criteria

This current-state document remains valid only while:

- every implemented phase in `ProgramRegistry::build_main_program` appears in the taxonomy;
- each DB exposure claim matches actual registration code;
- Navigation Context and Navmesh Survey are not conflated;
- no current-state claim is justified solely by a planning document;
- the central opcode count, context variant, worker lifecycle, and warm-reuse behavior match inspected
  code; and
- new implementation evidence is added to `12-source-evidence-map.md` and reconciled here.

The future refactor has addressed these pressure points when adding a phase from existing capabilities
requires no change to `SavorWorker`, `ProgramRuntime`, `ProgramExecutor`, `ExecutionEngine`, the
stop-point router, transport core, or a central opcode switch.

## Deferred work

This document does not decide:

- exact C++ interface spelling or source directory layout;
- final transport bytes or SQL schema;
- the authored script syntax/UI;
- final module revision/hash algorithms;
- Survey-specific action algorithms;
- generalized `eventhook` trigger handling;
- collision anomaly objectives;
- overworld game rules; or
- cutscene acceleration policy.

Those decisions are either defined elsewhere in this package at a logical-contract level or explicitly
deferred in document 11.

## Source references

Primary implemented-code evidence:

- `SavorWorker/SavorWorker.cpp`
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Runner/Script/PhaseScriptProgram.h`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
- `SavorCore/Runner/Script/PhaseScriptVM.h`
- `SavorCore/Runner/Script/PhaseScriptVM.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMControl.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMInput.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMMacro.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMMemory.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMNavigation.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMPredicates.cpp`
- `SavorCore/Runner/Script/PSContext.h`
- `SavorCore/Runner/Script/PSContextCodec.cpp`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp`
- `SavorCore/Runner/IPC/Wire.h`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
- `SavorDb/Execution/ProgramDB/ProgramKindRegistry.h`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorDb/Execution/ProgramDB/BattleContext/BattleContextProbeAdapters.cpp`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
- `SavorDb/Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.cpp`
- `SavorDb/Execution/Workflow/WorkflowComposition.cpp`
- `SavorCore/Phases/Programs/*/*Script.h`
- `SavorCore/Phases/Programs/*/*Payload.h`

Research and domain-planning evidence:

- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt`
- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`

See `12-source-evidence-map.md` for claim-level classification and conflict resolution.
