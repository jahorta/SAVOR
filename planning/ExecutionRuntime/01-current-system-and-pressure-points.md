# 01 - Current System and Pressure Points

## Purpose and non-goals

This document is an orientation map of the execution system inspected before replacement work began.
Current code and executable behavior win if this inventory drifts. It:

- follows a job from workflow scheduling through worker execution and result mapping;
- inventories the worker, registry, VM, context, macro, wire, descriptor, state, and workflow seams;
- classifies every implemented phase-program family;
- identifies strengths that the replacement must preserve; and
- identifies constraints that the replacement must remove rather than enshrine in a new ABI.

It does not propose a worker/runtime compatibility shim, define the target IR in detail, or claim that
a planning-only phase has been implemented. The private neutral SQLite insert bindings required while
the retained timing columns await a separate migration are schema compatibility only. In particular,
**Navmesh Survey is not implemented**. Navigation Context is implemented and has produced a concrete
`.sav`/`.nctx` bootstrap, but that is the starting point for the Survey rather than an implementation
of it.

## Current end-to-end execution path

The current DB-backed path is:

1. SavorQt registers a `ProgramKindDescriptor` for supported workflow step kinds.
2. A workflow step adapter persists jobs and constructs a `RuntimeInitRequest`.
3. `JobMaterializationService` resolves the descriptor by step kind, invokes
   `IRuntimeInitAdapter::BuildRuntimeInit`, materializes a `PSJob`, and derives savestate and bootstrap
   affinity strings.
4. The production coordinator runtime materializes each claimed job into an immutable workset with a complete exact
   artifact baseline. Worker placement carries no guest-state meaning.
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
ownership are fused into the present VM and its surrounding switches. It also leaves every worker slot
with one in-flight job: after each result, the coordinator must process that job and submit another before
the worker can continue, even when several independent jobs have the same program, runtime, and source
state.

The current path also carries phase-specific elapsed execution policy through authoring
`run_ms`/`vi_stall_ms` columns, payload fields, context keys, fingerprints, runtime-init requests,
SIMMER predictor overrides, E2E setup, and worker operation budgets. Those values conflate valid slow guest
execution with an unhealthy emulator and cannot account correctly for synchronous router, sampler, or
capture work that temporarily blocks guest advancement. The pre-6A hard cutover removes this timing
family rather than translating it into the new architecture.

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

### Current predicate subsystem

The current battle predicate path is reusable in intent but not in ownership. `Predicate::Spec` combines
the evaluation point, breakpoint requirements, guest-address or address-program operands, optional delta
baselines, comparison, display identity, and `AbortOnFail`. `PhaseScriptVM` then:

- arms the predicate breakpoint set;
- captures baselines and reads guest values;
- performs the comparison;
- emits predicate progress text;
- updates passed/total counters; and
- sets VM-global abort state that the battle script converts into a predicate-failure outcome.

Battle Single Turn's result adapter projects the counters and abort outcome through the existing battle
result representation, and its current survivor selection uses passed counts as one ranking input. The
useful idea to preserve is a named condition that can observe, reject early, or contribute progress. The
coupling to physical breakpoints, Dolphin reads, VM-global state, reporting, and battle-only policy is not
the reusable boundary.

### Current observation and capture surfaces

The ingredients for semantic observation exist, but are split across subsystem-specific contracts.
Breakpoint keys name some logical program points, predicate records combine those points with direct or
address-program guest reads, input macros return stop/input receipts and keep local memory baselines, and
game adapters expose coherent domain queries. None of these currently provides one reusable definition
for "await this logical point, then acquire this typed evidence" with explicit hit-time, paused, ordering,
availability, baseline, and workset-epoch semantics.

`SavorProbe/ProbeProfile.*`, `AddressProgramEvaluator.h`, and `ProbeRuntime.*` separately implement the
existing `savor.capture.profile/1` capture language: PC and memory probes, filters and address programs,
sampling policies, windows, flight recorders, queues, progress delivery, control-facing flags, and
artifact production. Those profile semantics are valuable compatibility behavior, but the current probe
runtime also contains trusted control-wait authority. The target preserves the profile language behind a
passive `CaptureService`; router/execution ownership replaces its control authority without turning
capture profiles into the semantic-observation language.

