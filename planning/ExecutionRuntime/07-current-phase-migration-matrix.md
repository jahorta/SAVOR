# 07 - Current Phase Migration Matrix

## Status and authority

**Status:** Authoritative migration guidance for the current phase-program corpus.

**Code snapshot:** SAVOR commit
`b584920ffad8dbe770f343e532d7f7386c82fadf`, inspected 2026-07-25.

Current code remains authoritative for legacy behavior. This document is authoritative for how that
behavior reaches the target runtime. The target module IDs and entrypoint names below are semantic
identities; they are not claims about current C++ symbols.

## Purpose and non-goals

This document assigns every current fixed worker program a target module, typed contract, reusable
capability composition, parity obligation, and legacy deletion gate. It prevents an implementer from
deciding ad hoc that a difficult phase needs a separate native controller or permanent compatibility
runtime.

It does not:

- implement a module, action, schema, or DB migration;
- preserve current payload bytes as the target contract;
- make Navmesh Survey part of the current-phase migration corpus;
- move durable workflow decisions into a worker program; or
- require final SQL, C++ declarations, or binary wire layouts.

Navmesh Survey begins only after the migrated Navigation Context module and common runtime pass the
handoff gate at the end of this document.

## Current code evidence

Document 01 and document 12 own the complete evidence inventory. The migration rows below are grounded in
the fixed programs returned by `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-70`, their current
program-specific decoders at `ProgramRegistry.cpp:74-109`, the `PK_*` catalog in
`SavorCore/Runner/IPC/Wire.h:63-83`, and the phase builders under
`SavorCore/Phases/Programs/`. Current DB support is established separately through
`SavorDb/Execution/ProgramDB`; a worker program's presence does not imply that a current DB descriptor or
workflow step exists for it.

## Locked target decisions

### Migration rules

Every current phase follows the same rules:

1. **One executor.** The target is a `ProgramModule` run by `ProgramExecutor`. No migration may introduce
   a phase runner, native controller, factory-selected executor, or second VM.
2. **Exact semantic identity.** Workflow activation names module ID, immutable revision/hash, entrypoint,
   dependency closure, state policy, and typed input. `ProgramKind` is retained only as historical/UI
   metadata during migration.
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
8. **Workflow topology stays durable.** Seed grids, battle waves, battle-end chaining, phase switches,
   fan-out, reduction, and retry across jobs remain coordinator responsibilities.
9. **Local retry must be bounded.** A program may restore its invocation baseline and retry a bounded
   effect when the domain contract requires immediate same-attempt recovery. It may not generate an
   unbounded search frontier.
10. **Delete as each family clears its gate.** Old decoders, context keys, opcode handlers, and registry
    switch cases are removed when their last migrated consumer clears parity; they are not retained for
    speculative compatibility.

## Canonical target module catalog

| Current family | Target module ID | Entrypoint | Long-term disposition |
|---|---|---|---|
| SeedProbe | `soa.seed_probe` | `probe` | Supported |
| TAS playback | `soa.tas_movie` | `play_and_checkpoint` | Supported |
| TAS input-stream detector | `soa.tas_frame_detector` | `detect` | Supported diagnostic program |
| Legacy multi-turn battle path | `soa.battle.legacy_path` | `run` | Migration-only, deprecated after caller audit |
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

## Common legacy-to-target translation

