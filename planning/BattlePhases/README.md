# Battle Phases Investigation Notebook

## Purpose

This is the living, evidence-first record for the new `battle.context`,
`battle.single_turn`, `battle.completion`, `battle.record`, and
`battle.replay` phases.
Historical investigation remains in place, while the Decisions and latest log
entries record the approved implementation architecture.

Battle E2E runs validate the static join, entry and Battle Context provenance,
predicate accounting, realized dynamic lineage, and outcome-specific artifact
contracts. They report all BattleSets, waves, turns, candidates, outcomes,
selection states, RNG/timing values, predicate counts, artifacts, and progress
for human review. They do not require any particular turn count, continuation,
selection, Victory, Defeat, or BattleSet status.

We will refine it incrementally:

1. record current and legacy behavior;
2. separate observed facts from hypotheses;
3. list the functionality and boundaries that need investigation;
4. discuss a small number of questions at a time; and
5. record decisions only after they are made together.

Current code and new runtime evidence are authoritative for the new backend.
The checkout at `C:\Users\jahor\.codex\worktrees\e4f9\SAVOR` is a read-only
source of legacy behavior.

## Status

Journal opened: 2026-08-07.

Current phase: backend implementation is present and compiler/runtime
validation is in progress for Battle Context, predicate-enabled Battle Single
Turn, atomic workset initialization, homogeneous workers, and adaptive
synchronized input. Qt authors reusable Battle Plans and launches the public
Battle workflow directly from one exact published plan.

## Public Battle workflow contract

The public workflow unit is `battle`. It references one exact
`authoring.battle_plan`; Battle Chain Specs, Battle Run Specs, and Battle
Explorer/Plan Settings are not part of the current model. The Battle Plan owns
only reusable turn strategy. Each workflow launch owns its execution policy:
the fake-attack minimum and maximum plus one required typed
`continuation_mode` choice.

`manual_selection` stops after each completed wave for an explicit selection.
`automatic_best_per_ending_rng` applies the canonical ranking and continues the
best eligible candidate for each ending RNG. `battle.start` freezes the plan
ID, plan fingerprint, normalized continuation mode, fake-attack range, and
workflow provenance into the BattleSet. Later waves use only that frozen
contract and reject plan drift.

## Normative Battle exit and recording architecture

This section supersedes every earlier Battle-planning statement that models
the Results Screen as a separate Full Phase, workflow node, result aggregate,
or savestate-producing operation. The matching ExecutionRuntime contract is
recorded in
[`../ExecutionRuntime/19-battle-completion-recording-and-results-handler.md`](../ExecutionRuntime/19-battle-completion-recording-and-results-handler.md).

The Battle exit path has three explicit production phases and one reusable
downstream interaction:

1. `battle.completion` advances one explicitly selected durable Victory
   candidate through Battle completion and reward commitment. It ends at the
   accepted field preseed boundary, publishes the durable `.bcmb` completion
   manifest, and publishes a movie-inactive planning savestate.
2. `battle.record` is a separate, explicitly requested commitment operation.
   It reconstructs the exact selected Battle lineage and restores the
   movie-paired Battle-entry checkpoint as a `Savestate` baseline with its
   exact DTM continuation sidecar. It adopts the playback session restored at
   that cursor through `runtime.movie.adopt_restored_read_only_playback`,
   branches it into recording before guest advancement,
   adaptively replays the entire Battle and completion sequence, and ends the
   recorded segment at the same accepted field preseed boundary. It never uses
   the TAS-only opt-in `ReadOnlyMovie` origin baseline.
3. `battle.replay` is the non-recording control phase. It restores the exact
   `BattleSet.entry_savestate_id` as a `Savestate`, with its exact DTM sidecar
   when the entry is movie-paired and with no sidecar when it is movie-inactive.
   The worker branches from `MovieService`'s post-restore observation: it
   adopts and stops `ReadOnlyPlayback`, or proceeds directly from `Inactive`,
   then executes the same canonical replay plan and adaptive body without
   recording or publishing artifacts.
4. The Battle Results Screen is not a phase. It is a static, reusable adaptive
   interaction lowered into a future field, cutscene, or ship-runtime phase.
   That downstream phase performs the ordinary SeedProbe/TBR transition to
   field postseed and then invokes the handler before beginning its own domain
   behavior.

`battle.completion` and `battle.record` share one Battle Completion
interaction. Starting at `EndBattleVictory` (`0x800706D8`), it accepts either
researched causal completion path, reaches reward entry (`0x8006F598`) and
reward commit (`0x8006FD58`), requires the committed state to have
`battleInputState == 2`, and captures
the before/after character and reward evidence used by
`BattleCompletionManifestV1`. Foreground waits remain unbounded by elapsed
time or frame count; cancellation and worker-health policy remain outside the
phase program.

The common terminal is the first accepted field preseed point:

- fast preseed `0x80101894` is accepted only when
  `u32[0x803475D4] != 0`;
- otherwise execution continues to deferred preseed `0x801018AC`.

At that point the worker captures `FieldTransitionContextV1`, including area,
raw and effective suffixes, reconstructed `me%03d%c.sct` filename, RNG, and
execution provenance. Area 99 obtains the effective suffix from
`u8[0x80310A22] + 'a'`. Coordination, not the worker, classifies the filename:

| Filename | Downstream route |
|---|---|
| `me099*` | `OverworldNavigation` |
| other `me000*` through `me199*` | `FieldNavigation` |
| `me200*` through `me499*` | `Cutscene` |
| `me500*` and above | `ShipRuntime` |

`ShipRuntime` is a field/SCPT route. No filename classification may create a
new `battle.*` chain, and there is no Battle-to-Battle transition.

### `battle.completion` contract

- Materialization requires explicit user selection of a durable Victory turn
  job. Merely observing Victory never launches the phase.
