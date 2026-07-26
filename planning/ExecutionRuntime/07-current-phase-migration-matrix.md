# 07 - Current Phase Migration Matrix

## Scope

This matrix is implementation guidance for moving the current fixed phase-program corpus to the target
runtime. Recheck the current repository code for legacy behavior before changing a program family. The
target module IDs and entrypoint names below are semantic identities; they are not claims about current
C++ symbols.

## Purpose and non-goals

This document assigns every current fixed worker program a target module, typed contract, reusable
capability composition, parity checks, and conditions for legacy deletion. It prevents an implementer from
deciding ad hoc that a difficult phase needs a separate native controller or permanent compatibility
runtime.

It does not:

- implement a module, action, or runtime type schema;
- change SavorDb SQL/schema, migrations, stored representations, database-service interfaces, queues,
  claims, workflow persistence, transaction boundaries, or artifact-storage interfaces;
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
   independent. Current `PSResult::ok` and context keys are translated only at the legacy edge.
7. **One differential window.** A temporary compiler from current `PhaseScript` builders may produce the
   new IR for trace comparison. It is never a second production executor and accepts no new feature.
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
| TAS playback | `soa.tas_movie` | `play_and_checkpoint` | Supported |
| TAS input-stream detector | `soa.tas_frame_detector` | `detect` | Supported diagnostic program |
| Legacy multi-turn battle path | `soa.battle.legacy_path` | `run` | Migration-only, deprecated after consumer checks |
| Battle Context | `soa.battle.context` | `capture` | Supported |
| Battle Single Turn | `soa.battle.single_turn` | `execute` | Supported |
| Battle Macro Probe | `soa.battle.macro_probe` | `probe` | Supported diagnostic program |
| Battle Completion | `soa.battle.completion` | `complete` | Supported |
| Battle Results Screen | `soa.battle.results_screen` | `advance` | Supported |
| Navigation Context | `soa.navigation.context` | `capture` | Supported |

Module IDs do not imply one file per module. They are immutable semantic families in
`ProgramDefinitionStore`. All revisions use the common IR, verifier, invocation, action, result, and
artifact contracts.

### Canonical reusable imports used by the migrations

Document 04 owns the descriptors and signatures. The migration uses these exact logical IDs:

| Capability | Logical import |
|---|---|
| State lifecycle | `runtime.state.capture_baseline`, `runtime.state.restore`, `runtime.state.restore_baseline`, `runtime.state.save_artifact` |
| Emulator advancement | `runtime.execution.continue_until`, `runtime.execution.step_instructions`, `runtime.execution.step_frames` |
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
handling. `PausedAtPoint` performs ordinary reads while execution is paused. Observing after an
instruction requires an explicit step followed by another observation; it is not a hidden acquisition
mode.

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
source-stop step-off behavior, reached-instruction policy, attached observations/checks,
timeout/stall/movie/cancel policy, and typed completion mapping. `InteractionSegmentResult` preserves the
exact stop receipt, request and release receipts, ordered observations/checks, elapsed evidence, epoch,
and distinct terminal status.

Before verification, the composer lowers static sequences and adaptive reducers into ordinary
subprogram CFG, action awaits, semantic-observation composition, branches, and emissions. A reducer may
select only a declared segment ID; it cannot construct effects or access a runtime service. There is no
`InteractionRuntime`, segment scheduler, macro opcode family, or action that hides a whole macro.

The migration preserves the current temporal behavior:

- publish the segment input and obtain its epoch before stepping off a current semantic stop;
- execute exactly one source instruction with the new input before waiting;
- match completion by logical point, PC, stop sequence, and epoch;
- preserve whether the reached instruction remains paused or executes under the held request;
- read the request receipt after any held-through-hit execution and before publishing neutral;
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

## Common legacy-to-target translation