| Current construct | Target treatment |
|---|---|
| `ARM_PHASE_BPS_ONCE`, canonical/gated vectors | Module imports semantic stop-point identities; awaited execution actions acquire scoped router subscriptions |
| `LOAD_SNAPSHOT` and init-time savestate path | Invocation `StatePolicy`; `runtime.state.restore_baseline` is used only for a declared local retry |
| Timeout keys and `SET_TIMEOUT*` | Invocation deadline/budget plus action-specific bounded deadline |
| `RUN_UNTIL_BP*`, frame/opcode stepping | `runtime.execution.continue_until`, `runtime.execution.step_frames`, or `runtime.execution.step_instructions` under the sole `ExecutionEngine` |
| `APPLY_INPUT_FROM`, raw tape application | Scoped `runtime.input.*` actions with poll acknowledgement and mandatory neutral release |
| `READ_*` | Checked `runtime.guest.read_*` actions or typed game observations |
| `WRITE_U32` and future patches | `runtime.guest.write_checked` or `runtime.guest.patch_executable` with receipt and declared restoration policy |
| Movie start/stop and recording | Scoped `runtime.movie.*` actions; stop is guaranteed by unwind |
| Save savestate from a guest path string | `runtime.state.save_artifact` publishes an immutable artifact under an invocation-declared role |
| `GET_BATTLE_CONTEXT`, `GET_NAVIGATION_CONTEXT` | `soa.battle.capture_context` and `soa.navigation.capture_context` |
| Predicate arm/capture/evaluate | Verified predicate definition plus router qualifications and typed observation result |
| `MATERIALIZE_*` and `EXECUTE_*_MACRO_STEP` | `soa.battle.materialize_turn_input` or the named macro subprogram/reducer plus the shared action-await continuation model |
| `EMIT_RESULT` and `RETURN_RESULT` | Typed record/artifact emission and typed domain return |
| `PSContext` keys | Entrypoint input, local, output, emission, diagnostic, and artifact schemas |
| Program-specific payload codec | Workflow-side typed invocation materializer |
| Result INI/context mapper | Temporary compatibility adapter around `ProgramResult`, then typed persistence binding |

## Migration matrix

### `soa.seed_probe::probe`

**Current control flow**

`MakeSeedProbeProgram` restores the VM baseline, applies one input frame, selects a pre-battle gated stop
or field-return canonical stop, reads the RNG seed, optionally compares an expected seed and saves a
state, emits the seed, and returns a run outcome. One worker definition serves neutral, grid, unique, and
field-return workflow variants; the durable adapters generate those variants and later steps.

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
reference. The program emits one `SeedObservation` record; workflow variants persist or reduce it as
appropriate.

**Required composition**

- scoped input lease and guest-observed release;
- semantic stop wait for the selected checkpoint;
- checked RNG `u32` query;
- optional immutable state save; and
- no SeedProbe-specific core opcode.

**Lifecycle duplication removed**

- `OpArmPhaseBps` and `OpLoadSnapshot`;
- timeout and output-path context keys;
- first-byte `PK_SeedProbe` dispatch;
- program-specific payload decoding; and
- use of `DW_RUN_OUTCOME_CODE` as both infrastructure and domain result.

**Parity evidence**

- legacy payload v1/v2 codec and field-return tests;
- existing SeedProbe DB adapter and dynamic grid/unique transition tests;
- `SavorE2E/SeedProbeRealWorkerScenario.cpp`;
- exact observed seed and materialized state semantics for PreBattle and FieldReturn;
- cancellation while waiting leaves neutral input and no router resources.

**Deletion gate**

All SeedProbe workflow step kinds materialize `soa.seed_probe::probe`; result persistence consumes typed
outputs; the E2E scenario uses `ProgramInvocation`; no active caller uses `SeedProbePayload`,
`SeedProbeKeys`, `PK_SeedProbe`, or the SeedProbe branches in `ProgramRegistry`.

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
- semantic stop wait with movie-ended observation;
- immutable state save; and
- routed one-frame advancement only if still required by a verified state-publication invariant.

**Lifecycle duplication removed**

Movie stop becomes scope unwind rather than a branch-sensitive opcode. DTM and output paths become
artifact references. Timeout derivation occurs before activation and is recorded in invocation
provenance.

**Parity evidence**

- current TAS payload derivation tests and DB workflow adapter tests;
- `SavorE2E/TasMovieRealWorkerScenario.cpp`;
- successful and failed playback both stop the movie;
- exact terminal state artifact is usable by the next invocation; and
- wrong disc rejects before movie playback.

**Deletion gate**

`tasmovie.play` uses the typed module, the worker has no TAS payload switch, and no caller sends
`PK_TasMovie`. Any retained `TasMoviePayload` reader is offline import compatibility only and is outside
worker execution.

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

**Parity evidence**

- current payload/codec behavior;
- first sample occurs before the first step;
- one sample follows every stepped frame;
- empty/immediately-ended movies terminate correctly; and
- cancellation finalizes or aborts the artifact according to its declared publication policy and always
  stops playback.