- The workset is a singleton with the selected Victory successor savestate,
  exact BattleSet/wave/turn-job/execution-job lineage, empty predicate and
  derived-state bindings, and one maximum attempt.
- Entry qualification requires exact paused PC `0x800706D8`.
- Success publishes a first-class `BATTLE_COMPLETION` `.bcmb` artifact, a
  movie-inactive preseed successor savestate, and one relational
  `BattleCompletion` result referencing both.
- The manifest contains the selected lineage, entry/reward/preseed
  provenance, Battle RNG evidence, before/after character state, rewards,
  expected Results presentation, and field-transition context.
- Unexpected hooks, malformed rewards, or transition inconsistency fail the
  job atomically and publish neither manifest nor successor.

### `battle.record` contract

- Materialization requires a second explicit user request against a successful
  `battle.completion` result.
- Coordination walks the chosen Victory lineage back to turn one and produces
  one source-neutral immutable `BattleReplayPlanV1`: the confirmed first-turn
  SeedProbe frame, ordered
  concrete per-turn commands and fake-attack counts, expected terminal and
  ending RNG per turn, the expected completion manifest, exact lineage, and a
  canonical plan hash. A separate `BattleReplaySourceBindingV1` names the
  exact movie-paired recording checkpoint and inherited DTM/itinerary. Gaps,
  ambiguity, symbolic commands, or changed source evidence fail before dispatch.
- Its singleton workset uses the movie-paired checkpoint as a `Savestate`
  baseline and carries the exact DTM as that state's continuation sidecar.
  `ReadOnlyMovie` is reserved for TAS Movie phases explicitly starting from
  the DTM-declared origin; it is neither a TAS default nor valid for
  `battle.record`.
- `runtime.movie.adopt_restored_read_only_playback` validates and scopes the
  playback session already established by savestate restoration. It performs
  no DTM-origin preparation, core restart, state restore, or guest advancement
  before the recording branch.
- `MovieService` alone classifies the restored session as read-only playback,
  recording, naturally ended playback, or inactive. It reconciles raw native
  facts only while the core is authoritatively paused; running
  `ExecutionEngine` maintenance reads the epoch-qualified cached state. The
  recording branch verifies and commits `Recording` before the first replay
  advance, then receives no physical movie polling. Owned playback uses
  Dolphin's pause-at-movie-end setting, so only a confirmed pause followed by
  genuine source playback exhaustion triggers the existing movie-ended failure
  policy.
- The worker executes one job in one item. Predicate execution is disabled. It
  validates live Battle Context before each turn, reuses the adaptive Battle
  command interaction, verifies each selected terminal/RNG, and then reuses
  the shared completion interaction.
- The observed completion and transition must be semantically equal to the
  standalone completion. Semantic comparison excludes timing, timestamps,
  worker identity, predicate/progress data, and artifact IDs.
- Success captures the preseed SAV at cursor `N`, then records a fresh neutral
  publication until its exact controller poll is observed and the paused
  recording cursor is `M > N`. The finalized DTM contains `M` records and
  extends the item-local `N`-record prefix witness exactly while preserving
  the inherited boot-origin DTM header. The paired SAV is a checkpoint into
  that movie; it does not make the DTM savestate-starting. The worker never
  publishes the short checkpoint-time DTM or a SAV sidecar. Coordination
  stores the SAV and extended DTM independently, links them through one
  authoritative `MoviePaired` StateDB savestate, and publishes the
  inherited-plus-appended TMI itinerary, final-command timing-anchor annotation,
  durable `BattleRecording`, and an unvalidated TAS Movie tree. Existing TAS
  Movie validation is scheduled automatically, and checkpoint sterilization
  is scheduled only after a `Valid` validation result.
- `ReplayMismatch` is a successful domain outcome. It publishes no DTM,
  savestate, itinerary, tree, or partial movie artifact; diagnostics and
  canonical progress remain available.
- The timing anchor identifies the final turn's last player-controlled command
  commitment and exact DTM input index. This phase creates no DTM variants;
  future generic DTM modification may insert neutral input records before the
  anchor.

### `battle.replay` control contract

- Materialization requires an explicit request against the same completed
  `battle.completion` input used by Battle Record.
- Coordination uses the same canonical replay-plan builder. Record and Replay
  therefore persist byte-identical `BattleReplayPlanV1` bytes and hashes for
  the same completion lineage.
- Replay has its own immutable `BattleReplaySourceBindingV1`, whose sole state
  authority is the exact `BattleSet.entry_savestate_id`. Coordination stages
  only that SAV and, when durable evidence marks it movie-paired, its exact
  validated DTM sidecar. It never substitutes the paired ancestor.
- After restoration, `runtime.movie.observe_state` asks `MovieService` for the
  canonical state. `Inactive` qualifies `BeforeRandSeedSet` and enters the
  shared body directly. `ReadOnlyPlayback` adopts the restored session,
  qualifies the same PC, stops playback, and must observe `Inactive` before
  entering the body. Prepared playback, recording, exhausted playback, and
  unknown state fail before input publication or guest advancement.
- An inactive source creates no playback handle or cleanup. A paired source
  releases its playback controller reservation before adaptive input begins.
- SeedProbe delivery, TurnInputs entry, live Battle Context validation,
  adaptive multi-turn commands, terminal/RNG checks, and completion/transition
  semantic comparison are shared with Battle Record.
- `Matched` and `ReplayMismatch` are durable successful domain outcomes.
  Replay persists source/completion lineage, its canonical source binding and
  plan, terminal hash, and mismatch or observed completion evidence. DTM and
  itinerary references are nullable for movie-inactive entry states.
- Replay declares no artifacts and creates no savestate, DTM, itinerary,
  timing anchor, TAS tree, validation request, sterilization request, or
  downstream workflow transition.