| Current construct | Target treatment |
|---|---|
| `ARM_PHASE_BPS_ONCE`, canonical/gated vectors | `SemanticPointDefinition` and `SemanticAwaitDefinition` lower exact alternatives and current-point policy into scoped router subscriptions plus `runtime.execution.continue_until` |
| `LOAD_SNAPSHOT` and init-time savestate path | Invocation `StatePolicy`; `runtime.state.restore_baseline` is used only for a declared local retry |
| Timeout keys and `SET_TIMEOUT*` | Invocation deadline/budget plus action-specific bounded deadline |
| `RUN_UNTIL_BP*`, frame/opcode stepping | `runtime.execution.continue_until`, `runtime.execution.step_frames`, or `runtime.execution.step_instructions` under the sole `ExecutionEngine` |
| `APPLY_INPUT_FROM`, raw tape application | Interaction composition lowers requested input, semantic gates, poll acknowledgements, and neutral-release witnesses into scoped `runtime.input.*` and execution actions |
| `READ_*`, address programs, direct query helpers | `AddressExpression<T>`, `ObservationDefinition<T>`, and `ObservationUse<T>` lower to checked `runtime.guest.read_*` actions or registered coherent game queries |
| `WRITE_U32` and future patches | `runtime.guest.write_checked` or `runtime.guest.patch_executable` with receipt and declared restoration policy |
| Movie start/stop and recording | Scoped `runtime.movie.*` actions; stop is guaranteed by unwind |
| Save savestate from a guest path string | `runtime.state.save_artifact` publishes an immutable artifact under an invocation-declared role |
| `GET_BATTLE_CONTEXT`, `GET_NAVIGATION_CONTEXT` | `soa.battle.capture_context` and `soa.navigation.capture_context` |
| Predicate arm/capture/evaluate | Shared predicate composition consumes semantic-observation results, lowers comparison and reaction to ordinary IR, and optionally emits declared `ConditionObservation` progress |
| Input-macro `Start`/`Advance`, plans, and `EXECUTE_*_MACRO_STEP` | Interaction composition lowers initialization/advancement reducers and verifier-known segment definitions into ordinary subprogram CFG, semantic awaits/observations, input actions, and emissions |
| Existing capture profile | `runtime.capture.attach` passes the unchanged `savor.capture.profile/1` artifact to passive `CaptureService`; profile internals are not lowered or redesigned |
| `EMIT_RESULT` and `RETURN_RESULT` | Typed record/artifact emission and typed domain return |
| `PSContext` keys | Entrypoint input, local, output, emission, diagnostic, and artifact schemas |
| Program-specific payload codec | Program-kind runtime adapter decodes existing persisted job/domain data and constructs typed `ProgramInvocation` input |
| Result INI/context mapper | Program-kind result adapter consumes `ProgramResult` and writes through existing result/domain persistence operations |

## Migration matrix

### `soa.seed_probe::probe`

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

### `soa.tas_movie::play_and_checkpoint`

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
- scoped movie playback;
- semantic await with movie-ended observation;
- immutable state save; and
- routed one-frame advancement only if still required by a verified state-publication invariant.

**Lifecycle duplication removed**

Movie stop becomes scope unwind rather than a branch-sensitive opcode. DTM and output paths become
artifact references. Timeout derivation occurs before activation and is recorded in invocation
provenance.

**Parity checks**

- current TAS payload derivation tests and DB workflow adapter tests;
- `SavorE2E/TasMovieRealWorkerScenario.cpp`;
- successful and failed playback both stop the movie;
- exact terminal state artifact is usable by the next invocation; and
- wrong disc rejects before movie playback.

**Legacy removal condition**

`tasmovie.play` uses the typed module, the worker has no TAS payload switch, and no caller sends
`PK_TasMovie`. `TasMoviePayload` may remain behind the SavorDb program-kind handler or a read-only
historical boundary to preserve existing stored records; it is outside worker execution.

### `soa.tas_frame_detector::detect`

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

### `soa.battle.legacy_path::run`

**Current control flow**

The legacy `PK_BattleTurnRunner` is a multi-turn path runner, distinct from Battle Single Turn. It
applies an initial frame, repeatedly captures battle context, materializes raw input frames from a
`BattlePath`, runs among battle stop points, evaluates predicates, and returns victory, defeat,
turns-exhausted, predicate failure, materialization failure, or execution failure.

**Typed input**

`LegacyBattlePathRequest` contains initial input, typed `BattlePath`, predicate-composition inputs,
maximum turn count, and execution bounds. State is an explicit invocation artifact/policy.

**Typed output and emissions**

`LegacyBattlePathResult` contains typed battle outcome, terminal turn index, terminal stop, typed
condition observations and predicate compatibility summary, and optional final battle context. It emits
per-turn observations only when declared by the module schema.

**Required composition**

- battle-context query;
- legacy path-to-input pure compiler;
- interaction-composed input execution;
- semantic battle awaits and typed observations; and
- shared predicate composition and typed condition observations.

**Disposition**

This module is migration-only and marked deprecated at its first revision. It proves parity for any
external/direct caller that still depends on kind `3`, but new workflows use Battle Context plus Battle
Single Turn. It may import the same reusable battle actions; it receives no legacy executor or special
opcode privileges.

**Parity checks**

- current payload version and `BattlePath` codec;
- all existing outcome branches;
- predicate qualification, baseline, comparison, progress, passed/total, and abort semantics;
- initial-input and turn-limit behavior; and
- exact action/stop trace comparison against the legacy VM.

**Legacy removal condition**

After repository references and known external consumers show no production use, remove the deprecated
module, `PK_BattleTurnRunner`, `BattleRunnerScript`, and its context
surface. If a real consumer remains, retain the module—not the old VM—until that consumer migrates.