### State bootstrap and reset behavior

`PhaseScriptVM::init` optionally loads one disk savestate, installs the new program's breakpoint sets, and
captures one in-memory snapshot. `PhaseScriptVM::run` restores that snapshot before interpreting the job.
Most state-based built-in scripts then execute `LOAD_SNAPSHOT` near their entrypoint, producing a second
restore. Battle Single Turn can execute another `LOAD_SNAPSHOT` for its local retry.

The worker issues `SessionId` and `WorksetEpoch` bindings for action/resource safety, but the coordinator
does not supply either as a state input. A later workset always materializes its declared artifacts;
temporary state handles exist only inside one active multi-item workset.

The target keeps the correctness boundary at one workset. A host-only staged successor may hash and
verify its own declared artifacts while another workset uses the session, but it cannot acquire guest
state or a state handle. Promotion then materializes the active workset's exact source state. A multi-item active
`WorkerWorkset` retains one composite `ProgramBaselineDefinition` at workset scope. Its ordered
`ProgramBaselineComponent`s include the savestate, exact movie continuation, and runtime-facing
program-kind adapter-declared derived state. `ProgramBaselineKey` identifies that complete definition,
and every later child starts only after one `PreparedProgramBaselineReceipt` proves the composite
restore. Each successful restore retains the workset's existing `WorksetEpoch`; the handle belongs only to
the active workset, and no item-local receipt or resource crosses that boundary.

That paragraph describes the legacy execution corpus, which remains compiled only as production-
disconnected, read-only orientation and deletion evidence during the hard cutover. It is never invoked
as an executable reference or differential oracle. Dependency Slice 4 now gives
`EmulationSession::BeginWorkset` is the sole `WorksetEpoch` authority. Each active workset owns a
`SavestateService` with bounded private memory handles and caller-declared immutable
read-only file artifacts with SHA-256, runtime/disc compatibility, lineage, embedded/hash-verified DTM
history, and active-DTM identity checks on restore. Recording checkpoints are limited to workset-local
memory handles. Baseline restoration is coordinated outside the byte-owning service and retains the
workset epoch; recoverable failure rolls back staged movie/input/ingress state, while unknown integrity
taints the session.

The same checkpoint adds session-owned `InputArbiter`, `GuestMemory`, `GuestMutationService`,
`MovieService`, `CaptureService`, `ScreenshotService`, `TelemetryBus`, and a standalone
`SessionResourceLedger`. These are generic runtime facilities, not restored program execution.
`ProgramInvocation` and game capability packs remain unavailable until later slices.

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
- stop-sequence, input-epoch, requested-input, input-poll, and acknowledgement receipts;
- the historical held-through-hit mechanism's semantic intent: same-publication continuation from a
  suppressed source receipt to a declared successor; and
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

The implemented hard-cutover WRMS seam and typed runtime currently model one encoded invocation and one
terminal result. Production invocation is still disabled, so the target replaces that unactivated scalar
submission with `SubmitWorkset`: one bounded envelope containing one or more independent invocation
templates and non-lossy per-item terminals. The pipelined target permits one active session-mutating
workset and one immutable host-only staged successor, while a separate worker-global completion/
acknowledgement ledger retains completed execution records, promoted immutable output captures and
pending finalizers, and authoritative terminals until exact durable acknowledgement. WRMS remains
version 1 and contains only the workset program-submission command.

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

The production composition prelude now centralizes the complete current descriptor catalog in
`SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.*`. The factory owns the fixed registration
order, canonical numeric winners, all sixteen production step-kind mappings, descriptor-capability
validation, and production working-directory defaults. SavorQt and every DB-backed SavorE2E scenario
construct that same complete catalog. SavorE2E still owns one shared `DBService`, but admits only one
scenario's coordinators at a time and checks workflow quiescence before and after each DB-backed
scenario and repeat. `battle_macro_probe` remains a direct-worker development scenario and has no
SavorDb descriptor or workflow step kind.