**Deletion gate**

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

`LegacyBattlePathRequest` contains initial input, typed `BattlePath`, predicate definitions, maximum turn
count, and execution bounds. State is an explicit invocation artifact/policy.

**Typed output and emissions**

`LegacyBattlePathResult` contains typed battle outcome, terminal turn index, terminal stop, predicate
observations, and optional final battle context. It emits per-turn observations only when declared by
the module schema.

**Required composition**

- battle-context query;
- legacy path-to-input pure compiler;
- input segment execution;
- semantic battle stop waits; and
- predicate evaluation.

**Disposition**

This module is migration-only and marked deprecated at its first revision. It proves parity for any
external/direct caller that still depends on kind `3`, but new workflows use Battle Context plus Battle
Single Turn. It may import the same reusable battle actions; it receives no legacy executor or special
opcode privileges.

**Parity evidence**

- current payload version and `BattlePath` codec;
- all existing outcome branches;
- predicate abort semantics;
- initial-input and turn-limit behavior; and
- exact action/stop trace comparison against the legacy VM.

**Deletion gate**

After repository and known external caller audit shows no production consumer, remove the deprecated
module as well as `PK_BattleTurnRunner`, `BattleRunnerPayload`, `BattleRunnerScript`, and its context
surface. If a real consumer remains, retain the module—not the old VM—until that consumer migrates.

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

- query current semantic stop;
- wait for `TurnInputs` if necessary; and
- typed `soa.battle` context capture.

**Workflow boundary**

The module does not create waves. The workflow validates the captured output, creates or resolves waves,
and publishes `soa.battle.single_turn::execute` invocations.

**Parity evidence**

- immediate-capture and run-to-capture paths;
- exact current `BattleContext` codec output;
- timeout, wrong-stop, and memory-read failure;
- existing DB tests for direct-wave and bootstrap-wave fan-out; and
- restart/idempotency around transition publication.

**Deletion gate**

Both `battle.context_probe` and `battle_chain` bootstrap materialize the target invocation; transitions
consume its typed output; `PK_BattleContextProbe`, the payload codec, and `GET_BATTLE_CONTEXT` opcode path
have no caller.

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
- shared input-segment action with router wake subscriptions, memory baselines/change waits, and input
  poll acknowledgement;
- battle-context query; and
- routed observation-tail execution.

No single “run battle macro probe” native action is allowed. The program owns visible branching in IR;
the reducer decides only the next bounded segment from typed state and the preceding completion.

**Parity evidence**

- all compiler/planning-context tests in `test_battle_macro_probe.cpp`;
- provider permission and unexpected-stop behavior;
- fake-attack pattern and memory-gate ordering;
- `InputMacroRuntime` cleanup-once cases;
- direct-worker SavorE2E scenarios; and
- cancellation at every segment boundary leaves neutral acknowledged input and no subscriptions.

**Deletion gate**

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
- verified predicate set;
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
- predicate and macro outcomes;
- optional terminal `BattleContext`;
- optional successor `StateArtifact`; and
- action, input-receipt, mutation, capture, and branch trace references.

**Required composition**

- semantic battle waits and routed stepping;
- checked RNG read and checked scoped write with readback receipt;
- the same command compiler/reducer/input-segment composition used by Macro Probe;
- predicate qualification and evaluation;
- scoped capture attachment;
- immutable state save; and
- explicit baseline restore for the one bounded local retry, which advances `StateEpoch`.

The RNG mutation's lifetime is declared. It may persist in the disposable emulated branch until restore
or state publication, but its receipt and readback remain part of provenance. It cannot be an untracked
raw write.

**Workflow boundary**

The program returns one candidate. It never chooses survivors, creates a battle wave, or schedules
another turn. Existing deterministic reduction selects best candidates, creates next-wave records, and
appends the next set of exact module invocations.

**Parity evidence**