### Results Screen handler contract and validation boundary

The handler enters at field postseed `0x801012B4` with the exact completion
manifest. It owns one adaptive controller lease and follows the researched
descriptor, intro, gold, normal EXP, stat, magic EXP, learned-magic, item,
confirmation, fade, lifecycle-exit (`0x800E64A0`), and cleanup
(`0x800E3694`) gates. At cleanup it requires lifecycle `0xFF`, completion flag
`1`, a null result pointer, game mode `6`, and RNG unchanged from handler
entry. It produces only a typed receipt for its enclosing downstream phase;
it publishes no standalone artifact, savestate, workflow result, or database
aggregate.

Static construction, lowering, codec, and reducer tests remain appropriate.
Live, E2E, and test-only Full Phase execution validation of the Results Screen
handler is explicitly deferred until a real downstream phase exists. We will
not invent a temporary production-shaped phase merely to run it.

The global controller-input contract is normative in
[`../ExecutionRuntime/16-adaptive-synchronized-input.md`](../ExecutionRuntime/16-adaptive-synchronized-input.md).
Battle command entry never materializes or executes a raw input tape.

The global semantic-routing, workset-capture, and canonical-progress contract
is normative in
[`../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md`](../ExecutionRuntime/17-semantic-routing-workset-capture-canonical-progress.md).
It supersedes this notebook's earlier statements that arbitrary capture was
deferred or phase-owned. Battle phases use the same immutable workset-level
capture binding as every other program kind; the worker creates and finalizes a
fresh session for each item.

The typed derived-state runtime is normative in
[`../ExecutionRuntime/18-derived-state-runtime.md`](../ExecutionRuntime/18-derived-state-runtime.md).
`battle.single_turn` selects `soa.derived.battle.core/1` once per workset and
creates fresh item-local snapshots for turn entry, turn order, and rewards.
At native semantic triggers, bounded raw Battle evidence is captured
synchronously on Dolphin's CPU thread before the game leaves the breakpoint;
the core is not paused for a successful passive observation. Parsing,
aggregation, validation, generation assignment, and atomic typed publication
occur later on the worker actor. A later-turn item already paused at
`TurnInputs` uses the one explicit retained-current-point initialization path;
no native hit may fall back to delayed actor-side reads. These snapshots are
never sourced from capture and never survive item unwind. The block is
available to Full Phase programs and predicates through its exact typed query
actions and pure reducers. It does not create a general derived artifact.

`battle.single_turn` automatically requests the registered Battle-event and
predicate-evaluation progress libraries unless planning explicitly disables
them. The Battle event library preserves the researched typed observations for
attack damage, counterattack, death, item drop, and the twelve-slot
instruction/turn state. These are passive observations and cannot wake or
redirect Battle execution. `battle.context` declares an empty default progress
set. Battle programs use shared `SPS1` semantic point sets, while
`ExecutionEngine` owns every foreground wait directly.

The current discussion has established the broad phase lifecycle, confirmed
`TurnInputs` as the per-turn continuation boundary, and assigned exactly one
wave to each `battle.single_turn` workflow step.

Predicate redesign is tracked separately in
[`PredicateSystem.md`](PredicateSystem.md) so reusable predicate architecture
does not become coupled to the Battle phase modules.

The worker-fleet boundary is defined by
[`../ExecutionRuntime/14-homogeneous-worker-architecture.md`](../ExecutionRuntime/14-homogeneous-worker-architecture.md).
Battle worksets carry their exact Battle/predicate package and common input;
workers never advertise Battle support or query SavorDb. Headless and Visual
workers are interchangeable. VisualDebug is reserved for explicit replay and
is not part of normal wave scheduling.

## Evidence reviewed so far

### Current checkout

- The completed Full Phase examples are SeedProbe and TAS Movie validation.
  They use typed worker requests/results, immutable module definitions, and
  SavorDb-owned materialization, result persistence, and continuation.
- The production Full Phase registry does not yet register Battle Context or
  Battle Single Turn.
- The uniform ProgramRuntime capability-pack registry already contains a typed
  `soa.battle.BattleContext`, battle semantic points, a coherent Battle Context
  capture action, and a Battle turn-input materialization reducer.
- Predicate, semantic-observation, and interaction composition infrastructure
  exists, but its exact suitability for the legacy Battle Single Turn behavior
  has not yet been established.
- Navigation Context provides a current-backend precedent for context storage:
  it writes a codec-specific context file, registers it as a State artifact by
  hash, size, extension, and filename, and publishes the artifact ID as the
  workflow result and output reference.
- A current `ProgramKindDescriptor` owns the program result handler and workflow
  transition adapter alongside materialization and workset reconstruction. This
  is the coordination seam selected for the Battle turn-limit policy.
- Current workflow gating rejects mixed successful/failed transitions when
  `allow_mixed_success_failed_transition` is false. The new Battle requirement
  is stronger: command-entry failure stops the active workset immediately
  rather than merely preventing its later transition.
- The current SeedProbe program has an explicit Confirm stage that repeats a
  provisional candidate frame and only completes with confirmed evidence. This
  is the determinism authority for Battle's initial inputs; Battle does not need
  a second RNG confirmation step.
- Legacy Battle adapters and PhaseScript implementations remain present in the
  current checkout as migration evidence. They are not the desired new module
  architecture.

### Read-only legacy checkout

Observed legacy `BattleContext` behavior:

- restores the assigned state;
- recognizes when execution is already stopped at `TurnInputs`;
- otherwise advances to `TurnInputs`;
- captures the full Battle Context; and
- returns that context for durable persistence and first-wave creation.

Observed legacy `BattleSingleTurn` behavior:

- optionally overrides and verifies the starting RNG seed;
- applies the initial SeedProbe input on the first turn;
- reaches the turn-input boundary;
- materializes and executes the battle-command macro;
- uses adaptive breakpoint and memory-gate behavior during command entry;
- confirms that the turn was accepted;
- retries the input path once for selected failures;
- evaluates configured predicates and capture watchpoints;
- runs to the next turn, victory, defeat, predicate rejection, turn limit, or
  execution failure;
- records ending RNG and detailed macro/predicate evidence;
- returns the next-turn Battle Context when applicable; and
- saves successor states for next-turn and victory outcomes.

Observed legacy SeedProbe-to-Battle usage:

- the TAS/entry savestate is supplied independently to SeedProbe and Battle;
- SeedProbe applies candidate controller frames and observes the RNG at
  `AfterRandSeedSet` (`0x8000A1DC`), producing unique input frames;
- the Battle chain receives both the unchanged entry savestate and that input
  frame set;
- Battle Context runs once from the unchanged entry savestate;
- coordination creates one first-turn seed candidate and wave for each input
  frame; and
- every first-turn Battle Single Turn job restores the unchanged entry
  savestate, reapplies its candidate SeedProbe frame, and advances through the
  prelude to `TurnInputs`. Later turns use their parent job's successor state
  and do not apply the initial frame.

Observed legacy coordination behavior outside the worker phase:

- resolves authored battle plans and concrete target variants;
- expands fake-attack variants within the remaining budget;
- persists per-candidate Battle turn jobs;
- groups surviving results by ending RNG;
- prefers more predicates passed, then fewer fake attacks, when resolving
  duplicate RNG outcomes;
- prioritizes victory survivors;
- creates subsequent waves; and
- terminates on victory, no survivors, disabled automatic advancement, or no
  authored next turn.

Legacy ranking evidence needs one qualification:

- the inspected DB-backed selector deduplicates by ending RNG, preferring more
  predicates passed and then fewer cumulative fake attacks;
- Battle turn results persist `vi_start`, `vi_end`, and derived `delta_vi`, but
  that inspected selector does not use `delta_vi`; and
- frame count has nevertheless been identified as required behavior for the
  new selector. The required within-RNG ordering is fewer cumulative fake
  attacks, fewer `delta_vi` frames, more predicates passed (`pred_passed`),
  then stable job ID.

Observed legacy turn-limit and Victory behavior:

- the legacy authored Battle Plan stored both `num_turns` and concrete authored
  turn rows; the new backend removes the redundant count and uses only rows;
- the worker received `max_turn` from that plan, while coordination stopped
  fan-out when `FindTurn(plan, current_turn + 1)` found no authored next turn;
- the meaningful legacy turn limit is therefore the end of the authored Battle
  Plan, not a count of generated waves; and
- Victory is detected at `EndBattleVictory` (`0x800706D8`). The script reads the
  ending RNG and saves the successor state immediately at that point, before
  results-screen handling.

Observed legacy Battle Single Turn result surface:

- domain/timing facts: battle outcome, turn timing (`vi_start`, `vi_end`, and
  persisted `delta_vi`), and ending RNG;
- selection facts: fake attacks used and predicate passed/total/abort values;
- command-entry diagnostics: macro failure code, step count, last step index,
  expected breakpoint, actual breakpoint, and actual PC;
- now-obsolete direct-override diagnostics: override enable/seed,
  original/applied seed, and memory-write/readback status; and
- artifact references: applied input, input trace, optional capture, successor
  savestate, and next-turn Battle Context.

Observed legacy Battle Context storage behavior:

- `BattleContextCodec` declares version `1` and the `.bctx` extension;
- start-of-battle context probes persisted the encoded blob and codec version
  in Analysis Battle records;
- continuing Battle Single Turn results persisted a base64-encoded context blob
  and codec version in their Analysis Battle turn-job records; and
- no legacy production path that publishes those Battle Contexts as standalone
  `.bctx` artifact files has yet been found. The extension existed, but the
  workflow persistence found so far was database-backed.

## Things the new phases need us to investigate

### `battle.context`

- Exact battle-entry qualification and rejection evidence for states already
  in the middle of a battle.
- Whether `TurnInputs` is the single fixed capture boundary.
- Which Battle Context fields are required by later planning and execution.
- Whether the existing typed Battle Context schema is the intended durable
  contract or only a useful starting point.
- How the durable `.bctx` blob is identified, associated with its producing
  result, exposed to users, and deleted on explicit user request.
- Required stop, state-lineage, and capture provenance.
- Failure behavior for wrong stops, runtime failure, and partial memory reads.
- Its handoff to `battle.single_turn`.
- Its context-only output contract: a `.bctx` artifact with no output savestate.
- Which legacy bootstrap responsibilities belong to the phase descriptor or
  coordinator rather than the worker module.

### `battle.single_turn`

- The smallest factual request that still executes one concrete candidate.
- First-turn versus later-turn prelude behavior.
- Starting RNG override semantics and required mutation/readback evidence.
- Initial SeedProbe input semantics.
- Concrete turn-command and target representation.
- Full adaptive command-macro behavior, including fake-attack patterns and
  memory gates.
- Turn-acceptance confirmation and failure evidence, with no internal retry.
- Predicate inputs, observation points, baselines, comparisons, abort policy,
  and passed/total accounting, supplied by a reusable predicate subsystem that
  is not coupled to Battle.
- Optional capture-profile behavior and capture artifact publication, deferred
  to a separate later design discussion.
- Outcome taxonomy and the boundary between domain rejection and runtime
  failure.
- Ending RNG, timing, semantic command diagnostics, and stop evidence.
- Returning the complete Battle Context for every continuing candidate that
  reaches the next `TurnInputs` boundary.
- The exact context capture and successor-state ordering at `TurnInputs`.
- Which outcomes publish a successor savestate.
- Cancellation and cleanup at every interaction boundary.

