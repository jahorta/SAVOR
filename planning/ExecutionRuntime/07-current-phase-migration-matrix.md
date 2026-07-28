# 07 - Current Phase Migration Matrix

## Scope

This matrix is implementation guidance for moving the current fixed phase-program corpus to the target
runtime. Recheck the current repository code for legacy behavior before changing a program family. The
target module IDs and entrypoint names below are semantic identities; they are not claims about current
C++ symbols.

## Purpose and non-goals

This document assigns each supported production or diagnostic phase a native typed module, reusable
capability composition, focused behavior checks, and a vertical migration slice. It prevents an
implementer from deciding ad hoc that a difficult phase needs a separate native controller or permanent
compatibility runtime. The legacy multi-turn BattleRunner and monolithic BattleEndResults path remain
current-source orientation and deletion targets; they receive no target module.

It does not:

- implement a module, action, or runtime type schema;
- change SavorDb SQL/schema, migrations, stored representations, database-service interfaces, queues,
  claims, workflow persistence, transaction boundaries, or artifact-storage interfaces; the shared
  workset prelude may make only its documented coordinator scheduling and bookkeeping changes;
- make current persisted payload bytes the worker runtime contract; program-kind handlers may continue to
  use existing codecs to preserve the stored representation;
- make Navmesh Survey part of the current-phase migration corpus;
- move durable workflow decisions into a worker program; or
- require final C++ declarations or binary worker-wire layouts.

If Navmesh Survey is taken up after this migration, it begins only after the migrated Navigation Context
module and common runtime meet the prerequisites at the end of this document. Survey itself is not a
completion condition for the current-phase migration.

## Current implementation references

The migration rows below were prepared from the fixed programs returned by
`SavorCore/Phases/Programs/ProgramRegistry.cpp:44-70`, their current program-specific decoders at
`ProgramRegistry.cpp:74-109`, the `PK_*` catalog in
`SavorCore/Runner/IPC/Wire.h:63-83`, and the phase builders under
`SavorCore/Phases/Programs/`. Current DB support is established separately through
`SavorDb/Execution/ProgramDB`; a worker program's presence does not imply that a current DB descriptor or
workflow step exists for it.

## Migration constraints

### Migration rules

Every current phase follows the same rules:

1. **One executor.** The target is a `ProgramModule` run by `ProgramExecutor`. No migration may introduce
   a phase runner, native controller, factory-selected executor, or second VM.
2. **Exact runtime identity.** A SavorDb program-kind handler derives module ID, immutable revision/hash,
   entrypoint, dependency closure, state policy, and typed input from existing job/domain records and
   runtime configuration. `ProgramKind` may remain SavorDb job, handler, transition, queue/affinity,
   historical, and UI metadata; it no longer selects worker execution.
3. **Lifecycle is invocation policy.** Boot/load/restore, capture attachment, movie policy, default
   deadline, and state artifact roles are not hidden in payload keys or duplicated as entrypoint prologue
   operations.
4. **Domain effects are actions.** Stop waits, input leases, memory access, state saves, movie operations,
   capture, and Skies-specific queries use the exact descriptors defined in document 04. The rows below
   name semantics, not additional interpreter opcodes.
5. **Adaptive native logic is a reducer.** Existing macro-provider `Start/Advance` logic becomes a pure
   continuation reducer plus bounded awaited actions. It cannot call Dolphin, own an event loop, or
   schedule workflow work.
6. **Structured results.** Infrastructure status, domain outcome, and cleanup/session status are
   independent. Runtime-facing adapters project current payload/result representations into and out of
   native typed contracts.
7. **Native definitions only.** Each phase is authored directly as a typed module. Current
   `PhaseScript` builders, providers, opcodes, tests, and artifacts are behavioral orientation; they are
   neither compiler inputs nor an executable reference runtime.
8. **Workflow topology stays durable and unchanged.** Seed grids, battle waves, battle-end chaining,
   phase switches, fan-out, reduction, and retry across jobs remain coordinator responsibilities through
   existing SavorDb storage and interfaces.
9. **Local retry must be bounded.** A program may restore its invocation baseline and retry a bounded
   effect when the domain contract requires immediate same-attempt recovery. It may not generate an
   unbounded search frontier.
10. **Delete as each family clears its gate.** Worker-side payload decoders, context keys, opcode handlers,
    and registry switch cases are removed when their last migrated consumer clears parity. Existing
    persisted-data codecs may remain behind SavorDb program-kind handlers; they cannot execute the legacy
    VM.

## Canonical target module catalog

| Current family | Target module ID | Entrypoint | Long-term disposition |
|---|---|---|---|
| SeedProbe | `soa.seed_probe` | `probe` | Supported |
| Navigation Context | `soa.navigation.context` | `capture` | Supported |
| TAS playback | `soa.tas_movie` | `play_and_checkpoint` | Supported |
| TAS input-stream detector | `soa.tas_frame_detector` | `detect` | Supported diagnostic program |
| Battle Context | `soa.battle.context` | `capture` | Supported |
| Battle Macro Probe | `soa.battle.macro_probe` | `probe` | Supported diagnostic program |
| Battle Single Turn | `soa.battle.single_turn` | `execute` | Supported |
| Battle Completion | `soa.battle.completion` | `complete` | Supported |
| Battle Results Screen | `soa.battle.results_screen` | `advance` | Supported |

Module IDs do not imply one file per module. They are immutable semantic families in
`ProgramDefinitionStore`. All revisions use the common IR, verifier, invocation, action, result, and
artifact contracts.

### Canonical reusable imports used by the migrations

Document 04 owns the descriptors and signatures. The migration uses these exact logical IDs:

| Capability | Logical import |
|---|---|
| State lifecycle | `runtime.state.capture_baseline`, `runtime.state.restore`, `runtime.state.restore_baseline`, `runtime.state.save_artifact` |
| Emulator advancement | `runtime.execution.continue_until`, `runtime.execution.step_frames` |
| Input | `runtime.input.acquire_lease`, `runtime.input.set_held`, `runtime.input.pulse`, `runtime.input.neutralize`, `runtime.input.play_sequence`, `runtime.input.await_guest_poll` |
| Movie | `runtime.movie.play`, `runtime.movie.stop`, `runtime.movie.record_start`, `runtime.movie.record_stop` |
| Guest reads | `runtime.guest.read_u8`, `runtime.guest.read_u16`, `runtime.guest.read_u32`, `runtime.guest.read_u64`, `runtime.guest.read_f32`, `runtime.guest.read_f64` |
| Guest mutation | `runtime.guest.write_checked`, `runtime.guest.patch_executable` |
| Capture | `runtime.capture.attach`, `runtime.capture.marker`, `runtime.capture.finalize` |
| Game observations | `soa.battle.capture_context`, `soa.navigation.capture_context` |
| Pure turn materialization | `soa.battle.materialize_turn_input` |
| Adaptive battle subprograms | `soa.battle.command_macro`, `soa.battle.completion_macro`, `soa.battle.results_screen_macro` |
| Pure adaptive reducers | `soa.battle.command_macro.reduce`, `soa.battle.completion_macro.reduce`, `soa.battle.results_screen_macro.reduce` |

