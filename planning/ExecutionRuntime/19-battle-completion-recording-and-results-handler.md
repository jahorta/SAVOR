# Battle Completion, Recording, and Results Handler

## Status and authority

This document is the normative ExecutionRuntime contract for the Battle exit
pipeline. It supersedes every older runtime-planning statement that treats the
Results Screen as `battle.results_screen`, a Full Phase, a workflow node, a
database aggregate, or an independently saved state. It also supersedes older
Battle Completion contracts that stopped before the accepted field preseed or
used legacy phase-specific input/publication behavior.

The production units are:

- `soa.battle.completion/complete`, registered as `battle.completion`;
- `soa.battle.record/record`, registered as `battle.record`; and
- `soa.battle.replay/replay`, registered as `battle.replay`; and
- `soa.battle.results.handler`, a reusable static interaction lowered into a
  future downstream phase and never registered as a Full Phase.

## Shared completion boundary

Battle Completion and Battle Record lower the same adaptive completion
interaction. It owns one controller lease, applies neutral through the
state-oriented input contract, and accepts either registered causal path:

- `0x8006F554` to `0x8006F558`; or
- `0x8006F590` to `0x8006F594`.

It then reaches reward entry `0x8006F598` and reward commit `0x8006FD58`,
requires `battleInputState == 2`, and captures the evidence used to build
`BattleCompletionManifestV1`. Every foreground wait is unbounded by a
phase-supplied frame or elapsed-time limit. External cancellation and the
session health monitor remain the only liveness authorities.

Both phases then wait for one accepted field preseed:

- `0x80101894` when `u32[0x803475D4] != 0`; or
- otherwise `0x801018AC`.

At the accepted point the worker captures a typed `FieldTransitionContextV1`.
It reconstructs `me%03d%c.sct` from area `u32[0x80311AC4]` and raw suffix
`u8[0x80311AC8]`; area 99 replaces the suffix with
`u8[0x80310A22] + 'a'`. The worker reports the filename and provenance but does
not select the next program kind. Coordination maps it to
`OverworldNavigation`, `FieldNavigation`, `Cutscene`, or `ShipRuntime`. None of
those routes creates a Battle continuation.

## `battle.completion`

This phase is a planning operation over one explicitly selected durable
Victory candidate. Its one-item workset restores that Victory successor,
requires exact entry at `0x800706D8`, uses empty predicate and derived-state
bindings, and permits one attempt.

Success atomically publishes:

- one `BATTLE_COMPLETION` `.bcmb` artifact containing the versioned completion
  manifest;
- one movie-inactive savestate at accepted field preseed; and
- one relational `BattleCompletion` aggregate relating both outputs to the
  selected BattleSet, wave, turn job, execution job, and entry state.

The manifest records entry, reward-entry, reward-commit, and preseed
provenance; Battle RNG; pre/post character state; reward values and drops;
expected Results presentation; and field-transition context. Any inconsistent
hook, reward, transition, or lineage is a job failure and publishes neither
artifact.

## `battle.record`

This phase is an explicit route-commit operation requested only after a
successful standalone completion. Coordination reconstructs the selected
lineage and sends a closed, source-neutral `BattleReplayPlanV1`; the worker
never queries a database or resolves symbolic commands. The plan includes the
confirmed initial SeedProbe frame, ordered concrete turn commands and
fake-attack counts, per-turn expected terminal/RNG, selected completion
lineage and manifest, and a canonical plan hash. A separate
`BattleReplaySourceBindingV1` carries the exact recording checkpoint/DTM/TMI
identities and hashes.

The workset has exactly one item and uses a `Savestate` baseline containing
the movie-paired Battle-entry checkpoint plus its exact DTM continuation
sidecar. It does not use `ReadOnlyMovie`, which is reserved for TAS Movie
phases that explicitly opt to start from a DTM-declared origin and is never
their default. Initialization restores the checkpoint and its movie cursor
normally. Before the guest advances, the
program calls `runtime.movie.adopt_restored_read_only_playback` to adopt that
already-restored playback session into its invocation scope, then
`MovieService` branches the exact cursor into recording. Adoption performs no
movie preparation, core restart, state restore, or guest advancement. Movie
recording observes controller states, while `InputArbiter` remains the sole
controller publisher. No phase input tape or competing movie-input owner is
introduced.

`MovieService` is the sole authority for the movie state throughout this
handoff. Paused restoration reconciles the exact restored cursor as
`ReadOnlyPlayback`, and a successful branch performs one native verification,
commits `Recording`, and restores the prior pause-at-playback-end configuration
before the first Battle Record `ContinueUntil`. Running `ExecutionEngine`
maintenance reads only `MovieService`'s cached state and cursor; it performs no
physical movie query. Thus a recording branch cannot be mistaken for playback
exhaustion. Genuine source-DTM exhaustion makes Dolphin pause, after which the
confirmed paused boundary is reconciled as a `MovieEndedPolicy::Fail` canary.

The program reuses canonical SeedProbe delivery, live Battle Context
validation, adaptive Battle commands, and the shared completion interaction.
Predicates are disabled. Every selected turn terminal and ending RNG is
required. The final completion and transition must semantically equal the
standalone manifest; artifact identity, worker identity, timestamps, VI
timing, predicate data, progress, and other incidental provenance are excluded
from semantic equality.