### Coordination surrounding both phases

- Where seed candidates, target variants, and fake-attack variants are
  expanded.
- How a completed one-wave step exposes its eligible `TurnInputs` and Victory
  results for either automatic selection or later user selection.
- How survivor selection and deterministic tie-breaking should be retained.
- How enemy identity/count and alive/dead composition plus total raw party HP
  are exposed as planning information without affecting automatic selection.
- How victory, no-survivor, turn-limit, and manually stopped searches are
  represented durably.
- How the descriptor-owned turn limit stops automatic coordination while
  preserving completed candidates for user action.
- Workset grouping rules for candidates that share a source state.
- Idempotent result persistence and restart recovery.
- How a failed, stopped workset is retried after a command-entry bug is fixed
  without hiding the original error.
- Whether existing Analysis Battle records remain the target projection or
  need a separately approved change.

## Separate investigation tracks

- Predicate redesign is an active, reusable-system investigation. Battle will
  consume it but must not own it.
- Arbitrary capture-profile support is deferred to a separate later discussion
  and is not required for the initial Battle modules.

## Question backlog

These are investigation prompts, not decisions. We will take them in small
batches.

The next active questions are maintained in the predicate-system investigation.
Battle-specific questions will be added here as new evidence exposes them.

## Decisions

### 2026-08-07 - Phase lifecycle and per-turn context

- `battle.context` is a workflow step run only at the start of a battle.
- `battle.context` rejects a source state already in the middle of a battle.
- `TurnInputs`, including the first `TurnInputs`, is always a rejected source
  boundary for `battle.context`; the phase must never be launched there.
- `battle.context` is context-only: it publishes the `.bctx` artifact and does
  not publish a successor savestate.
- First-turn `battle.single_turn` jobs retain the original battle-entry
  savestate and independently replay the prelude using their SeedProbe-derived
  initial input. This varies the starting RNG without a Battle Context output
  savestate.
- `battle.context` and SeedProbe may run in parallel from that shared entry
  savestate. A first-turn `battle.single_turn` step joins the completed Battle
  Context result lineage, the entry savestate, and one selected confirmed
  SeedProbe input. The `.bctx` file itself is planning data rather than a
  required execution input.
- Each selected confirmed SeedProbe input creates one first-turn wave.
- Battle does not repeat SeedProbe's RNG determinism check; SeedProbe's Confirm
  phase is authoritative.
- SeedProbe-derived input is the only supported starting-seed mechanism. Direct
  starting-RNG override is removed from Battle.
- One `battle.single_turn` workflow step represents exactly one wave. Later
  waves are separate workflow steps.
- The existing `auto_wave_trigger_enable` run setting is retained.
- When automatic wave triggering is disabled, no subsequent wave is created
  until the user requests one from user-selected jobs that reached
  `TurnInputs`.
- Each manually selected parent job creates its own downstream wave and
  `battle.single_turn` workflow step.
- `battle.single_turn` returns the complete Battle Context when a continuing
  result reaches the next turn's `TurnInputs` boundary.
- Coordination may use those per-turn contexts to prune candidate endings
  before downstream waves are queued.
- Initial and per-turn Battle Context blobs are retained durably. Deletion is an
  explicit user decision rather than an automatic lifecycle policy.
- Battle Contexts are first-class, file-backed artifacts using the `.bctx`
  extension and a dedicated `BATTLE_CONTEXT` artifact kind.
- Deleting a `.bctx` artifact does not invalidate its producing result or make
  downstream actions unavailable. It removes further-turn guidance for that
  result from the planning UI. Downstream execution proceeds without the prior
  artifact and does not regenerate it. `battle.single_turn` validates its plan
  against the live Battle state restored for that candidate.
- Enemy identity/count, alive/dead state, and total raw party HP are presented
  to users as planning information. The narrow exception is that the next
  authored turn's concrete targets may use a successor `.bctx` to exclude a
  candidate from automatic continuation when a target is known absent,
  non-enemy, or dead. Missing or invalid context remains eligible, manual
  continuation is never filtered, and live worker validation remains
  authoritative.
- Automatic selection waits for the complete wave to finish.
- Ending-RNG deduplication is retained. Within each ending-RNG bucket, candidates
  rank by fewer cumulative fake attacks, then fewer frames, then more predicates
  passed (`pred_passed`), then stable job ID.
- Frame count for this ranking is `delta_vi = vi_end - vi_start`.
- Every selected job becomes a separate downstream wave.
- A Victory result stops automatic wave triggering even if the completed wave
  also contains `TurnInputs` results. Those results remain available for manual
  use.
- `.bctx` artifacts are produced only for results that reach `TurnInputs`, not
  for Victory results.
- `battle.single_turn` performs no internal retry. Command-entry failure fails
  the job so it remains visible as a canary for improving command entry.
- Workflow steps default to `max_attempts = 1`, but an explicit later retry of
  the stopped workset remains available after the underlying bug is fixed.
- A command-entry failure stops the active workset, reports the error to the
  user, and prevents execution from continuing with its other candidates. The
  workset remains eligible for later retry.
- Explicit retry reruns every candidate in the stopped workset, including items
  that completed before the failure.
- `HitTurnLimit` is not a worker outcome. Turn limits are coordination policy
  owned by the `battle.single_turn` Program Kind Descriptor.
- The descriptor derives that limit solely from positive, one-based,
  contiguous authored turn rows and stops fan-out when the exact next row does
  not exist. No derived turn count is stored.
- The worker reports fake attacks used by its current candidate. Coordination
  calculates and retains the cumulative fake-attack value across turns.
- `ReachedNextTurn`, Victory, Defeat, and predicate rejection are successful
  typed domain results. Invalid plan/materialization, runtime error, timeout,
  and command-entry failure fail the job.