`soa.battle.materialize_turn_input` is a pure imported reducer/callable, not an awaited action. The three
macro base IDs are reusable IR subprograms. Their `.reduce` imports are pure transitions over typed state
and a completed effect. None is a whole-phase action or a peer engine.

### Reusable semantic-observation composition

Current stop keys, breakpoint alternatives, direct reads, address programs, baselines, and context
queries migrate through one reusable semantic-observation composition library:

- capability packs own `SemanticPointDefinition` identities and map them to physical PC, memory, or
  synthetic evidence;
- a `SemanticAwaitDefinition` declares exact point alternatives, bounded hit-time qualification or
  sampling, current-point acceptance, deadlines, and execution policy;
- a successful await returns a `SemanticPointReceipt` containing the logical point, physical evidence,
  stop sequence, `StateEpoch`, and declared hit-time samples;
- `AddressExpression<T>` and `ObservationDefinition<T>` describe bounded typed reads, checked
  dereferences/offsets, receipt fields, or registered coherent domain queries; and
- an `ObservationUse<T>` binds an observation to a point receipt, acquisition mode, required/optional
  behavior, baseline policy, and authoritative-emission or telemetry policy at the use site.

The library lowers before verification into exact capability/type/action imports, scoped router
subscriptions, `runtime.execution.continue_until`, ordinary guest-read or game-query actions, branches,
locals, and emissions. `HitTimeSample` is restricted to bounded router-side sampling before program
handling. `PausedAtPoint` performs ordinary reads while execution is paused. Evidence after a particular
instruction requires normal continuation to an explicitly declared later semantic point under exact
receipt suppression, or a frame step when frame granularity is the actual contract. Guest-opcode
stepping is not an observation mode or runtime action.

Ordered observations stay ordered, and a coherent multi-field value uses one registered query rather
than claiming separate scalar reads are atomic. Optional unavailability remains distinct from false or
zero. Required missing evidence is a structured action or infrastructure failure. Named baselines are
ordinary IR values with explicit `First` or `Latest` policy, and no receipt, baseline, or guest-derived
handle may cross a `StateEpoch` replacement.

### Reusable interaction composition

Current input-macro plans migrate through one reusable interaction-composition library rather than a
shared controller-like input-segment action. An `InteractionDefinition<State, Output>` has stable
identity/revision, typed state and output, pure initialization and advancement reducers, a finite
verifier-known segment set, hard budgets, and declared emissions. Each
`InteractionSegmentDefinition` declares its semantic gates, requested input and acknowledgement policy,
source-stop departure behavior, an optional declared semantic successor held under the same
publication, attached observations/checks,
timeout/stall/movie/cancel policy, and typed completion mapping. `InteractionSegmentResult` preserves the
exact stop receipt, request and release receipts, ordered observations/checks, elapsed evidence, epoch,
and distinct terminal status.

Before verification, the composer lowers static sequences and adaptive reducers into ordinary
subprogram CFG, action awaits, semantic-observation composition, branches, and emissions. A reducer may
select only a declared segment ID; it cannot construct effects or access a runtime service. There is no
`InteractionRuntime`, segment scheduler, macro opcode family, or action that hides a whole macro.

The migration preserves the current temporal behavior:

- publish the segment input and obtain its epoch before departing a current semantic stop;
- depart through exact current-receipt suppression and ordinary continuation without disabling the
  shared physical point;
- match completion by logical point, PC, stop sequence, and epoch;
- leave the reached semantic gate paused unless the segment explicitly retains the same publication
  through a declared successor point;
- read any required request receipt after the declared successor and before publishing neutral;
- where release matters, prove guest-observed release with a separately named witness using a fresh
  neutral epoch rather than treating neutral publication as proof;
- capture memory baselines before advancement and retain one neutral frame between translated
  memory-change polls; and
- hold one input lease across the interaction while segment subscriptions and observations use nested
  scopes.

Common unwind neutralizes input, proves release when required, releases nested subscriptions and the
lease, and taints the session if mandatory cleanup fails. Timeout, unexpected point, unacknowledged input,
unsatisfied check, infrastructure failure, cancellation, and cleanup failure remain distinguishable.

### Reusable predicate composition

Predicates migrate as one reusable module-composition library, not as battle opcodes or a predicate
runtime. A `PredicateDefinition` is a pure typed condition over semantic-observation results. A `Check`
binds that definition to:

- a semantic evaluation point;
- an ordered typed semantic-observation plan;
- required or optional evidence;
- a use policy such as branch, clean domain rejection, explicit fail, record, or accumulate; and
- an optional declared `ConditionObservation` emission.

Before activation, the library lowers each check into canonical IR, exact action/type/capability imports,
scoped router-subscription operations acquired through awaited actions, ordinary branches or returns,
and typed emissions. After lowering, `ProgramExecutor` sees no predicate opcode, service, private loop,
or separate executor. A predicate definition cannot read Dolphin, advance emulation, acquire resources,
write persistence, or determine workflow topology.

Current `AbortOnFail` data translates into a require/reject policy at the check use site; it is not part
of the reusable pure predicate definition. An unsatisfied required predicate is a typed domain
rejection. Failure to obtain required evidence is an action or infrastructure failure, and cleanup
status remains independent. Record-only predicates may evaluate false and still represent successful
program progress.

Current stored battle predicate definitions and result fields remain unchanged. Program-kind adapters
translate them into composition inputs in memory and project typed observations, passed/total counts,
and predicate-rejection outcomes through existing result operations. Current battle baseline behavior
translates as `Latest`, with the new value installed before evaluation at the same routed hit.

### Existing capture-profile compatibility

Existing `savor.capture.profile/1` artifacts remain unchanged and are handed through
`runtime.capture.attach` to `CaptureService`. The refactor does not translate profiles into program IR or
invent a replacement capture language. `CaptureService` preserves profile parsing, filters and
predicate bytecode, address programs, activation/dynamic watchpoints, PC and post-write sampling,
sampling order and policies, one-shot/max-hit behavior, windows, flight recorders, trace buffers,
retention, queue/drop/coalescing behavior, progress, event ordering, and artifact finalization.

Capture remains passive. `StopPointRouter` and `ExecutionEngine` own wake/control authority, while
`CaptureService` observes the same routed matched event so profile `control` subscriptions,
control-triggered windows/recorders, flags, metrics, and synthetic control events retain their existing
meaning. One routed hit keeps one sequence/snapshot/epoch identity across control, capture, and progress
views. Modules may attach, mark, or finalize an existing profile through ordinary capture actions, but
cannot reinterpret its internals.

## Common current-to-native reconstruction