Retain a read-only `BattleRunnerPayload` codec if existing stored records require it.

### `soa.battle.context::capture`

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

### `soa.battle.macro_probe::probe`

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
  memory baseline/change observations, and explicit reached-instruction policy;
- battle-context observation/query; and
- routed observation-tail execution.

No single “run battle macro probe” native action is allowed. The program owns visible branching in IR;
the reducer decides only the next bounded segment from typed state and the preceding completion.

**Parity checks**

- all compiler/planning-context tests in `test_battle_macro_probe.cpp`;
- provider permission and unexpected-stop behavior;
- fake-attack pattern, baseline-before-advance, one-neutral-frame polling, and memory-gate ordering;
- input-before-step, exact point/PC/sequence/epoch matching, held-through-hit, request-receipt-before-neutral,
  and fresh-neutral release-witness behavior;
- `InputMacroRuntime` cleanup-once cases;
- direct-worker SavorE2E scenarios; and
- cancellation at every segment boundary leaves neutral acknowledged input and no subscriptions.

**Legacy removal condition**

Direct SavorE2E activation uses `ProgramInvocation`; all provider behavior is reachable through the common
reducer/action path; `PK_BattleMacroProbe`, `MATERIALIZE_BATTLE_MACRO_STEPS`,
`EXECUTE_BATTLE_MACRO_STEP`, the VM macro-host inheritance, and the subordinate runtime scheduler are
removed.

### `soa.battle.single_turn::execute`

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
or state publication, but its receipt and readback remain part of provenance. It cannot be an untracked
raw write.

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
- differential action/branch traces for the existing three first-turn checkpoint corpus where
  applicable.

**Legacy removal condition**

All `battle.single_turn` jobs use the target module and typed runtime result; the result handler projects
that result into the existing wave operations; durable wave spawning remains idempotent after restart;
no VM macro opcode or direct memory/write path is used; and kind `5` plus the Battle Single Turn branches
in `ProgramRegistry` are removed from worker execution. Existing persisted payload version `5` remains
readable by the SavorDb handler.

### `soa.battle.completion::complete`

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

- pure completion provider/reducer;
- shared interaction composition;
- typed battle reward/context queries;
- immutable manifest publication; and
- immutable state save.

**Parity checks**

- BCMB round trip and expected-view derivation;
- pre/post capture and reward-phase validation;
- wrong-source and read-failure cases;
- exactly acknowledged input/release behavior;
- current DB aggregate/result identity checks; and
- cleanup/cancellation between every provider segment.

**Legacy removal condition**

The `battle.completion` program-kind adapter translates existing inputs to the typed runtime contract and
projects its manifest/state outputs into the current downstream representation. Kind `9`, completion
context keys, materialize opcode, and the old worker result path have no active worker-side caller;
stored payload/result codecs remain where existing SavorDb operations require them.

### `soa.battle.results_screen::advance`

**Current control flow**

The split Results Screen phase consumes a completion manifest and field-return seeded state, validates
the expected presentation, adaptively advances required/full results UI actions, reaches the
field-return reseed boundary, saves the terminal state, and publishes a detailed BERB report.

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

**Parity checks**

- BERB round trip and payload policy tests;
- source/manifest qualification;
- stat, learned-magic, item, mandatory-confirm, and fade branches;
- RequiredOnly and FullAdaptive behavior;
- fresh input-epoch and causal release requirements;
- RNG/lifecycle/completion invariants;
- `SavorE2E/BattleEndResultsScenario.cpp`; and
- DB end-workflow idempotency and artifact-lineage tests.

**Legacy removal condition**

The split `battle.results_screen` program-kind adapter uses the target module and translates typed
report/state artifacts into the existing downstream representation. Kind `8`, its compatibility alias,
context keys, materialize opcode, and old worker result path are removed from worker execution; existing
stored payload/result codecs and bindings remain unchanged.

### `soa.navigation.context::capture`

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
6. Preserve input publication/epoch before source-stop step-off, exact completion identity,
   held-through-hit behavior, request and release receipts, baseline/change ordering, and separately
   witnessed neutral release.
7. Acquire one interaction-wide input lease and nested router, observation, watchpoint, and capture
   resources through the invocation scope stack.
8. Preserve the cleanup-once behavioral tests, but make the common scope unwinder the mechanism.
9. Remove `IInputMacroHost`, `IInputMacroDriverHost` physical-service access, VM private inheritance,
   local exclusive-session state, and `InputMacroRuntime` as a scheduler.

The reusable battle command, completion, and results reducers may remain native C++ because they perform
complex game-specific decisions. They remain pure: typed state plus typed completion in; next requested
effect/events/completion out.

## Migration dependencies