Durable orchestration already supports dynamic fan-out. `WorkflowTransitionDecision::spawn_steps`
describes children, and `WorkflowTerminalAdvancementService` persists them through
`AppendDynamicSteps`. Battle Context emits initial Battle Single Turn steps; Battle Single Turn chooses
survivors, creates subsequent waves, and emits another set of `battle.single_turn` steps. SeedProbe
similarly expands its chain into grid and unique work.

This is concrete evidence that waves and arbitrary successor scheduling belong above the worker. Current
workflow persistence remains a fixed integration boundary for this refactor. Current claims already
distinguish `CLAIMED` from `RUNNING` and can reserve several ready jobs. Workset support may change how
the coordinator groups, accounts for, renews, starts, and drains those independently durable jobs, but it
does not create a persisted workset or move successor decisions into the worker. A generalized persisted
frontier record or policy is a separate future project.

The current worker coordinator also exposes process-order costs that the hard cutover does not need to
preserve. Initial `Start()` preflights and opens every desired worker serially before any data-plane
thread starts, even though later pool repair already has a bounded concurrent-start facility. Dispatch
then performs savestate materialization and program setup only after an idle worker is selected, and the
single result drainer retains that worker until result mapping and persistence finish. The pre-6A
process seam therefore implements the eventual one-compatible-worker gate and progressive startup with
at most two concurrent launches without opening DB work; Slice 7 applies the gate only after
`CompleteExact` proves exactly the two production Full Phase modules and no extras. The same target stages one
immutable successor package while the active workset runs and
moves terminal retention out of active workset/session ownership.

The database/coordinator hot path is likewise only batch-shaped at its surface today:
`ClaimBatchReadyExecutionJobs` loops scalar claim transactions, lease maintenance renews one job at a
time, and materialized selection scans the candidate map with per-candidate job reads. The workflow
coordinator uses timed polling and a fixed active-workflow-count throttle instead of actual
coordinator/worker item residency. The target uses one ordered claim transaction, exact-set lease
renewal, indexed deterministic workset assembly, targeted post-commit terminal wakeups with periodic
repair scans, and item-capacity credits derived from the complete pipeline.

## Supported current-source inventory

“Implemented” below means the current source contains behavior that must be reconstructed by a direct
native typed-module builder. A DB registration is called out separately because the two diagnostic
programs are not available through the current workflow registry. Rows follow the current numeric worker
identity for source orientation; the authoritative implementation order is Slices 6A-6I below.

| Family | Current worker identity | Current shape and outputs | DB/workflow exposure |
|---|---|---|---|
| SeedProbe | `PK_SeedProbe` (`1`) | Restores a paused baseline, classifies its authoritative entry PC, applies one input, reaches the one corresponding pre-battle or field-return RNG checkpoint, reads RNG, optionally verifies and materializes a savestate, and emits the seed | Registered as one composable and standalone-launchable `seed_probe` workflow unit whose `seedprobe.survey`, `seedprobe.search`, and `seedprobe.confirm` stages are separate workflow steps with flat job sets |
| TAS playback | `PK_TasMovie` (`2`) | Validates disc identity, starts DTM playback, runs to the configured stop, stops the movie, saves a savestate, and returns movie failure status | Registered as `tas_movie` and `tasmovie.play` |
| Battle Context | `PK_BattleContextProbe` (`4`) | Restores a battle entry state, reaches `TurnInputs`, captures and emits `BattleContext` | Registered as `battle.context_probe`; the public `battle` unit joins it with a confirmed SeedProbe run and one direct `authoring.battle_plan`, then `battle.start` spawns `battle.single_turn` children |
| Battle Single Turn | `PK_BattleSingleTurnRunner` (`5`) | Optionally overrides starting RNG, reaches turn input, materializes an adaptive command macro, confirms input acceptance, evaluates predicates/watchpoints, records ending RNG/context, and conditionally saves successor state | Registered as `battle.single_turn`; transition code chooses survivors and appends subsequent turn-wave steps |
| TAS input-stream detection | `PK_TasInputStreamDetector` (`6`) | Plays a DTM, samples input once per stepped frame until movie end, then stops and returns status | Compiled worker program and payload exist; no active `SavorDb` descriptor registration was found |
| Battle Macro Probe | `PK_BattleMacroProbe` (`7`) | Materializes a battle macro plan, repeatedly executes adaptive macro segments, and performs an observation tail | Compiled worker program; exercised by direct-worker SavorE2E scenarios; no active `SavorDb` descriptor registration was found |
| Battle Results Screen | `PK_BattleResultsScreenRunner` (`8`) | Validates a completion manifest through its provider, advances the results presentation adaptively, preserves required invariants, and saves the field-return state | Registered as `battle.results_screen`; the old `PK_BattleEndResultsRunner` name is only an alias |
| Battle Completion | `PK_BattleCompletionRunner` (`9`) | Starting from victory, runs the adaptive completion provider, captures pre/post rewards and manifest data, and saves the completion state | Registered as `battle.completion`; participates in the hidden battle-end workflow sequence |
| Navigation Context | `PK_NavigationContextRunner` (`10`) | Reaches or validates the fixed initial-player-input capture point, captures the navigation context, saves the matching savestate, and emits `.nctx` content with explicit failure codes | Registered as `navigation.context_probe`; this is the implemented bootstrap for later Survey work |