| Current construct | Target treatment |
|---|---|
| `ARM_PHASE_BPS_ONCE`, canonical/gated vectors | `SemanticPointDefinition` and `SemanticAwaitDefinition` lower exact alternatives and current-point policy into scoped router subscriptions plus `runtime.execution.continue_until` |
| `LOAD_SNAPSHOT` and init-time savestate path | Invocation `StatePolicy` over a typed state handle or caller-declared immutable artifact; `StateService` alone advances `StateEpoch`, and `runtime.state.restore_baseline` is used only for a declared local retry |
| Timeout keys and `SET_TIMEOUT*` | Invocation deadline/budget plus action-specific bounded deadline |
| `RUN_UNTIL_BP*` and source-stop departure | `runtime.execution.continue_until` plus exact retained-receipt suppression under the sole `ExecutionEngine`; old step-off opcodes are mechanics, not target behavior |
| Frame stepping | `runtime.execution.step_frames` when the phase contract is genuinely frame-granular |
| Guest opcode stepping | No target action. Reconstruct the intent with suppression and a declared semantic successor; reject a path whose behavior cannot be expressed semantically |
| `APPLY_INPUT_FROM`, raw tape application | Interaction composition lowers requested input, semantic gates, poll acknowledgements, and neutral-release witnesses into scoped `runtime.input.*` and execution actions |
| `READ_*`, address programs, direct query helpers | `AddressExpression<T>`, `ObservationDefinition<T>`, and `ObservationUse<T>` lower to checked `runtime.guest.read_*` actions or registered coherent game queries |
| `WRITE_U32` and future patches | `runtime.guest.write_checked` or `runtime.guest.patch_executable`; data restores unless explicitly committed, executable patches are always reversible, and both return checked receipts |
| Movie start/stop and recording | Scoped `runtime.movie.*` actions holding a movie-exclusive input reservation; read-only DTM preparation occurs before boot and exact movie continuation participates in state restore |
| Save savestate from a guest path string | `runtime.state.save_artifact` publishes at a new caller-declared path with SHA-256, compatibility, lineage, and any exact DTM companion under an invocation-declared role |
| `GET_BATTLE_CONTEXT`, `GET_NAVIGATION_CONTEXT` | `soa.battle.capture_context` and `soa.navigation.capture_context` |
| Predicate arm/capture/evaluate | Shared predicate composition consumes semantic-observation results, lowers comparison and reaction to ordinary IR, and optionally emits declared `ConditionObservation` progress |
| Input-macro `Start`/`Advance`, plans, and `EXECUTE_*_MACRO_STEP` | Interaction composition lowers initialization/advancement reducers and verifier-known segment definitions into ordinary subprogram CFG, semantic awaits/observations, input actions, and emissions |
| Existing capture profile | `runtime.capture.attach` passes the unchanged `savor.capture.profile/1` artifact to passive `CaptureService`; profile internals are not lowered or redesigned |
| `EMIT_RESULT` and `RETURN_RESULT` | Typed record/artifact emission and typed domain return |
| `PSContext` keys | Entrypoint input, local, output, emission, diagnostic, and artifact schemas |
| Program-specific payload codec | Program-kind runtime adapter decodes existing persisted job/domain data and constructs typed `ProgramInvocation` input |
| Result INI/context mapper | Program-kind result adapter consumes `ProgramResult` and writes through existing result/domain persistence operations |

## WorkerWorkset dispatch boundary

Every entrypoint below remains scalar: one typed `ProgramInvocation` consumes one phase input and
produces one independently correlated terminal result. `WorkerWorkset` is the sole bounded production
dispatch envelope around already-independent scalar invocations; using more than one item is optional.
It is not a module
entrypoint, IR instruction, phase-sized controller, domain result, persisted job-set replacement, or
permission for a module to enumerate workflow work.

Any independently ready invocations whose exact `WorkerWorksetExecutionKey` matches may use this path; phase
identity alone neither permits nor forbids it. The key covers the exact module/entrypoint and dependency
identity, runtime/session profile, source-state or reusable-baseline identity, and the execution/service
policy that affects safe reuse. A one-item workset is valid and is the normal direct-tool boundary.
Within one worker, children still execute one at a time. Each child retains its own job, invocation,
attempt, deadline, cancellation, provenance, cleanup, artifact, and terminal-result identity, and every
child begins through the exact state preparation or baseline restore declared for the key.

The worker publishes each child result as it completes. Clean completion or a clean domain-negative
result may permit the next child; session taint or unproven cleanup aborts the remaining dispatch. The
coordinator bounds worksets by item count, encoded bytes, aggregate declared child budgets,
resident-item capacity, and unacknowledged terminal count/bytes; it may split one compatible population
across workers and leaves unstarted work independently cancelable or retryable through current
operations.
This is a locality/dispatch optimization only: existing SavorDb fan-out, leases, supersession, reduction,
and transitions remain authoritative.

A bounded list inside one scalar input is instead a **domain candidate list**. Its elements are one
program's declared algorithmic work and share that invocation's result and failure boundary. It does not
become a `WorkerWorkset`, and a workset never flattens or interprets such a list.

## Native module specifications

### Slice 6A - `soa.seed_probe::probe`

**Current control flow**

`MakeSeedProbeProgram` restores the VM baseline, applies one input frame, selects a pre-battle gated stop
or field-return canonical stop, reads the RNG seed, optionally compares an expected seed and saves a
state, emits the seed, and returns a run outcome. One worker definition serves neutral, grid, unique, and
field-return workflow variants; the existing SavorDb handlers continue to generate those variants and
later steps through current operations.

**Typed input**

`SeedProbeRequest` contains:

- `target`: `PreBattle` or `FieldReturn`;
- `mode`: `Observe` or `Materialize`;
- one `GCInputFrame`;
- optional expected RNG seed, required in `Materialize`;
- invocation/action deadline policy; and
- optional output-state artifact role, required in `Materialize`.

The source state is supplied by invocation `LoadArtifact` or `RestoreBaseline`; it is not a payload path.

**Typed output and emissions**

`SeedProbeResult` contains target, mode, observed seed, optional expected seed, match status, stop
observation, and typed domain outcome. Materialize success includes one immutable `StateArtifact`
reference. The program emits one `SeedObservation` record; the program-kind result adapter projects it
through the existing SeedProbe result and transition operations.

**Required composition**

- scoped input lease and guest-observed release;
- semantic await for the selected checkpoint and exact point receipt;
- checked RNG `u32` observation;
- optional immutable state save; and
- no SeedProbe-specific core opcode.

**Lifecycle duplication removed**

- `OpArmPhaseBps` and `OpLoadSnapshot`;
- timeout and output-path context keys;
- first-byte `PK_SeedProbe` dispatch;
- program-specific payload decoding; and
- use of `DW_RUN_OUTCOME_CODE` as both infrastructure and domain result.

**WorkerWorkset fit**