- Battle Turn payload and program-shape tests;
- RNG override original/requested/applied evidence;
- first-turn and later-turn prelude behavior;
- macro retry and retry-exhaustion paths;
- next-turn, victory, defeat, turn-limit, predicate, materialization, and execution outcomes;
- capture memory-watchpoint behavior;
- `SavorE2E/BattleSingleTurnScenario.cpp`;
- DB survivor selection and multi-wave dynamic-step tests; and
- differential action/branch traces for the existing three first-turn checkpoint corpus where
  applicable.

**Deletion gate**

All `battle.single_turn` jobs use the target module and typed result; durable wave spawning remains
idempotent after restart; no VM macro opcode or direct memory/write path is used; kind `5`, payload
version `5`, and Battle Single Turn branches in `ProgramRegistry` are removed.

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
- shared input-segment action;
- typed battle reward/context queries;
- immutable manifest publication; and
- immutable state save.

**Parity evidence**

- BCMB round trip and expected-view derivation;
- pre/post capture and reward-phase validation;
- wrong-source and read-failure cases;
- exactly acknowledged input/release behavior;
- current DB aggregate/result identity checks; and
- cleanup/cancellation between every provider segment.

**Deletion gate**

`battle.completion` binds typed inputs/outputs, downstream steps consume the declared manifest/state
artifacts, and kind `9`, completion payload/context keys, materialize opcode, and old result mapper path
have no active caller.

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
- shared input-segment action;
- battle results/lifecycle queries;
- immutable report publication; and
- immutable state save.

**Parity evidence**

- BERB round trip and payload policy tests;
- source/manifest qualification;
- stat, learned-magic, item, mandatory-confirm, and fade branches;
- RequiredOnly and FullAdaptive behavior;
- fresh input-epoch and causal release requirements;
- RNG/lifecycle/completion invariants;
- `SavorE2E/BattleEndResultsScenario.cpp`; and
- DB end-workflow idempotency and artifact-lineage tests.

**Deletion gate**

The split `battle.results_screen` workflow uses the target module, all report/state consumers use typed
artifact bindings, and kind `8`, its compatibility alias, payload/context keys, materialize opcode, and
old result mapper are removed.

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

NCTX v1 remains a portable artifact codec. The target typed record, not an unscoped context blob, is the
program/workflow contract.

**Required composition**

- scoped neutral input with observed release;
- current-stop query and semantic navigation stop wait;
- typed `soa.navigation` context capture;
- NCTX artifact writer; and
- immutable state save.

**Parity evidence**

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

**Deletion gate**

`navigation.context_probe` uses the target module, current `.nctx` and state artifact consumers remain
compatible, no caller uses kind `10`, Navigation Context payload/context keys, `GET_NAVIGATION_CONTEXT`,
or its `ProgramRegistry` branches, and the Survey handoff below passes.

## Input macro consolidation

The current providers must migrate once, not independently inside each battle phase:

1. Convert each provider's mutable controller into a typed reducer state record.
2. Translate `Start` into the reducer's initial transition and `Advance` into a transition over one
   completed segment result.
3. Represent a provider decision as one of:
   - request one bounded input/execution/memory effect;
   - return completed;
   - return typed domain failure.
4. Store the pending continuation in `ProgramInstance`.
5. Execute provider plans through the same registered action handlers and `ExecutionEngine` used by
   ordinary program operations.
6. Acquire input, router, watchpoint, and capture resources through the invocation scope stack.
7. Preserve the cleanup-once behavioral tests, but make the common scope unwinder the mechanism.
8. Remove `IInputMacroHost`, `IInputMacroDriverHost` physical-service access, VM private inheritance,
   local exclusive-session state, and `InputMacroRuntime` as a scheduler.

The reusable battle command, completion, and results reducers may remain native C++ because they perform
complex game-specific decisions. They remain pure: typed state plus typed completion in; next requested
effect/events/completion out.

## Ordered migration and dependency gates