The compiled `PK_BattleTurnRunner` (`3`) multi-turn `BattleRunner` has no active SavorDb descriptor in the
inspected tree and is not in the supported migration catalog. Its builder, payload, outcomes, and tests
remain only as orientation for deleting the disconnected legacy surface. They do not justify a
`soa.battle.legacy_path` module.

There is also no monolithic BattleEndResults migration target. `PK_BattleEndResultsRunner` is source
compatibility naming for kind `8`, which is the split Battle Results Screen phase; Battle Completion is
the independent kind `9` phase. Shared `BattleEndResults` directory, adapter, scenario, and test names
remain source-navigation and compatibility evidence for those two supported phases.

Navmesh Survey has no worker kind, fixed program, payload codec, action set, result type, DB descriptor,
or executing workflow. It remains planning-only future work rather than part of current-phase migration.

The implemented Navigation Context bootstrap has an observed output pair at:

`C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`

and:

`C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

The pair is evidence that the bootstrap exists. It does not close any Survey implementation gap.

## Strengths to preserve

The clean-slate replacement must preserve these proven ideas:

1. **One ordinary worker path.** Fixed programs already share one worker and one interpreter rather than
   launching a process type per phase.
2. **Bounded atomic execution.** Each program invocation consumes one job and returns one independently
   authoritative result; durable continuation remains outside the worker even when several invocations
   share a transient dispatch workset.
3. **Declarative control-flow definitions.** Even complex phases expose much of their sequencing in
   builders rather than hard-coding a complete controller in `SavorWorker`.
4. **Adapter-chain separation.** DB materialization, result persistence, and workflow transition logic
   are not performed by the worker.
5. **Typed low-level observations.** Breakpoint keys, address registries, input receipts, capture reports,
   Navigation Context codecs, and state artifact records provide useful reusable evidence.
6. **Adaptive macro continuation.** Provider `Start/Advance` and cleanup-once behavior establish a viable
   model for interruptible effect composition.
7. **Capture-profile behavior.** Existing profile parsing, address programs, filters, sampling, windows,
   flight recorders, progress, queue behavior, and artifact production are established semantics rather
   than a new authoring problem for this refactor.
8. **Dynamic workflow fan-out.** Current battle and seed workflows prove that the coordinator can append
   durable successor work after reduction.
9. **Artifact-atomic placement.** Process/module affinity may reduce compilation overhead, but it never
   skips artifact materialization or guest-state establishment. A bounded workset and one host-only
   staged successor preserve per-item reset and result boundaries.
10. **Exact state/movie continuation.** Dolphin's state/movie relationship is preserved as a single
    typed checkpoint: immutable state evidence plus the exact DTM identity/bytes and continuation
    counters, rather than an inferred ambient movie.

## Pressure points that force replacement

| Pressure point | Current consequence | Required replacement property |
|---|---|---|
| Numeric `ProgramKind` is execution identity | Central switches choose program, decoder, result path, and affinity | Immutable `ProgramModule` identity plus named entrypoint and dependency closure |
| Domain behavior is an opcode | Every new domain can grow the central interpreter switch | Small stable IR plus registered typed actions and subprograms |
| `PhaseScriptVM` owns Dolphin facilities | Programs, macros, capture, and visual control cannot compose safely | Session services with one `ExecutionEngine`, router, input arbiter, and scoped resources |
| `PSContext` is one global map | Inputs, locals, output, errors, and domain records share one closed variant and key registry | Entry-point schemas and scoped typed values/records/lists/artifact references |
| Result is `bool + error + context` | Domain failure and cleanup failure can be collapsed into transport success/failure | Separate infrastructure, domain, and cleanup/session status |
| Baseline restore is implicit and duplicated | Init, `run`, script ops, and retries all participate in reset semantics | One exact artifact baseline restored by the active workset under one immutable epoch |
| State files and movies are selected from ambient paths | A state can be restored without proving compatibility, lineage, or exact DTM continuation | Caller-declared immutable artifacts with SHA-256, compatibility, lineage, and explicit external movie-import policy |
| Macro execution is a subordinate runtime | Adaptive control has a second scheduler and private physical ownership | Common action-await continuation model under `ProgramExecutor` |
| Logical points, guest-address derivation, reads, baselines, and receipts are subsystem-specific | Predicates, macros, and future phases would reconstruct subtly different observation timing | Shared semantic-observation composition that lowers exact awaits, typed reads/queries, baselines, and emissions into ordinary runtime facilities |
| Predicate definition mixes trigger, observation, comparison, progress, scoring, and abort policy | Reuse outside battle would copy VM/Dolphin coupling or create another mini-runtime | Shared predicate composition library that separates pure conditions from use policy and lowers to ordinary IR, actions, scoped router subscriptions, and emissions |
| Capture profiles and trusted control waits share one probe runtime | Preserving capture could retain a second source of execution authority | Preserve `savor.capture.profile/1` behind passive `CaptureService`; route all wake/control through the router and execution engine |
| Breakpoint state is replaced globally | Independent observers/interceptors cannot remain composed | Logical subscriptions and one physical stop-point owner |
| Worker visual thread calls runtime directly | External controls can bypass command serialization | All commands routed through `WorkerRuntime` |
| Descriptor mixes execution with workflow integration | A new phase appears to require another descriptor/controller combination | Keep existing SavorDb contracts; adapt only runtime-facing handler behavior to construct/consume typed runtime contracts |
| Affinity uses kind/bootstrap strings | Cache locality can be mistaken for exact revision or clean state | Verify exact runtime module/state/session identity after current materialization without changing stored affinity or claim data |
| Initial pool startup is serial and gates every data-plane thread | Dolphin boot cost grows with the complete desired pool before useful work can begin | Establish a one-compatible-worker gate and at-most-two progressive startup before 6A; open the data plane only after Slice 7 proves `CompleteExact` |
| State/program preparation begins only after a worker becomes idle | Immutable reads, hashing, and decoding create a cold handoff bubble | One host-only staged successor package that verifies its own artifacts; session mutation remains serialized after promotion |
| One parent submission per worker result | Compatible jobs incur repeated dispatch decisions between items | One active finite static `WorkerWorkset`, one host-only staged successor, sequential child admission, and streamed per-item terminals |
| Terminal retention is workset-scoped | A clean successor can remain blocked solely because an earlier terminal awaits durable acknowledgement | One bounded worker-global completion/acknowledgement ledger independent of the active workset/session scope |
| No general mutation ledger | Data writes and future code patches lack one restoration/taint contract | Checked scoped `GuestMutationService` receipts and mandatory unwind |
| Resource cleanup is distributed across subsystem guards | Cleanup order and retry after partial release are inconsistent | One workset-owned `SessionResourceLedger` with typed receipts, reverse-order unwind, and taint disposition |

## Target direction

The current evidence locks these conclusions:

- The replacement remains one universal program runtime; direct native phase builders produce verified
  modules but do not introduce native phase runners.
- `ProgramInstance` is execution state, not a polymorphic controller.
- `PhaseScript` builders are read-only orientation and deletion evidence, not compiler inputs, a
  permanent wire format, or an authored ABI. Each supported phase receives a direct native typed-module
  builder.
- Current domain operations migrate into actions, reducers, queries, or reusable subprograms; they do not
  receive corresponding core IR opcodes.
- `IInputMacroPlanDriver::Start/Advance` is generalized into program continuations and awaited actions.
  Shared interaction composition lowers its finite segments, pure reducer transitions, input scopes,
  semantic awaits, observations, checks, and emissions into the ordinary program runtime;
  `InputMacroRuntime` is not retained as a peer program executor.
- Semantic points, awaits, address expressions, observations, and their use policies are reusable
  composition inputs. They lower before verification and do not introduce an observation runtime, query
  VM, or new opcode family.
- Existing capture profiles remain opaque inputs to passive `CaptureService`. Their sampling, window,
  recorder, progress, queue, control-event, and artifact semantics are preserved while
  `StopPointRouter`/`ExecutionEngine` own control authority.
- `EmulationSession` owns workset epoch allocation; workset-local services own input publication, guest
  reads and mutations, movie lifecycle, one passive capture attachment, screenshots, telemetry, and resource
  cleanup. State file paths are caller-declared, data mutations restore unless explicitly committed,
  executable patches cannot be committed, read-only state restore uses exact embedded/hash-verified DTM
  history and active-DTM identity, recording rewind is same-session memory-only, mandatory capture
  finalization failure blocks reuse, telemetry coalescing preserves sequence order, and screenshot
  ownership is synchronously actor-bound pending future nonblocking ingress/cancellation.
- The remaining direct `DolphinWrapper` operations in the disconnected VM fail locally and are retained
  only as orientation and deletion evidence. Capability packs and program actions arrive with the typed
  runtime rather than being improvised in Slice 4.
- Exact guest-opcode stepping is not a forward phase contract. Breakpoint departure uses router
  suppression; behavior that must be observed after executing guest code uses a later semantic witness
  or other routed continuation. Any future `ProgramRuntime` IR instruction-stepping debugger is a
  separate deferred facility and does not step guest PowerPC opcodes.
- Workflow transition and dynamic-step capabilities are retained through their current persistence and
  transaction contracts. Program-kind handlers translate between those records and typed runtime
  inputs/results. Generalized frontier orchestration is out of scope.
- `ProgramKind` may remain SavorDb job/handler/queue/affinity and UI/history metadata but ceases to select
  worker execution after runtime materialization.
- `WorkerWorkset` becomes the sole production dispatch envelope. It is a transient ordered collection of
  independent item templates, not IR, durable workflow topology, a phase controller, or one aggregate
  retry/result.
- `WorkerRuntime` owns exactly one active session-mutating workset, at most one immutable host-only
  staged successor package, and one executing child. Staging may decode, verify, read/hash immutable
  artifacts, and pin exact module definitions, but it cannot bind an epoch, capture a baseline,
  acquire session-effect resources, construct a `ProgramInstance`, or advance Dolphin.
- Immediately before each active child, `WorkerRuntime` binds the current session and epoch. It streams
  the child's completed execution record and any promoted immutable output capture into a bounded
  worker-global completion/acknowledgement ledger. The ledger assembles the authoritative result only
  after required host finalization; acknowledgement ownership never remains in the active workset.
- Once every active-workset item has completed session work with required immutable capture promoted or
  is classified unstarted, its complete invocation/workset resources and baseline lease are released,
  and the session is proven clean, the staged successor may promote while older outputs finalize or
  terminals remain unacknowledged if the global ledger still has capacity.
- The pre-6A parent process seam implements and directly tests the eventual one-compatible-worker gate
  plus at-most-two progressive startup. It keeps coordinator data-plane work disabled; Slice 7
  applies the gate only after `CompleteExact` proves exactly the two production Full Phase module IDs/hashes,
  their dependency manifest, and no extras.
- Workset-specific coordinator grouping, active/staged/global-ledger capacity, and lease maintenance may
  change while every item retains its own claim, start, attempt, result mapping, retry, cancellation, and
  transition.
- Claim/start authority is validated for the complete finite membership before `SubmitWorkset`.
  Accepted children then run in immutable order without a per-item coordinator authorization pause;
  later lease loss or supersession is enforced through exact cancellation.
- Partial process characterization transfers a test-only canonical module through the ordinary module
  preparation protocol. It is never compiled into or counted among the two production Full Phase modules.
- Database work opens only for `CompleteExact`: exactly the two production Full Phase module IDs/hashes, the exact
  dependency manifest, and no extra installed module.
- The initial configurable bounds are 16 items/32 MiB per workset; 64 total and 32 active-plus-staged
  item credits; two finalizer threads with eight
  pending captures/256 MiB; 32 retained terminals/128 MiB; two concurrent startups; and one coordinator-
  buffered successor per negotiated Ready worker.
- Navigation Context migrates as an ordinary current phase. Navmesh Survey is the first net-new consumer
  after current behavior reaches the new runtime.

## Interfaces and ownership affected

The replacement crosses these current seams:

- `SavorWorker.cpp` control loop and visual command path;
- `ProgramRegistry` fixed construction, decoding, and retry metadata;
- `PhaseScript`, `PSInit`, `PSJob`, `PSResult`, `PSContext`, and `PSContextCodec`;
- the 52-opcode dispatch and all split `PhaseScriptVM*` host implementations;
- `InputMacroRuntime`, `IInputMacroPlanDriver`, and providers;
- predicate, breakpoint/address, and SavorProbe profile/runtime seams as their behavior is reconstructed
  through shared semantic-observation, passive-capture, and interaction composition;
- `WireSetProgram`, worker-protocol job/result envelopes, and worker-side `ProgramKind` dispatch;
- scalar production invocation submission, replaced by one `SubmitWorkset` path for 1..N children;
- the parent worker-process startup seam, changed from complete-pool serial preflight to an eventual
  one-compatible-worker data-plane gate plus at-most-two progressive startup, with actual DB activation
  held until Slice 7 proves `CompleteExact`;
- program-kind handler implementations and adjacent runtime-init/result adapters;
- job materialization and coordinator worker-slot bookkeeping needed to construct compatible worksets,
  account for active and staged items plus the worker-global terminal ledger, renew resident leases,
  publish per-item starts/results, and acknowledge committed terminals;
- worker host preparation needed to stage and hash one immutable successor without acquiring session
  authority or retaining state from another workset; and
- existing transition and successor-step publication behavior as an unchanged integration contract.

The migration adds no SavorDb schema/migration, persistent workset record, aggregate durable job/result,
or unrelated workflow, artifact, or transaction redesign. The timing hard cutover does intentionally
remove obsolete fields from public authoring interfaces and from newly generated job arguments.
The six physical `run_ms`/`vi_stall_ms` authoring columns remain ignored until a separate database
refactor removes them; only private insert shims write neutral `0,0` values required by their current
`NOT NULL` constraints. Workset-specific coordinator scheduling, capacity bookkeeping, and startup may
change. Other database interfaces may change only for ordered batch claim, exact-set lease renewal,
claim/start validation, and targeted terminal reconciliation over existing rows and per-item semantics.
Existing macro, address-program, and capture-profile representations are decoded, adapted, or consumed
in memory. Predicate authoring instead uses the hard-cut Definition, Execution Binding, and Group
schema with no old-record adapter. The refactor must not push workflow persistence into the worker or
move emulator ownership into DB adapters.

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

- run the same structured unwind for return, handled domain failure, infrastructure failure,
  bounded-host timeout, cancellation, and verifier/guard abort after acquisition;
- distinguish a normal domain failure from infrastructure failure;
- report cleanup independently from both;
- neutralize/release input and restore every owned subscription and mutation;
- advance `WorksetEpoch` on every restore and reject stale handles; and
- taint and retire a session when mandatory cleanup cannot be verified.

## Dependencies and migration implications

The current programs are not migrated by serializing or compiling today's `PhaseScript`. Direct native
typed-module builders follow this dependency order:

1. preserve the established worker, stop-point, execution, and scoped session-service ownership seams;
2. define and verify the small core IR and typed module/invocation/result contracts;
3. register generic service actions and modular game capability packs;
4. expose current capabilities through registered actions and reusable subprograms;
5. perform the pre-6A cancellation/health hard cutover: remove phase-provided elapsed deadlines and
   VI-stall settings, retain structural bounds and bounded host operations, install session-owned core
   health plus synchronous host-activity accounting, and remove public/behavioral use of the obsolete
   authoring timing columns without changing migrations or DDL;
6. add the worker-process seam and one-item workset process tests: implement the eventual
   one-compatible-worker gate and at-most-two progressive startup, transfer one canonical test-only
   module for `Partial` process smoke, and leave coordinator `data_plane_enabled` false until Slice 7
   verifies `CompleteExact`;
7. add the pre-6A `WorkerWorkset` pipeline: unified 1..N dispatch, exact compatibility validation, one
   active session-mutating workset, one host-only staged successor with no guest-state authority,
   composite `ProgramBaselineDefinition`/`RestoreBaseline` flow, a worker-global per-item result
   acknowledgement ledger, and
   coordinator active/staged/completion-capacity accounting;
8. implement the supported phase builders incrementally as 6A SeedProbe, 6B Navigation Context, 6C TAS
   Movie, 6D TAS Frame Detector, 6E Battle Context, 6F Battle Macro Probe, 6G Battle Single Turn, 6H
   Battle Completion, and 6I Battle Results Screen;
9. verify each builder against its current functional contract and focused source-derived
   characterization without adding a temporary translator or parallel differential executor;
10. have existing program-kind handlers or adjacent adapters derive exact runtime identity, typed inputs,
   and workset compatibility during materialization without changing stored job identity or affinity;
11. remove current worker switches, domain opcodes, and the subordinate macro scheduler; and
12. implement Navmesh Survey only on the new path.

The current dynamic battle-wave implementation is a migration asset: it provides executable examples for
separating bounded worker expansion from durable orchestration.

## Using this inventory

Before relying on a current-state claim for an implementation slice, check the named source or symbol
and run focused checks for the seam being changed. This inventory does not need to be synchronized after
every implementation edit. Navigation Context and the unimplemented Navmesh Survey must remain distinct.

The future refactor has addressed these pressure points when adding a phase from existing capabilities
requires no change to `SavorWorker`, `ProgramRuntime`, `ProgramExecutor`, `ExecutionEngine`, the
stop-point router, transport core, or a central opcode switch.

## Deferred work

This document does not decide:

- exact C++ interface spelling or source directory layout;
- compression and measurement-driven tuning beyond the fixed configurable workset, item-credit,
  immutable-cache, finalizer, global-completion-ledger, two-startup, and one-buffered-successor limits;
- the authored script syntax/UI;
- final module revision/hash algorithms;
- Survey-specific action algorithms;
- generalized `eventhook` trigger handling;
- collision anomaly objectives;
- overworld game rules; or
- cutscene acceleration policy.

The logical pipelined `WorkerWorkset` contract, cancellation-driven guest execution, centralized
core-health policy, one-compatible-worker startup gate, and unified dispatch direction are decided, not
deferred. SavorDb migrations and DDL remain fixed inputs. The timing-only authoring-interface removal
and private neutral insert shim are explicit exceptions; any other database-interface change beyond
ordered batch claim, exact-set lease renewal, claim/start validation, and targeted terminal
reconciliation requires concrete evidence and a corresponding guidance update rather than an ambient
reopening of SavorDb architecture.

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
- `SavorCore/Runner/InputMacro/IInputMacroHost.h`
- `SavorCore/Runner/InputMacro/InputMacroPlan.h`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp`
- `SavorProbe/ProbeProfile.*`
- `SavorProbe/ProbeRuntime.*`
- `SavorProbe/AddressProgramEvaluator.h`
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