The scalar `SeedProbeRequest` does not change. Grid and Unique fan-outs are expected high-value workset
populations because many independent requests share one exact source baseline and module revision.
Grid may use bounded chunks directly. Unique uses small chunks, publishes each child result immediately,
and accepts asynchronous cancellation of unstarted siblings after the authoritative winner/supersession
path reacts; it must not hide a large committed candidate tail behind one worker. Neutral, prebattle, and
field-return requests are typically singleton, but no phase allowlist prevents an exact-key match from
using the generic path.

**Parity checks**

- legacy payload v1/v2 codec and field-return tests;
- existing SeedProbe DB adapter and dynamic grid/unique transition tests;
- `SavorE2E/SeedProbeRealWorkerScenario.cpp`;
- exact observed seed and materialized state semantics for PreBattle and FieldReturn;
- cancellation while waiting leaves neutral input and no router resources.

**Legacy removal condition**

All SeedProbe workflow step kinds construct `soa.seed_probe::probe` at the runtime boundary; the result
handler consumes typed output and writes through the existing SeedProbe persistence contract; the E2E
scenario uses `ProgramInvocation`; and no worker-side caller uses `SeedProbeKeys`, `PK_SeedProbe`, or the
SeedProbe branches in `ProgramRegistry`. `SeedProbePayload` may remain only where an existing SavorDb
handler needs it to preserve stored data.

### Slice 6B - `soa.navigation.context::capture`

**Current control flow**

The current Navigation Context script restores its baseline, publishes neutral input, records the entry
PC/stop, waits for `NavigationContextInitialPlayerInputReady` when necessary, validates the exact capture
key and PC, captures navigation state, saves a matching savestate, emits the `.nctx` blob, and maps
timeout/stall/host/unexpected/capture/data/save failures.

**Typed input**

`NavigationContextCaptureRequest` contains the source state relationship, capture qualification revision,
execution bound, and declared `navigation_context` plus `survey_bootstrap_state` artifact roles. The
neutral controller is module behavior through a scoped input action, not a serialized input key.

**Typed output and emissions**

`NavigationContextCaptureResult` contains:

- typed outcome and current failure taxonomy;
- source and output state lineage;
- exact capture stop/PC evidence;
- structured `NavigationContext`;
- portable NCTX artifact reference; and
- matching immutable Survey-bootstrap `StateArtifact`.

NCTX v1 remains a portable artifact codec. The target typed record is the worker/runtime contract; the
program-kind adapter projects it into the existing workflow and artifact representation.

**Required composition**

- scoped neutral input with observed release;
- current-point receipt and semantic navigation await;
- typed registered `soa.navigation` context observation;
- NCTX artifact writer; and
- immutable state save.

**WorkerWorkset fit**

Navigation Context normally contributes one scalar capture for one source state and therefore usually
uses a singleton workset. Exact-key independent captures may still use the generic dispatch path, but
the module never turns multiple source states into one request or one NCTX/state result.

**Parity checks**

- capture extractor and NCTX codec tests;
- payload validation and neutral-input tests;
- immediate-capture and arbitrary-entry paths;
- fixed key/PC qualification;
- all current `FailureCode` cases;
- DB source/output/artifact identity and idempotency tests; and
- focused SavorE2E capture using the known `a101b` bootstrap path.

The observed pair
`navigation-context-41.sav` / `navigation-context-41.nctx` is the concrete first parity fixture. The
module must reproduce a semantically equivalent typed context and matching-state relationship; the exact
existing files remain immutable inputs/evidence and are not overwritten.

**Legacy removal condition**

The `navigation.context_probe` handler constructs the target module invocation, projects its result into
the existing `.nctx` and state-artifact representation, and keeps current consumers compatible. No
worker-side caller uses kind `10`, Navigation Context context keys, `GET_NAVIGATION_CONTEXT`, or its
`ProgramRegistry` branches; the persisted payload codec may remain behind the handler, and the Survey
handoff below passes.

### Slice 6C - `soa.tas_movie::play_and_checkpoint`

**Current control flow**

`MakeTasMovieProgram` validates the DTM disc ID, starts playback, applies a derived timeout, waits for the
pre-battle stop or failure, stops playback, saves a savestate, steps one frame, and returns movie failure
status. The payload decoder derives disc identity and runtime from the DTM.

**Typed input**

`TasPlaybackRequest` contains an immutable DTM artifact reference, expected disc identity, explicit stop
condition, playback bounds/headroom policy, and an output-state artifact role. Invocation state policy is
explicit (`Boot` for the current workflow unless a future workflow intentionally supplies a state).

**Typed output and emissions**

`TasPlaybackResult` contains playback outcome, terminal stop observation, DTM input/VI counters reported
by `MovieService`, and one optional output `StateArtifact`. Playback failure is a domain outcome when the
backend and cleanup succeeded; movie-service/backend failures are infrastructure status.

**Required composition**

- disc query;
- scoped movie playback that, when supplied on initial `SessionOpenOptions`, validates and stages the
  DTM so `Movie::PlayInput` occurs before the session's single backend boot; starting playback on an
  already-open session uses the corresponding `StateService` reboot path;
- one unsuspendable movie-exclusive `InputArbiter` reservation held through playback;
- semantic await with movie-ended observation;
- immutable state save at a caller-declared path with SHA-256, compatibility, lineage, and the exact DTM
  bytes/hash plus current input/frame continuation; and
- routed one-frame advancement only if still required by a verified state-publication invariant.

**Lifecycle duplication removed**

Movie stop and input-reservation release become scope unwind rather than branch-sensitive opcodes. DTM
and output paths become artifact references. A later restore re-establishes the exact read-only
state-plus-DTM continuation; it does not ask `StateService` to infer whether a movie is active or which
DTM to use from Dolphin or an ambient path. A cold external read-only import carries no required
caller-supplied frame/input cursor: after the exact DTM is staged and state is restored, `MovieService`
records Dolphin's authoritative observed cursor. An internally captured checkpoint already carries a
known cursor and must match it exactly. External imports explicitly declare `NoMovie` or
`ReadOnlyPlayback`. Recording file-artifact capture/import/restore is unsupported, while same-session
recording rewind uses an in-memory handle. Timeout derivation occurs before activation and is recorded
in invocation provenance.

**WorkerWorkset fit**

TAS Movie requests remain independently schedulable scalar movie lifecycles. They are expected to be
singleton or small worksets because a long playback, boot/reboot policy, and per-request checkpoint
dominate dispatch overhead. An exact-key match may use the generic path, but workset reuse never carries
movie state, input position, or a prior request's checkpoint into the next child.

**Parity checks**

- current TAS payload derivation tests and DB workflow adapter tests;
- `SavorE2E/TasMovieRealWorkerScenario.cpp`;
- successful and failed playback both stop the movie;
- exact terminal state/DTM artifact pair is usable by the next invocation with its internally captured
  frame/input cursor matched and a new `StateEpoch`;
- cold external state/DTM import succeeds without invented cursor metadata and records Dolphin's
  post-restore observed frame/input position;
- wrong disc rejects before movie playback.

**Legacy removal condition**