- Plan inconsistency is an invalid-plan failure. Examples include targeting an
  enemy that is not alive or requesting an unavailable item. This validation is
  performed against the candidate's restored live Battle state; a prior
  `.bctx` planning artifact is not execution authority.
- Completed Battle results retain outcome, executed turn index, ending RNG,
  `vi_start`, `vi_end`, `delta_vi`, fake attacks used this turn, predicate
  passed/total counts, and terminal stop identity.
- Command-entry failures retain the legacy macro failure code, step count, last
  step index, expected breakpoint, actual breakpoint, and actual PC.
- Direct-RNG-override and memory-write/readback fields are removed from the
  Battle contract. During implementation, associated fixture changes must be
  limited to Battle-specific fixtures.
- Applied-input and input-trace artifacts are not emitted by the new Battle
  path, including on failure. Failures retain semantic command diagnostics and
  typed terminal evidence. Historical artifact rows remain readable.
- Battle command entry is adaptive. It captures a fresh live Battle Context at
  each `TurnInputs`, validates the per-job plan before input, and selects each
  next verifier-known command segment from live state.
- A fake attack reaches the attack target-selection semantic point, observes
  the bounded RNG change, advances exactly seven synchronized neutral frames,
  then publishes B and waits for the command-input-ready point.
- `UseItem` remains representable and inventory-validatable. A valid available
  item request fails explicitly as unsupported until its interaction is
  implemented.
- Predicate evaluation will be redesigned as a lean, flexible, reusable system
  outside the Battle modules. Battle is one consumer rather than its owner.
- Predicate Definitions, Predicate Execution Bindings, and Predicate Groups are
  distinct first-class authored objects. Published revisions are immutable.
- Their opaque stable keys are generated by the Authoring database and survive
  revisions. Names and descriptions are mutable parent metadata and never
  affect semantic fingerprints, runtime revision hashes, Battle Plans, or
  prepared packages.
- Create and Duplicate operations use idempotency request keys. Exact retries
  return the original identity; only explicit Duplicate creates a second
  logical identity with equivalent semantics.
- A Battle Plan turn may name one published Predicate Group revision. Concrete
  parameter values and witness sources belong to Execution Bindings, never to
  the plan, wave, or workflow arguments.
- Every candidate in one wave/workset uses one exact Predicate Execution
  Package so its results remain comparable and the workset can reuse the same
  prepared program variant.
- Each group member combines one published Execution Binding with a sorted
  nonempty hook set, a combined-stream occurrence policy, evidence behavior,
  reaction, and aggregation participation. A multi-hook member is one atomic
  predicate and has no separate per-hook-use identity.
- Named baselines are candidate-local typed observation state with explicit
  capture hooks and `First` or `Latest` update behavior.
- Common legacy comparisons use a simple editor, while an advanced editor may
  compose typed comparisons, Boolean logic, modest arithmetic, and exact pure
  reducers. Arbitrary user scripts are outside the initial design.
- Predicate Groups are resolved against versioned semantic hooks and verified
  once per exact phase, hook contract, group, and capability-dependency key.
  They are not lowered separately for each candidate job.
- Coordination materializes a self-contained workset package containing the
  canonical Predicate Execution Package as shared input and the exact prepared
  program definition. Workers do not resolve database identifiers or query any
  authoring, analysis, or execution database.
- The generalized workset model has explicit common inputs and per-job inputs.
  Common inputs contain the phase-wide Predicate Execution Package and
  dependency identity once; item inputs contain the job's Battle Plan,
  candidate-specific state, SeedProbe input, fake-attack choice, and correlation.
  Existing new-backend phases use the same shape with empty or phase-specific
  common inputs.
- Workers validate and execute the supplied package and return typed results and
  staged artifact manifests. Coordination owns artifact registration and all
  result persistence.
- Arbitrary capture-profile artifacts are deferred and are not part of the
  initial Battle module slice.
- Successor savestates are published for `ReachedNextTurn` and Victory.
- The Victory savestate is captured immediately at `EndBattleVictory`
  (`0x800706D8`), before results-screen handling.
- Defeat and predicate rejection publish no successor savestate.
- Automatic selection does not create a durable `pruned` candidate class. Every
  candidate that reaches `TurnInputs` or Victory remains available for later
  user selection, and selection rules or rejection reasons do not need to be
  persisted.
- Other pruning conditions are expressed through predicates and early aborting
  rather than being added as default coordinator policy.
- The continuing terminal is `TurnInputs` at `0x80071740`. This matches the
  legacy Battle Single Turn path, which saves the successor state and captures
  the next-turn Battle Context after reaching that breakpoint.

## Investigation log

### 2026-08-07 - Initial orientation

The legacy responsibilities were inventoried at a high level. The initial
questions concerned the phase lifecycle and one-wave versus multi-wave
ownership for `battle.single_turn`.

### 2026-08-07 - Lifecycle refinement

The phase-lifecycle question was resolved: Battle Context runs once at battle
entry, while Battle Single Turn supplies complete ending contexts at
`TurnInputs` for pre-wave pruning.

### 2026-08-07 - Wave ownership, retention, and initial pruning

Each Battle Single Turn step owns one wave, with later waves represented by
separate workflow steps and not forced to advance automatically. All produced
Battle Context blobs are durable until the user elects to delete them. Enemy
composition and party HP were initially considered as optional selection
dimensions, but that idea was later withdrawn.

### 2026-08-17 - Typed workflow-owned continuation

Continuation is no longer stored in an authored settings wrapper. Every
`battle` workflow launch must choose `manual_selection` or
`automatic_best_per_ending_rng`. Manual selection creates a later wave only
after the user selects `TurnInputs` jobs. Automatic selection waits for the
entire wave before applying its ranking. Selection is ephemeral coordination
behavior, not a stored pruning judgment: all
`TurnInputs` and Victory candidates remain available. Battle Contexts are
first-class `.bctx` file artifacts; deleting one removes planning guidance but
does not invalidate its result or downstream actions.