| Order | Family | Gate proved before moving on |
|---|---|---|
| 1 | SeedProbe | Core IR, typed invocation/result, stop wait, input scope, memory read, state save, basic workflow binding |
| 2 | Navigation Context | Navigation capability pack, qualified capture, immutable NCTX/state pair, and artifact lineage needed by Survey |
| 3 | TAS playback and detector | Movie scope, routed step, streamed/bounded artifacts, Boot policy |
| 4 | Battle Context | Typed capability-pack query and workflow fan-out from a typed result |
| 5 | Battle Macro Probe | Common adaptive reducer/action continuation and input acknowledgement |
| 6 | Battle Single Turn | Mutation receipt, capture, local restore/epoch, predicates, complex result, durable arbitrary turn waves |
| 7 | Battle Completion and Results Screen | Multiple typed artifacts, manifest/report invariants, shared reducer infrastructure |
| 8 | Legacy battle path | Final caller audit and deprecated-module parity before old interpreter deletion |

The cutover plan may overlap implementation work where dependencies permit, but it may not reverse a
gate. No net-new phase behavior lands on the legacy VM once the translator exists.

## Interfaces and ownership affected

Migration changes:

- worker activation from `ProgramKind` to exact `ProgramInvocation`;
- program selection from `ProgramRegistry` switches to `ProgramDefinitionStore`;
- payload materialization from byte codecs to typed schema binding;
- fixed builders from runtime definitions to compiler inputs or direct module builders;
- VM host calls to registered actions over session services;
- input macro providers to pure reducers;
- result mappers from `PSContext`/INI extraction to typed result/artifact binding;
- affinity from kind/bootstrap strings to exact module/dependency/state/runtime locality; and
- workflow step registrations from descriptors that select worker code to adapters that bind exact
  modules and typed domain persistence.

Workflow transition handlers may remain domain-specific during migration. They must consume typed
outputs and may not become worker controllers.

## Failure and cleanup behavior

Every parity test must classify legacy terminal behavior into:

- **infrastructure status:** verification, backend, transport, service, or action-contract failure;
- **domain outcome:** expected game result such as seed mismatch, defeat, predicate rejection,
  no-progress, unexpected qualified stop, or invalid capture; and
- **cleanup/session status:** whether all acquired resources were released/restored and the session is
  reusable.

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
- workflow/frontier transaction boundaries.

DB migrations can be staged behind typed compatibility adapters, but a worker-side legacy/new
controller split is forbidden. In-flight legacy jobs are drained or terminated at the release boundary;
live `PhaseScriptVM` state is never serialized into `ProgramInstance`.

Portable domain codecs such as NCTX, BCMB, and BERB may remain because they are artifact formats.
Program-specific job payload codecs and `PSContext` are not retained as the permanent runtime ABI.

## Acceptance criteria

### Global acceptance and deletion criteria

The current-phase migration is complete only when:

- every module in the catalog activates through the same verifier, executor, and action registry;
- differential tests cover every legacy branch that has a current test or E2E scenario;
- the same immutable state and typed inputs produce equivalent domain outputs and action/branch traces,
  allowing only explicitly declared nondeterministic fields;
- current workflow restart, idempotency, fan-out, survivor selection, and artifact lineage still pass;
- cancellation/fault injection proves complete unwind for every resource type;
- `SavorWorker` has no `ProgramKind` program-selection or payload-decoder switch;
- `PhaseScriptVM`, `PSContext` worker execution, domain opcodes, and `InputMacroRuntime` scheduler are
  deleted after the bounded differential window;
- no production dual activation path or `PK_UserScript` exists; and
- adding the next phase from existing capabilities changes only a module definition, schemas, and
  workflow bindings.

### Navmesh Survey handoff

Navmesh Survey may start as the first net-new program only after:

1. `soa.navigation.context::capture` passes parity and publishes the exact typed context/state pair;
2. baseline restore and `StateEpoch` behavior are proven on fresh and warm workers;
3. checked `u8`, masked data write, executable patch, teleport/settle, input, and state actions exist with
   scoped receipts;
4. two-wave workflow fan-out and deterministic reduction use the common orchestration contracts; and
5. no Survey code is added to the legacy opcode table, `ProgramRegistry`, or a native runner.

## Deferred work

The migration does not decide:

- future authored-program syntax or UI;
- final SQL/wire encoding;
- additional legacy external callers not visible in the repository;
- whether the deprecated legacy battle-path module is retained after that caller audit;
- new game behavior beyond current parity;
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

Claim-level evidence and conflict disposition are indexed in `12-source-evidence-map.md`.