`tasmovie.play` uses the typed module, the worker has no TAS payload switch, and no caller sends
`PK_TasMovie`. `TasMoviePayload` may remain behind the SavorDb program-kind handler or a read-only
historical boundary to preserve existing stored records; it is outside worker execution.

### Slice 6D - `soa.tas_frame_detector::detect`

**Current control flow**

`MakeTasFrameDetectorProgram` validates the disc, plays a DTM, samples the current input, then loops over
one routed frame step and another sample until movie end. It stops playback and returns status. The fixed
program and payload exist, but no active `SavorDb` descriptor registration was found.

**Typed input**

`TasFrameDetectionRequest` contains DTM artifact, expected disc identity, sampling policy, and explicit
maximum input/frame budget.

**Typed output and emissions**

`TasFrameDetectionResult` contains movie outcome, total samples, and one immutable
`TasInputStreamArtifact` or bounded emitted sample sequence. Large sample streams are artifacts, not an
ever-growing program-local record returned inline.

**Required composition**

- scoped movie playback;
- routed frame step;
- movie input/status observation; and
- bounded artifact writer.

The loop remains ordinary IR control flow. `RECORD_TAS_INPUT_SAMPLE` does not become a core opcode.

**WorkerWorkset fit**

This direct diagnostic enters the worker through a one-item workset. Its bounded sample sequence is
domain work inside one scalar invocation, not a collection of worker jobs.

**Parity checks**

- current payload/codec behavior;
- first sample occurs before the first step;
- one sample follows every stepped frame;
- empty/immediately-ended movies terminate correctly; and
- cancellation finalizes or aborts the artifact according to its declared publication policy and always
  stops playback.

**Legacy removal condition**

Direct diagnostic callers, if any, invoke the target module; `PK_TasInputStreamDetector`, its decoder,
context keys, and `RECORD_TAS_INPUT_SAMPLE` handler are removed. Migration does not create a DB workflow
registration merely because none exists today.

The legacy `PK_BattleTurnRunner` multi-turn path is not a migration target. It has no production
descriptor in the current catalog, receives no typed module or runtime adapter, and is deleted with the
legacy execution corpus. A payload codec may remain only if a supported persisted-data reader
independently requires it; it cannot activate worker behavior.

### Slice 6E - `soa.battle.context::capture`

**Current control flow**

`MakeBattleContextProbeProgram` restores baseline, records whether the current stop is already
`TurnInputs`, otherwise waits there, captures `BattleContext`, and emits its blob. The DB transition
validates output/wave relationships and appends one or more `battle.single_turn` children.

**Typed input**

`BattleContextCaptureRequest` contains only capture qualification and action bounds that are not already
fixed by the module revision. The source state is a required immutable invocation artifact.

**Typed output and emissions**

`BattleContextCaptureResult` contains a typed `BattleContext` record, source-state identity, terminal stop
observation, and capture provenance. A codec artifact may be published for compatibility, but the worker
contract is the typed schema.

**Required composition**

- acquire an explicit current-point receipt;
- await `TurnInputs` if necessary; and
- acquire typed `soa.battle` context through a registered coherent observation.

**Workflow boundary**

The module does not create waves. The existing Battle Context transition handler validates the projected
output and creates or resolves waves through current SavorDb commands and records. The Battle Single
Turn program-kind adapter constructs `soa.battle.single_turn::execute` only when the existing child job
is activated.

**WorkerWorkset fit**

Battle Context normally performs one coherent capture for one wave/source-state relationship and thus
usually uses a singleton workset. Exact-key independent captures may use generic co-dispatch, but the
module does not absorb downstream wave fan-out or combine contexts from distinct source states.

**Parity checks**

- immediate-capture and run-to-capture paths;
- exact current `BattleContext` codec output;
- timeout, wrong-stop, and memory-read failure;
- existing DB tests for direct-wave and bootstrap-wave fan-out; and
- restart/idempotency around transition publication.

**Legacy removal condition**

Both `battle.context_probe` and `battle_chain` bootstrap construct the target invocation at the runtime
boundary; their existing transition handlers consume adapter-projected output; and
`PK_BattleContextProbe` plus the `GET_BATTLE_CONTEXT` worker opcode path have no worker-side caller. The
payload codec may remain only behind the SavorDb handler to preserve stored data.

### Slice 6F - `soa.battle.macro_probe::probe`

**Current control flow**

`MakeBattleMacroProbeProgram` materializes a command/fake-attack plan, repeatedly asks the macro runtime
to execute one adaptive segment, steps past the turn-ready stop, optionally observes a bounded tail, and
returns macro status. SavorE2E currently activates this program directly rather than through a DB
descriptor.

**Typed input**

`BattleMacroProbeRequest` contains:

- ordered macro commands and target slots;
- transition-neutral-frame policy;
- segment and VI-stall bounds;
- observation-tail bound;
- fake-attack budget and selected pattern set; and
- optional capture profile artifact.

**Typed output and emissions**

`BattleMacroProbeResult` contains planner status, macro terminal status, typed failure, final stop, input
receipt summary, optional battle-context observation, and trace/capture artifacts. Each fake-attack or
memory-gate observation is emitted with stable sequence identity.

**Required composition**

- pure command-plan compiler;
- pure adaptive reducer derived from provider `Start/Advance`;
- shared interaction composition with semantic gate alternatives, request/release acknowledgements,
  memory baseline/change observations, exact source suppression, and declared semantic successors where
  a publication must span a later point;
- battle-context observation/query; and
- routed observation-tail execution.

No single “run battle macro probe” native action is allowed. The program owns visible branching in IR;
the reducer decides only the next bounded segment from typed state and the preceding completion.

**WorkerWorkset fit**

This direct diagnostic enters through a one-item workset. Its ordered commands, adaptive segments, and
observation tail are domain control flow within that one scalar invocation; they are not workset
children.

**Parity checks**

- all compiler/planning-context tests in `test_battle_macro_probe.cpp`;
- provider permission and unexpected-stop behavior;
- fake-attack pattern, baseline-before-advance, one-neutral-frame polling, and memory-gate ordering;
- publication-before-departure, exact point/PC/sequence/epoch matching, successor causality,
  request-receipt-before-neutral, and fresh-neutral release-witness behavior;
- `InputMacroRuntime` cleanup-once cases;
- native in-process/runtime characterization of the diagnostic module, with direct-worker SavorE2E
  deferred until Slice 7 activates the complete catalog; and
- cancellation at every segment boundary leaves neutral acknowledged input and no subscriptions.

**Legacy removal condition**

Direct SavorE2E activation uses `ProgramInvocation`; all provider behavior is reachable through the common
reducer/action path; `PK_BattleMacroProbe`, `MATERIALIZE_BATTLE_MACRO_STEPS`,
`EXECUTE_BATTLE_MACRO_STEP`, the VM macro-host inheritance, and the subordinate runtime scheduler are
removed.

### Slice 6G - `soa.battle.single_turn::execute`

**Current control flow**

Battle Single Turn is the most demanding current migration. It:

- restores a source state and optionally reaches `AfterRandSeedSet`;
- performs and verifies an RNG override;
- applies optional initial input on turn one;
- advances to `TurnInputs`;
- compiles a `TurnPlan` into an adaptive battle-command macro;
- executes macro segments and confirms `TurnIsReady`;
- retries the input path once from baseline on selected failure;
- arms capture memory watchpoints;
- runs until next turn, victory, defeat, predicate failure, or execution failure;
- records ending RNG and, on successor/victory outcomes, publishes a savestate; and
- returns context used by the durable survivor/wave reducer.

**Typed input**

`BattleSingleTurnRequest` contains:

- current turn index and maximum turn;
- optional initial input;
- typed `TurnPlan`;
- existing predicate set translated into reusable predicate-composition inputs;
- fake-attack budget/accounting;
- optional starting RNG override;
- bounded same-attempt retry policy, fixed to at most one retry for parity;
- optional capture profile artifact; and
- declared successor-state artifact role.

The source savestate is the invocation baseline. Output paths and bootstrap profile strings are not
program inputs.

**Typed output and emissions**

`BattleSingleTurnResult` contains:

- typed battle outcome;
- source state and state-epoch provenance;
- starting/original/requested/applied/ending RNG values where applicable;
- turn input/output indices and retry count;
- terminal semantic stop;
- typed condition observations, aggregate predicate compatibility outcome, and macro outcomes;
- optional terminal `BattleContext`;
- optional successor `StateArtifact`; and
- action, input-receipt, mutation, capture, and branch trace references.

**Required composition**

- semantic battle waits and routed stepping;
- checked RNG read and checked scoped write with readback receipt;
- the same command compiler/reducer/interaction composition used by Macro Probe;
- shared semantic observation for exact stop receipts, ordered witness acquisition, baselines, and context
  queries;
- shared predicate composition over those observation results, including pure evaluation,
  and declared fail-fast/progress policy;
- scoped capture attachment;
- immutable state save; and
- explicit baseline restore for the one bounded local retry, which advances `StateEpoch`.

The RNG mutation's lifetime is declared. It may persist in the disposable emulated branch until restore
or state publication, but its receipt and readback remain part of provenance. If it is intended to
remain in the current guest state after its scope, that is an explicit data-mutation commit; otherwise it
restores. It cannot be an untracked raw write, and this commit option never applies to executable
patches.

**WorkerWorkset fit**

Battle Single Turn is an expected high-value workset population: independently persisted candidates for
one wave commonly share the exact source baseline, module revision, capture/predicate configuration, and
execution policy. Each candidate nevertheless remains one scalar invocation, publishes its own terminal
result and optional successor state, and restores the common baseline before it runs. Workset execution
does not select survivors or create a next wave; existing durable reduction consumes the same
per-candidate results as ungrouped execution.

**Workflow boundary**

The program returns one candidate. It never chooses survivors, creates a battle wave, or schedules
another turn. Existing deterministic reduction selects best candidates, creates the current next-wave
records, and appends the current dynamic steps. The program-kind adapter constructs the exact runtime
invocation when each existing job is activated.

**Parity checks**

- Battle Turn payload and program-shape tests;
- RNG override original/requested/applied evidence;
- first-turn and later-turn prelude behavior;
- macro retry and retry-exhaustion paths;
- next-turn, victory, defeat, turn-limit, predicate, materialization, and execution outcomes;
- predicate trigger, baseline, comparison, unavailable-evidence, abort, passed/total, and progress
  behavior;
- capture memory-watchpoint behavior;
- `SavorE2E/BattleSingleTurnScenario.cpp`;
- DB survivor selection and multi-wave dynamic-step tests; and
- native runtime behavior over the existing three first-turn checkpoint corpus where applicable.

**Legacy removal condition**

All `battle.single_turn` jobs use the target module and typed runtime result; the result handler projects
that result into the existing wave operations; durable wave spawning remains idempotent after restart;
no VM macro opcode or direct memory/write path is used; and kind `5` plus the Battle Single Turn branches
in `ProgramRegistry` are removed from worker execution. Existing persisted payload version `5` remains
readable by the SavorDb handler.

### Slice 6H - `soa.battle.completion::complete`

**Current control flow**

Starting at a victory state, the completion script materializes the adaptive completion provider,
executes segments until reward commit is complete, captures the pre/post character and reward state into
a `BCMB` manifest, saves the completion state, and returns completion/failure.

**Typed input**

`BattleCompletionRequest` contains the exact victory source-state relationship, bounded completion policy,
and output-state/manifest artifact roles. It does not contain an output filesystem path.

**Typed output and emissions**

`BattleCompletionResult` contains typed outcome/failure, the structured completion manifest, its
invariant flags, terminal stop, input receipts, and completion `StateArtifact`. The existing BCMB encoding
may remain an artifact codec; it is not the program's internal result type.

**Required composition**

- a phase-scoped input lease that publishes neutral for the new epoch while the restored core is paused,
  before any resume;
- causal semantic routes `0x8006F554 -> 0x8006F558` and
  `0x8006F590 -> 0x8006F594`, retaining the first receipt until its exact successor;
- paused verification that `battleInputState == 2` only after the matching successor;
- pure completion reducer and ordinary semantic waits for reward entry and reward commit;
- typed battle reward/context queries;
- immutable manifest publication; and
- immutable state save.

**WorkerWorkset fit**

Battle Completion normally has one scalar invocation per victory lineage and therefore usually uses a
singleton workset. Exact-key independent completions may use generic co-dispatch, but distinct victory
states never share a baseline merely because they belong to the same phase.

**Parity checks**

- BCMB round trip and expected-view derivation;
- pre/post capture and reward-phase validation;
- wrong-source and read-failure cases;
- successful current-epoch neutral publication before resume, with no guest neutral-poll or release
  witness requirement;
- both causal store/successor routes and rejection of a successor without its matching first receipt;
- absence of guest-instruction-step imports;
- current DB aggregate/result identity checks; and
- cleanup/cancellation between every provider segment.

**Legacy removal condition**

The `battle.completion` program-kind adapter translates existing inputs to the typed runtime contract and
projects its manifest/state outputs into the current downstream representation. Kind `9`, completion
context keys, materialize opcode, and the old worker result path have no active worker-side caller;
stored payload/result codecs remain where existing SavorDb operations require them.

### Slice 6I - `soa.battle.results_screen::advance`

**Current control flow**

The split Results Screen phase consumes a completion manifest and state already paused at
`BattleEndFieldReturnReseedComplete` (`0x801012B4`), validates the expected presentation, adaptively
advances the remaining results lifecycle and cleanup, saves the terminal state, and publishes a detailed
BERB report. It starts at that split results-screen boundary and does not reproduce the
compatibility-only victory-to-results path.

**Typed input**

`BattleResultsScreenRequest` contains exact completion-manifest artifact, acceleration policy
(`RequiredOnly` or `FullAdaptive`), source-state lineage, execution bounds, and terminal-state/report
artifact roles.