### 2026-08-07 - Deduplication and downstream-wave fan-out

Each selected parent job creates one downstream wave. Ending-RNG deduplication
is retained. Its comparator is cumulative fake attacks ascending, `delta_vi`
ascending, `pred_passed` descending, then stable job ID. Victory stops
automatic triggering. Only `TurnInputs` results produce `.bctx` artifacts;
deleted contexts are not recaptured and do not block downstream execution.

### 2026-08-07 - Composition and HP revised to information only

Enemy identity/count, alive/dead state, and total raw party HP remain available
to the planning UI, but they have no role in automatic advancement. The
composition/HP selection controls and associated job-limit concept are no
longer planned.

### 2026-08-07 - Context-only entry phase and failure visibility

Battle Context accepts only a true battle-entry source and produces only a
`.bctx` artifact. Battle Single Turn has no hidden retry: command-entry errors
fail their jobs as visible canaries. `HitTurnLimit` moves out of the worker and
into Program Kind Descriptor coordination policy. The worker reports only its
current fake-attack use; coordination calculates the cumulative count. Domain
outcomes remain completed results, runtime/materialization/command failures are
failed jobs, and successor savestates are retained for `TurnInputs` and Victory.

### 2026-08-07 - SeedProbe lineage, authored turn limits, and fail-fast worksets

The legacy SeedProbe Battle chain establishes the intended first-turn lineage:
Battle Context and SeedProbe share the unchanged entry state, while each first
turn candidate restores that state and reapplies one SeedProbe-derived frame to
vary the starting RNG. A first-`TurnInputs` source is invalid for Battle Context.
The authored Battle Plan defines the turn limit; descriptor coordination stops
when no next authored turn exists. Victory state is saved at
`EndBattleVictory` (`0x800706D8`). Command-entry failure stops and exposes the
whole active workset, which may be explicitly retried after the bug is fixed;
the default workflow attempt count remains one. Defeat and predicate rejection
do not publish successor savestates.

### 2026-08-07 - Parallel context capture and confirmed SeedProbe join

Battle Context and SeedProbe run independently from the same entry savestate.
Each first-turn wave joins the completed Battle Context result lineage with one
selected confirmed SeedProbe input and the original state. The `.bctx` artifact
is a planning aid, not a required `battle.single_turn` input. The SeedProbe
Confirm stage is the sole RNG-determinism authority, and direct starting-RNG
override is removed.
When a failed workset is explicitly retried after a fix, all of its candidates
run again.

### 2026-08-07 - Result contract and cross-cutting deferrals

The typed Battle result retains operational outcome, timing, RNG, fake-attack,
predicate-count, and stop evidence. Command-entry failures additionally retain
the semantic macro diagnostic tuple. Direct RNG override evidence is removed,
with implementation changes restricted to Battle-specific fixtures. No new
Battle result writes raw applied-input or input-trace artifacts. Predicate
redesign becomes a separate reusable-system investigation; arbitrary capture
profiles are deferred to another later discussion.

### 2026-08-09 - Adaptive synchronized input hard cut

All executable input-plan, sequence, pulse, cursor, direct-pad, and legacy
PhaseScript input-opcode surfaces are removed. One interaction-wide lease owns
state rather than a "held neutral" command: applying neutral implicitly
releases a hold, stable neutral publishes nothing, and guest advancement
consumes an ephemeral execution binding instead of a phase-managed poll or
neutral witness. SeedProbe uses one-shot delivery and retains one typed
delivery receipt; first-turn Battle shares that lowering and discards the
receipt after validation. Battle Single Turn uses a fresh live context,
per-job plan validation, adaptive command state, and the exact fake-attack
ordering: A remains held through target readiness and bounded RNG change,
then neutral release is observed across exactly seven frames before B. Raw
Battle input artifacts are no longer part of the result contract;
`battle.record` is the explicit production phase that commits the selected
adaptive lineage to the DTM.

### 2026-08-10 - Coherent context capture uses paused-state authority

Battle and Navigation context queries no longer require a routed stop
sequence. Their request contract carries the workset epoch and expected PC;
the action host requires the active epoch, a backend-confirmed authoritative
paused execution snapshot, and an exact PC match. This supports both a first
turn that reaches `TurnInputs` through a foreground wait and a later turn that
restores already paused at `TurnInputs`. Routed sequences remain on routed
receipts and canonical capture/progress provenance, but are not fabricated or
used as permission to read current paused guest state.

### 2026-08-11 - Battle enum identity is explicitly keyed

Battle action, turn type, Single Turn outcome, command reducer, and adaptive
segment definitions bind their enum key directly to their canonical metadata.
The same local Battle maps generate closed IR enum members and perform exact
numeric decoding. No Battle behavior depends on parallel enum ordinals or on a
definition's position in a vector; unknown numeric values fail decoding.

### 2026-08-11 - Relational turns and BattleSet completion

Authored Battle Plan turn rows are the sole authority for plan length and
content. `battle.start` rejects empty, zero-based, negative, duplicate, or
gapped turn sequences before creating a BattleSet. Coordination ends a branch
with `PlanComplete` when a successful next-turn candidate has no exact authored
successor row.

After an entire wave completes, automatic continuation resolves the next turn
and classifies each `ReachedNextTurn` candidate from its optional `.bctx`.
Known-unavailable targets are removed before ending-RNG deduplication. Missing,
deleted, corrupt, or undecodable context is `Unknown`, remains eligible, and
produces a coordination diagnostic. A fully filtered wave becomes
`AwaitingSelection`, as does a continuing wave when automatic triggering is
disabled; explicit manual selection remains available and completes the parent
after creating its children.