| Default order | Family | Prerequisites |
|---|---|---|
| 1 | SeedProbe | Core IR, typed invocation/result, stop wait, input scope, memory read, state save, existing SavorDb handler parity |
| 2 | Navigation Context | Navigation capability pack, qualified capture, immutable NCTX/state pair, and artifact lineage needed by Survey |
| 3 | TAS playback and detector | Movie scope, routed step, streamed/bounded artifacts, Boot policy |
| 4 | Battle Context | Typed capability-pack query and existing workflow fan-out after handler projection |
| 5 | Battle Macro Probe | Shared semantic-observation and interaction composition, adaptive reducer transitions, exact temporal ordering, and request/release acknowledgement |
| 6 | Battle Single Turn | Mutation receipt, unchanged capture-profile semantics, local restore/epoch, shared predicate and observation composition, complex result, and parity with existing durable turn waves |
| 7 | Battle Completion and Results Screen | Multiple typed artifacts, manifest/report invariants, shared reducer infrastructure |
| 8 | Legacy battle path | Repository reference check, known external consumer check, and deprecated-module parity before old interpreter deletion |

Use this as the default dependency order. Work may overlap or move earlier when its listed prerequisites
are satisfied. No net-new phase behavior lands on the legacy VM once the translator exists.

## Interfaces and ownership affected

Migration changes:

- program-kind handlers construct exact `ProgramInvocation` from existing persisted records;
- program selection from `ProgramRegistry` switches to `ProgramDefinitionStore`;
- worker input from decoded existing job/domain data to typed runtime values;
- fixed builders from runtime definitions to compiler inputs or direct module builders;
- current stop/address/query/baseline constructs to shared semantic-observation composition before module
  verification;
- current predicate records to shared in-memory predicate composition before module verification;
- VM host calls to registered actions over session services;
- input macro providers to pure reducers plus verifier-known interaction segments;
- existing `savor.capture.profile/1` artifacts to passive `CaptureService` without representation or
  semantic conversion;
- program-kind result handlers project `ProgramResult` through existing result/domain operations; and
- exact module/dependency/state/runtime validation occurs at worker activation without changing current
  SavorDb affinity, queue, or claim representations.

Existing workflow transition handlers remain domain-specific and use their current interfaces and
storage. An adapter may supply values derived from typed runtime output; the handler may not become a
worker controller.

## Failure and cleanup behavior

Every parity test must classify legacy terminal behavior into:

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
- differential tests cover every legacy branch that has a current test or E2E scenario;
- the same immutable state and typed inputs produce equivalent domain outputs and action/branch traces,
  allowing only explicitly declared nondeterministic fields;
- current workflow restart, idempotency, fan-out, survivor selection, and artifact lineage still pass;
- no SavorDb schema migration, stored-representation change, database-service/queue/claim/workflow
  interface change, or artifact-storage-interface change is introduced;
- current predicate records and result fields remain compatible through in-memory composition and
  existing result projection;
- current stop/address/query/baseline behavior lowers through semantic-observation composition with
  equivalent point, ordering, availability, and epoch semantics;
- current input macros lower through interaction composition with equivalent input-before-step,
  held-through-hit, acknowledgement, memory-wait, and cleanup behavior;
- existing `savor.capture.profile/1` parsing, sampling, control-observation, window/recorder, progress, and
  artifact behavior remains compatible behind passive `CaptureService`;
- cancellation/fault injection proves complete unwind for every resource type;
- `SavorWorker` has no `ProgramKind` program-selection or payload-decoder switch;
- `PhaseScriptVM`, `PSContext` worker execution, domain opcodes, and `InputMacroRuntime` scheduler are
  deleted after the bounded differential window;
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
- whether the deprecated legacy battle-path module is retained after the consumer check;
- new game behavior beyond current parity;
- any generalized capture-plan authoring language or replacement for `savor.capture.profile/1`;
- generalized trigger/eventhook handling;
- collision anomaly scoring;
- overworld traversal rules; or
- cutscene acceleration algorithms.

## Source references

- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Phases/Programs/SeedProbe/SeedProbeScript.h`
- `SavorCore/Phases/Programs/PlayTasMovie/TasMovieScript.h`
- `SavorCore/Phases/Programs/TasFrameDetector/TasFrameDetectorScript.h`
- `SavorCore/Phases/Programs/BattleRunner/BattleRunnerScript.h`
- `SavorCore/Phases/Programs/BattleContext/BattleContextScript.h`
- `SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h`
- `SavorCore/Phases/Programs/BattleMacroProbe/BattleMacroProbeScript.h`
- `SavorCore/Phases/Programs/BattleCompletion/BattleCompletionScript.h`
- `SavorCore/Phases/Programs/BattleEndResults/BattleEndResultsScript.h`
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