**Typed output and emissions**

`BattleResultsScreenResult` contains typed outcome/failure, expected and observed presentation, ordered
action traces, mismatch and invariant flags, diagnostic, terminal stop, report artifact, and terminal
`StateArtifact`. BERB remains a portable artifact encoding around the typed record.

**Required composition**

- pure results-screen provider/reducer;
- shared interaction composition;
- battle results/lifecycle queries;
- immutable report publication; and
- immutable state save.

**WorkerWorkset fit**

Battle Results Screen normally has one scalar invocation per completion-manifest/state lineage and
therefore usually uses a singleton workset. Completion followed by Results Screen is a dependency chain,
not a workset; ordinary worker affinity may preserve locality after the durable handoff, but the two
entrypoints are never fused into one child or result.

**Parity checks**

- BERB round trip and payload policy tests;
- source/manifest qualification;
- stat, learned-magic, item, mandatory-confirm, and fade branches;
- RequiredOnly and FullAdaptive behavior;
- fresh input-epoch and causal release requirements;
- RNG/lifecycle/completion invariants;
- no victory-store points, held-through guest instruction, or monolithic BattleEndResults module;
- `SavorE2E/BattleEndResultsScenario.cpp`; and
- DB end-workflow idempotency and artifact-lineage tests.

**Legacy removal condition**

The split `battle.results_screen` program-kind adapter uses the target module and translates typed
report/state artifacts into the existing downstream representation. Kind `8`, its compatibility alias,
context keys, materialize opcode, and old worker result path are removed from worker execution; existing
stored payload/result codecs and bindings remain unchanged.

## Input macro migration through interaction composition

The current providers must migrate once, not independently inside each battle phase:

1. Convert each provider's mutable controller into a typed reducer state record.
2. Translate `Start` into the reducer's initial transition and `Advance` into a transition over one
   completed segment result.
3. Represent a provider decision as one of:
   - select one verifier-known `InteractionSegmentDefinition`;
   - return completed;
   - return typed domain failure.
4. Store the pending continuation in `ProgramInstance`.
5. Lower each segment through the shared interaction and semantic-observation composers into the same
   registered action handlers and `ExecutionEngine` used by ordinary program operations.
6. Preserve input publication/epoch before source-stop departure, exact completion identity, request
   receipts, any declared successor under the same publication, baseline/change ordering, and separately
   witnessed neutral release only where release matters.
7. Acquire one interaction-wide input lease and nested router, observation, watchpoint, and capture
   resources through the invocation scope stack.
8. Preserve the cleanup-once behavioral tests, but make the common scope unwinder the mechanism.
9. Remove `IInputMacroHost`, `IInputMacroDriverHost` physical-service access, VM private inheritance,
   local exclusive-session state, and `InputMacroRuntime` as a scheduler.

The reusable battle command, completion, and results reducers may remain native C++ because they perform
complex game-specific decisions. They remain pure: typed state plus typed completion in; next requested
effect/events/completion out.

## Authoritative 6A-6I migration matrix

The generic Slice 4 services and standalone resource ledger are established, but this table remains a
program-migration order rather than a claim that their action descriptors or game queries exist.
Capability packs, typed action registration, and `ProgramRuntime` arrive in Slice 5.

| Slice | Family | Expected workset fit | Prerequisites |
|---|---|---|---|
| 6A | SeedProbe | Strong for Grid/Unique; other configurations typically singleton | Core IR, typed invocation/result, stop wait, input scope, memory read, state save, and existing SavorDb handler projection |
| 6B | Navigation Context | Typically singleton | Navigation capability pack, qualified capture, immutable NCTX/state pair, and existing handler projection |
| 6C | TAS Movie | Typically singleton or small | Movie scope, explicit boot policy, semantic stop, and checkpoint publication |
| 6D | TAS Frame Detector | One-item direct | TAS Movie lifecycle plus bounded frame stepping and in-process diagnostic module validation |
| 6E | Battle Context | Typically singleton | Typed capability-pack query and existing workflow fan-out after handler projection |
| 6F | Battle Macro Probe | One-item direct | Shared semantic-observation and interaction composition, adaptive reducer transitions, suppression/successor timing, and in-process diagnostic module validation |
| 6G | Battle Single Turn | Strong for exact-key wave candidates | Mutation receipt, unchanged capture-profile semantics, local restore/epoch, shared predicate/observation composition, complex result, and durable turn-wave projection |
| 6H | Battle Completion | Typically singleton | Neutral pre-resume publication, causal post-store points, BCMB/state artifacts, and existing handler projection |
| 6I | Battle Results Screen | Typically singleton | Completion-manifest lineage, real request/release witnesses, BERB/state artifacts, and existing handler projection |

Each slice is vertical: it adds the native module, any reusable action/composition refinement, current
program-kind adapter projection where applicable, and focused validation. Production
`ProgramInvocation` remains unavailable until all nine modules are ready. There is no legacy 6J and no
monolithic BattleEndResults slice.

## Interfaces and ownership affected

Migration changes:

- program-kind handlers construct exact `ProgramInvocation` from existing persisted records;
- program selection from `ProgramRegistry` switches to `ProgramDefinitionStore`;
- worker input from decoded existing job/domain data to typed runtime values;
- fixed behavior from current source and tests to direct native module builders;
- current stop/address/query/baseline constructs to shared semantic-observation composition before module
  verification;
- current predicate records to shared in-memory predicate composition before module verification;
- VM host calls to registered actions over session services;
- input macro providers to pure reducers plus verifier-known interaction segments;
- existing `savor.capture.profile/1` artifacts to passive `CaptureService` without representation or
  semantic conversion;
- program-kind result handlers project `ProgramResult` through existing result/domain operations; and
- exact module/dependency/state/runtime validation occurs at worker activation without changing current
  SavorDb affinity, queue, or claim representations;
- Multi-item `WorkerWorkset` formation optionally co-dispatches exact-key scalar invocations while
  preserving every existing job/attempt/result identity and remaining absent from persisted workflow
  or domain contracts.

Existing workflow transition handlers remain domain-specific and use their current interfaces and
storage. An adapter may supply values derived from typed runtime output; the handler may not become a
worker controller.

## Failure and cleanup behavior

Every phase behavior test must classify terminal behavior into:

- **infrastructure status:** verification, backend, transport, service, or action-contract failure;
- **domain outcome:** expected game result such as seed mismatch, defeat, predicate rejection,
  no-progress, unexpected qualified stop, or invalid capture; and
- **cleanup/session status:** whether all acquired resources were released/restored and the session is
  reusable.

For predicates, `Unsatisfied` follows the check's explicit record/branch/domain/fail/accumulate policy.
Failure to obtain or evaluate required evidence is not rewritten as false. A record-only false predicate
is successful program progress; a required false predicate may return a clean predicate-rejection domain
outcome.