BattleSet status is aggregated across leaf branches. Victory is dominant and
absorbing, active or awaiting branches keep the set active, any terminal
`PlanComplete` leaf makes an otherwise terminal set completed, and only an
entirely exhausted population becomes `NoSurvivors`. Candidate successor
savestates remain durable Battle result lineage but are not generic graph
outputs. The `battle` graph node publishes one `battle_set` output
(`analysis_battle.battle_set`) referring to the same BattleSet materialized by
`battle.start` once the aggregate becomes Victory, Completed, or NoSurvivors.

### 2026-08-07 - Predicate package preparation, wave assignment, and worker isolation

Predicate Definitions, Execution Bindings, and Predicate Groups are separate
immutable authored revisions. Battle turns supply one optional published Group
default. Before dispatch, coordination resolves the Group through its exact
Bindings and Definitions, verifies the stable phase hooks and dependency
closure, and builds one canonical Predicate Execution Package shared by every
compatible job. Workers never query SavorDb; they execute the supplied package
and return DB-independent typed results and staged artifacts for coordination
to persist.

### 2026-08-07 - Predicate authoring flexibility and wave bindings

Typed flexibility is expressed through semantic hooks, typed sources, exact
registered queries, pinned reads, concrete values, named baselines, pure
expressions, occurrence policies, reactions, and aggregation participation.
Concrete values and all witness sources are immutable Execution Binding
content. The authoring surface provides a typed expression editor, source
editor, and ordered Group editor without supporting arbitrary user scripts.

### 2026-08-07 - Workset shape, planning-only contexts, and clean new model

Worksets are intentionally refactored into common inputs plus per-job inputs.
The Predicate Execution Package, prepared phase definition, and exact
dependencies are carried once per workset. Each job carries its own Battle Plan
as a per-job input, along with
its selected SeedProbe input, fake-attack choice, execution identity, and
correlation. This is a normal new-backend evolution, not a legacy compatibility
constraint. Existing new-backend phases are adapted to the generalized shape;
obsolete wire or legacy-phase compatibility is not a design requirement.
Workers remain database-independent.

Battle Context files use the dedicated `BATTLE_CONTEXT` artifact kind and are
planning aids only. `battle.single_turn` does not require a prior `.bctx`; it
validates the authored plan against the restored live Battle state and fails an
inconsistent plan, including dead targets and unavailable items. It publishes a
new `.bctx` only when the candidate reaches the next `TurnInputs`.

Because the Battle Plan is per-job input, an inconsistency in that plan fails
only that job. It does not trigger the command-entry policy that stops the whole
workset. Workset-wide fail-fast remains specific to command-entry failures and
other explicitly defined workset-fatal conditions.

The ending-RNG comparator's predicate key is the count of predicates passed,
matching legacy `pred_passed`, rather than the number evaluated. Predicate
accounting counts every executed `Passed` or `Failed` check in total and only
`Passed` checks in passed; untriggered checks and checks skipped after rejection
do not count. Missing required evidence fails execution. Per-member
detail is durable for predicate rejection, execution failure, or explicit
evidence emission; ordinary successful results retain Group and package
identity plus the summary.

Prepared predicate variants are reconstructible. The first implementation may
reuse them in memory by structural key but does not require a durable prepared
program cache. The new predicate schema is a clean model with no legacy import
or compatibility requirement.

`StartTurn` and `StartAction` must be backed by genuinely distinct physical
Battle hooks. The current and legacy registries alias both names to
`0x800715dc`; that alias is evidence to correct, not a contract to preserve.

### 2026-08-12 - Symbolic target materialization

Battle Plan target selectors remain authoring concepts. Coordination compiles
each authored turn into deterministic concrete command variants before it
creates worker jobs. `SingleEnemy` contributes one slot, `MultipleEnemies` and
`AnyEnemy` contribute ordered alternative slots, and `SameAsOtherPC` reuses an
already resolved actor target without adding a Cartesian-product dimension.
References may be forward or transitive, but every chain must terminate at a
direct selector and cycles are rejected. Actor slots and action ordinals are
validated exactly.

Variant identity is the SHA-256 of the encoded command bytes. Duplicate command
sets collapse to one variant, while a hash collision between different command
bytes is an integrity failure. Each wave materializes the product of concrete
target variants and permitted fake-attack counts as separate jobs in one
workset. Workers receive only concrete commands and never resolve authoring or
access SavorDb.

Battle Context is optional narrowing evidence. A valid first-turn joined
context or later-turn candidate `.bctx` removes known absent, dead, and
non-enemy targets. Missing or invalid context retains the structural domain and
emits a diagnostic. Automatic continuation uses the same availability result
before ending-RNG deduplication; explicit manual continuation still bypasses
that filter. If narrowing would leave an already-created first or manual wave
empty, structural variants are retained so live worker validation remains the
execution authority.

The old `target_expr_ini` column and API are removed. There is no expression
adapter or legacy target parser in the new backend.

### 2026-08-12 - Legacy-analogous exploratory fixture

The standard Battle E2E fixture authors two generic turns. Actor 0 attacks
`AnyEnemy`; actor 1 attacks `SameAsOtherPC` referencing actor 0. Both turns use
the same ordinary published Predicate Group. The fixture exposes SeedProbe
bounds, samples, combination attempts, sampler tries, and the cumulative
fake-attack range as explicit scenario inputs. These settings broaden an
analogous workflow; they do not target a known frame, RNG, target, command, or
winning branch.

E2E validates concrete-job identities, predicate bindings, evidence shapes,
and artifact contracts, then reports the complete trajectory. It does not
require a target, turn count, continuation path, BattleSet status, or Victory;
trajectory comparison with legacy runs remains a human judgment.