On success, checkpoint capture occurs while paused at accepted preseed. The
program records through observation of the terminal neutral publication,
finalizes the DTM, and verifies source prefix/cursor consistency. Coordination
publishes the DTM, inherited-plus-appended TMI, paired preseed checkpoint,
final-command `BattleTimingAdjustmentAnchorV1`, `BattleRecording` aggregate,
and unvalidated TAS Movie tree. Validation is scheduled automatically;
sterilization is scheduled only for `Valid`.

`ReplayMismatch` is a successful domain result. It retains typed diagnostics
and canonical progress but publishes no DTM, itinerary, checkpoint, timing
anchor, TAS tree, or other partial movie artifact. Invalid replay plans and
runtime/infrastructure errors remain job failures.

## `battle.replay`

Battle Replay is the non-recording control for Battle Record. Both descriptors
call the same canonical replay-plan builder, persist the same exact BRP1 bytes
and hash, and lower the same synchronized SeedProbe delivery, adaptive turn
commands, terminal/RNG checks, and shared completion comparison.

Replay's separate BSB1 source binding always names the exact
`BattleSet.entry_savestate_id`; it never substitutes the movie-paired ancestor
used by Record. Coordination qualifies and stages a complete movie-inactive
SAV without a sidecar, or the exact movie-paired SAV plus its validated DTM
sidecar. Both are `Savestate` baselines. Durable metadata controls staging but
does not choose the runtime branch.

After restore, `runtime.movie.observe_state` obtains the canonical state from
`MovieService`. `Inactive` qualifies `BeforeRandSeedSet` and enters the shared
body directly. `ReadOnlyPlayback` is adopted while paused, qualifies the same
PC, is explicitly stopped, and must then re-observe `Inactive`. Every other
state fails before input publication or guest advancement. The inactive path
creates no playback resource; the paired path releases its controller
reservation before adaptive input begins. Replay permits playback only for
this conditional startup handoff and does not permit recording.

`Matched` and `ReplayMismatch` are coherent successful domain outcomes. The
relational `BattleReplay` aggregate retains exact completion/source lineage,
the canonical source binding and replay plan, nullable DTM/itinerary
references, execution identity, mismatch RNG evidence, optional
observed completion/transition evidence, and the worker terminal hash. Replay
declares no artifact schemas and publishes no DTM, itinerary, checkpoint,
timing anchor, TAS tree, validation request, or sterilization request. It has
no automatic downstream transition.

### Live adaptive-operation diagnostics

`SavorWorker` owns an asynchronous, bounded logger and drains it only after
the worker runtime and Dolphin wrapper have been destroyed. Runtime producers
format records but never wait for queue capacity or perform sink I/O; queue
contention and overflow drop the new record and are summarized by severity by
the logger thread.

Cancellation-driven execution operations retain their IR selector as a
worker-local stop-consumer label. While an operation is active, its ordinary
maintenance observation produces at most one diagnostic heartbeat per second.
That record names the selector and ordered foreground PCs and includes the
existing core/movie evidence plus a read-only inspection of the current input
relationship: publication epoch, controller buttons, all override callbacks,
exact `Buttons/A` callbacks, recording input cursor, and cursor delta. The
inspection never completes or mutates the input relationship, and it performs
no additional core or movie query. No logger call is allowed from controller
override or native-stop CPU callbacks.

## Results Screen handler

The Results Screen belongs to the downstream domain phase selected by the
preseed filename. That phase receives the completion manifest and sterilized
preseed checkpoint, performs the generic SeedProbe/TBR transition to field
postseed `0x801012B4`, and invokes the handler before its own exploration,
cutscene, or ship-runtime behavior.

The handler is a static homogeneous-worker capability and
`InteractionComposition` component. It owns one controller lease and adapts to
the registered descriptor, intro, gold, normal EXP, stat, magic EXP,
learned-magic, item, confirmation, fade, lifecycle-exit (`0x800E64A0`), and
cleanup (`0x800E3694`) points. At cleanup it requires lifecycle `0xFF`,
completion flag `1`, null result pointer, game mode `6`, and unchanged RNG.
Its typed receipt is consumed by the enclosing phase. It creates no artifact,
savestate, workflow output, or database aggregate.

Static construction, lowering, schema, reducer, and admission tests are in
scope now. Live, E2E, and test-only Full Phase execution validation of the
Results handler is deferred until a real downstream phase exists. A temporary
Full Phase must not be introduced solely for validation.

## Lifecycle and ownership invariants

- Completion, recording, and replay start only from explicit user requests.
- The selected Victory savestate is planning input to completion; the
  movie-paired Battle-entry checkpoint is replay input to recording.
- Each phase owns one singleton workset and publishes outputs atomically.
- Workers receive exact resolved inputs and never access SavorDb.
- `InputArbiter` is the sole controller publisher during adaptive replay.
- Capture, progress, predicates, derived state, and movie observation do not
  gain controller or foreground-routing authority.
- Battle Record emits no DTM variant and no Battle-specific SeedProbe.
- Battle Replay emits no game-state or movie artifact and exists as a reusable
  production control phase rather than an E2E-only fork.
- The accepted preseed is the Battle recording segment boundary; Results
  handling begins only after the ordinary downstream postseed transition.