For observations, optional `Unavailable` is not false or zero, required missing evidence is a structured
failure, and a stale receipt/baseline is rejected after `StateEpoch` replacement. For interactions,
timeout, unexpected point, unacknowledged request or release, unsatisfied check, infrastructure failure,
cancellation, and cleanup failure remain distinct. Normal and abnormal unwind attempt neutralization,
any required release witness, subscription release, and input-lease release exactly once; mandatory
cleanup failure taints the session.

Legacy code sometimes derives `PSResult::ok` solely from `DW_RUN_OUTCOME_CODE`. The migration must not
preserve that collapse. A domain failure may return a clean successful execution envelope, while failed
input neutralization, movie stop, breakpoint release, capture finalization, state handling, data-write
restoration, or patch restoration taints the session even if the domain result was otherwise successful.
A mandatory capture finalization failure also leaves capture unable to accept another attachment and
blocks session/worker reuse until a full rebuild.

Cancellation and timeout injection is mandatory at every action-await boundary for each migrated family.

## Dependencies and migration implications

This matrix depends on documents 02 through 06:

- the session actor and sole-ownership rules;
- verified module/IR/type contracts;
- action descriptors and resource scopes;
- invocation/result/artifact identity; and
- the fixed SavorDb integration boundary in document 06.

No DB migration is part of this refactor. Compatibility translation belongs in program-kind handlers or
adjacent runtime adapters. In-flight incompatible worker activations may be drained at the release
boundary; persisted job, workflow, result, artifact, queue, and affinity records are not migrated. A
worker-side legacy/new controller split is forbidden, and live `PhaseScriptVM` state is never serialized
into `ProgramInstance`.

Portable domain codecs such as NCTX, BCMB, and BERB may remain because they are artifact formats.
Program-specific job payload codecs and `PSContext` are not retained as the permanent runtime ABI.
Existing persisted macro, predicate, address-program, and capture-profile representations remain
unchanged; adapters translate or consume them in memory.

## Completion checks

### Current-phase migration

The current-phase migration is complete only when:

- every module in the catalog activates through the same verifier, executor, and action registry;
- focused native tests cover every supported branch that has a current test or E2E scenario;
- the same immutable state and typed inputs produce the required domain outputs, artifacts, and game
  witnesses, allowing only explicitly declared nondeterministic fields;
- current workflow restart, idempotency, fan-out, survivor selection, and artifact lineage still pass;
- no SavorDb schema migration, stored-representation change, database-service/queue/claim/workflow
  interface change, or artifact-storage-interface change is introduced;
- current predicate records and result fields remain compatible through in-memory composition and
  existing result projection;
- current stop/address/query/baseline behavior lowers through semantic-observation composition with
  equivalent point, ordering, availability, and epoch semantics;
- current input behavior lowers through interaction composition with equivalent
  publication-before-departure, successor causality, acknowledgement, memory-wait, and cleanup behavior;
- existing `savor.capture.profile/1` parsing, sampling, control-observation, window/recorder, progress, and
  artifact behavior remains compatible behind passive `CaptureService`;
- cancellation/fault injection proves complete unwind for every resource type;
- `SavorWorker` has no `ProgramKind` program-selection or payload-decoder switch;
- `PhaseScriptVM`, `PSContext` worker execution, domain opcodes, and `InputMacroRuntime` scheduler are
  deleted after the native modules and production activation are complete;
- no predicate opcode, executor, runtime service, direct guest access, or predicate-specific persistence
  remains after lowering;
- no observation or interaction executor, scheduler, query VM, opcode family, direct Dolphin access, or
  hidden controller remains after lowering;
- no production dual activation path or `PK_UserScript` exists; and
- adding the next phase from existing capabilities changes only a module definition, runtime type
  schemas, and program-kind/runtime integration implementation through existing SavorDb contracts.

### Navmesh Survey prerequisites

Navmesh Survey may start as the first net-new program only after:

1. `soa.navigation.context::capture` passes parity and publishes the exact typed context/state pair;
2. baseline restore and `StateEpoch` behavior are proven on fresh and warm workers;
3. checked `u8`, masked data write, executable patch, teleport/settle, input, and state actions exist with
   scoped receipts;
4. any two-wave workflow fan-out and deterministic reduction use existing SavorDb orchestration
   contracts; if those contracts are insufficient, that continuation remains outside this refactor; and
5. no Survey code is added to the legacy opcode table, `ProgramRegistry`, or a native runner.

## Deferred work

The migration does not decide:

- future authored-program syntax or UI;
- final worker-wire encoding;
- additional legacy external callers not visible in the repository;
- new game behavior beyond current parity;
- any generalized capture-plan authoring language or replacement for `savor.capture.profile/1`;
- generalized trigger/eventhook handling;
- collision anomaly scoring;
- overworld traversal rules; or
- cutscene acceleration algorithms.

## Source references

The BattleRunner source is deletion evidence only. The BattleEndResults-named source, SavorDb directory,
scenario, and tests contain current split-phase compatibility and Results Screen evidence; those names do
not define a monolithic target module.

- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Phases/Programs/SeedProbe/SeedProbeScript.h`
- `SavorCore/Phases/Programs/PlayTasMovie/TasMovieScript.h`
- `SavorCore/Phases/Programs/TasFrameDetector/TasFrameDetectorScript.h`
- `SavorCore/Phases/Programs/BattleRunner/BattleRunnerScript.h` (deletion evidence only)
- `SavorCore/Phases/Programs/BattleContext/BattleContextScript.h`
- `SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h`
- `SavorCore/Phases/Programs/BattleMacroProbe/BattleMacroProbeScript.h`
- `SavorCore/Phases/Programs/BattleCompletion/BattleCompletionScript.h`
- `SavorCore/Phases/Programs/BattleEndResults/BattleEndResultsScript.h` (split-phase orientation and
  monolithic-path deletion evidence)
- `SavorCore/Phases/Programs/NavigationContext/NavigationContextScript.h`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp`
- `SavorDb/Execution/ProgramDB/SeedProbe`
- `SavorDb/Execution/ProgramDB/BattleContext`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn`
- `SavorDb/Execution/ProgramDB/BattleEndResults`
- `SavorDb/Execution/ProgramDB/NavigationContext`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorE2E/SeedProbeRealWorkerScenario.cpp`
- `SavorE2E/TasMovieRealWorkerScenario.cpp`
- `SavorE2E/BattleMacroProbeScenario.cpp`
- `SavorE2E/BattleSingleTurnScenario.cpp`
- `SavorE2E/BattleEndResultsScenario.cpp`
- `SavorTests/test_phase_script_opcodes.cpp`
- `SavorTests/test_input_macro_runtime.cpp`
- `SavorTests/test_battle_macro_probe.cpp`
- `SavorTests/test_battle_end_results.cpp`
- `SavorTests/test_battle_end_results_db.cpp`
- `SavorTests/test_navigation_context_codec.cpp`
- `SavorTests/test_navigation_context_framework.cpp`
- `SavorTests/test_navigation_context_db.cpp`
- `SavorTests/test_savordb_fixture_sqlite.cpp`
