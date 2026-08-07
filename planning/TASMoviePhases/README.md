# TAS Movie Phase Family

## Status and authority

Living design document. Initial version: 2026-08-02.

This folder defines the intended purpose and behavior of the TAS Movie phase family. The complete-movie
validation worker foundation and its production persistence/descriptor integration are now implemented
in the refactored backend. Round 1 through Round 3, live Dolphin validation, and desynchronization diagnosis
remain deferred. Older planning documents remain research evidence; current code and this living document
define the active contract.

Current code, current database migrations, Dolphin behavior, new runtime evidence, and decisions recorded
here are authoritative. This document should change as the design is clarified and as live validation
reveals incorrect assumptions. There is no backwards-compatibility requirement.

The checkout at `C:\Users\jahor\.codex\worktrees\e4f9\SAVOR` is the designated legacy comparison
repository for this phase family. Its contents are read-only reference evidence: investigation may inspect
and compare that checkout, but must not edit, format, build-generated-write into, or otherwise mutate it.
Legacy behavior may inform current decisions, but it does not define the current architecture or contract.

The working name **TAS Movie phase family** is provisional. Recording, frame indexing/reduction,
timing-variant generation, and validation are separate program kinds within that family.

Current implementation discussions:

- [Complete-movie validation discussion draft](CompleteMovieValidationImplementationPlan.md) for the existing
  `PK_TasMovie` program kind.

## SAVOR purpose

SAVOR exists to help construct an optimal tool-assisted speedrun of *Skies of Arcadia Legends* (US).
The TAS Movie phase family owns establishment and reproducible continuation of the controller-input movie
that realizes the selected route.

It is not merely a facility for playing a DTM. It must:

- extend an established route from one checkpoint to the next;
- record a complete, reproducible DTM history;
- relate player-visible frames to Dolphin's controller-poll input stream;
- annotate meaningful dialog, choice, SeedProbe, and phase-boundary events;
- generate timing variants by replaying and re-recording selected delays;
- preserve database lineage between source and child recordings and checkpoints; and
- occasionally replay the complete movie from boot to validate that accumulated timing still reproduces.

## Foundational terminology

### Genesis movie

The root DTM is the handcrafted movie that begins at game boot. It is not a movie whose DTM header starts
from a savestate. It is the genesis reproduction artifact from which later checkpoints and movie branches
descend.

### Checkpoint

A recording checkpoint is a Dolphin savestate together with the complete DTM sidecar that belongs beside
that savestate. The savestate contains the live Dolphin movie cursor; the adjacent DTM contains the
complete known movie input stream. Loading the savestate therefore restores a cursor into that full movie.

For generated checkpoints, the savestate is captured first at the terminal breakpoint. Recording then
holds neutral and advances one presented frame at a time until the gamepad-override path observes at least
one controller poll. The finalized DTM is paired back with the earlier savestate. Consequently the saved
cursor intentionally has a neutral guard input after it in the DTM rather than sitting at movie end.

This checkpoint-first, neutral-until-polled, finalize-and-pair sequence is the uniform endpoint contract
for every program that produces a complete route DTM, including Round 1, Round 3, and the dedicated
recording replay program kinds owned by battle, navigation, ship battle, and later domain phase families.
After capturing the checkpoint, the producer disables progression services such as the dialog advancer,
disarms the segment breakpoint catalog, and gives the neutral gamepad override exclusive input ownership.
It then advances exactly one presented/player frame at a time until a poll is observed. No other input
source or semantic handler is active during this guard production.

The conceptual recording artifact is:

`recording-checkpoint savestate + same-name full DTM sidecar + persisted lineage`

Recording checkpoints are acceleration cursors and branch anchors. The complete DTM remains the
reproduction artifact. An exploration checkpoint is the specialized state-only form defined below.
Internal checkpoints of either applicable form may also be created when a user decision or other
intervention requires one.

Two checkpoint forms are relevant at boundaries that lead into exploratory work:

- **Recording checkpoint:** saved while Dolphin movie recording is still active. It preserves the movie
  cursor and complete DTM sidecar and is the source used later to commit a selected path into the movie.
- **Exploration checkpoint:** a savestate saved with movie recording inactive and with no DTM sidecar. It
  is a disposable/bootstrap baseline used while battle, navigation, ship-battle, or another exploratory
  domain phase enumerates and evaluates options without extending the canonical recording.

Not every boundary requires an exploration checkpoint. It is produced when the next work needs to explore
options before committing one route continuation. It can be a movie-inactive copy of the current recording
checkpoint, but the route may instead require deriving it from an earlier checkpoint upstream of the
actual exploration-phase entry.

Producing the exploration checkpoint ends that worker/job responsibility. The checkpoint is published
durably, and a later job explicitly loads it. SAVOR does not automatically continue from the live
movie-inactive state on the worker that created the checkpoint.

### Recording session / route segment

A recording session is a checkpoint-to-checkpoint route segment. Defined breakpoint or script boundaries
bookend it. A completed session establishes the next checkpoint and extends or branches the complete DTM.

SeedProbe PCs are one class of segment boundary. Battle and navigation entry PCs and reserved ship-battle
scripts are other expected boundary classes. A session can encounter multiple dialogs and choices before
its terminal boundary.

Every segment boundary is implemented by a Dolphin PC breakpoint. SAVOR does not add a separate
before-instruction/after-instruction policy. Dolphin's native breakpoint semantics determine the exact
stop, and research selects breakpoint PCs whose resulting stop state is compatible with the checkpoint or
phase transition that the boundary must establish.

### Presented frame

All user-facing timing offsets are measured in newly presented, non-duplicate frames: the frames a player
experiences when using Dolphin's frame-step behavior. They are not VI frames and are not inferred directly
from the DTM header's frame count.

### Movie input cursor

A DTM stores one controller record whenever the game polls input. Poll records do not have a one-to-one
relationship with presented frames. Dolphin's current movie input count is the cursor into this poll
stream. SAVOR persists and compares that input count, not a byte offset.

The DTM format defines the encoded length of each controller-input record. For the supported Skies of
Arcadia Legends GameCube movie domain, every GameCube controller record is exactly 8 bytes. A byte position
may therefore be derived from the DTM input-data start plus `input_count * 8`, but it is not an independent
annotation anchor. The record width belongs to the DTM input-record type rather than to individual
annotations. Variable-length Wii Remote packets are outside this phase family's supported domain.

An input count `N` identifies the stream boundary after exactly `N` complete input records and before the
zero-based record at index `N`. Record `N` occupies payload-relative bytes `N * 8` through `N * 8 + 7` and
the corresponding absolute DTM bytes beginning at `0x100 + N * 8`. Dolphin uses this same boundary in
playback and recording: it supplies or writes the record first, then increments the public current input
count. Dolphin serializes both its byte cursor and current input count in savestates, while SAVOR exposes
`GetCurrentInputCount()` without conversion.

### Annotation

An annotation is observed metadata related to the recording/analysis that produced it. Relevant examples
include:

- a presented-frame boundary and its DTM poll range;
- a dialog progression input and its movie cursor;
- a choice identity, selected option, and commit input;
- a SeedProbe PC hit;
- a battle, navigation, ship-battle, or other segment boundary; and
- an internal point at which an on-demand checkpoint can later be materialized.

Annotations and child artifacts use the existing database lineage model. They are not global facts that
must be manually reconciled across unrelated movies.

DTM annotations are divided by authority and use:

- Worker-consumed annotations are separate immutable typed artifacts, each bound to the exact DTM they
  describe. Checkpoint itineraries, semantic/progression observations, and presented-frame indexes are
  distinct artifact types rather than sections of one rewritten annotation document.
- Manual bookmarks are a mutable user-facing document associated with a DTM. They may be edited freely and
  provide labels, notes, and other human semantics only. A worker must never consume a manual bookmark as
  execution input, validation authority, or route evidence.

The surviving legacy `DtmAnnotationDoc`/`DTMINI` representation does not define this new artifact model. Its
exact-DTM binding and input-stream anchoring remain useful prior decisions, while its generic bookmark INI
and combined sidecar shape may be replaced without compatibility support.

## Program-kind decomposition

The family contains four individual program kinds:

| Program kind | Identity | Responsibility |
|---|---|---|
| Complete-movie validation | Existing `PK_TasMovie` enum value (`2`) | Replay a complete DTM from game boot and validate reproduction |
| Round 1 segment recording | `PK_TasMovieRecordSegment` | Record and semantically annotate one checkpoint-to-checkpoint segment |
| Round 2 frame reduction | `PK_TasMovieReduceSegment` | Replay one segment, index presented frames, and derive its reduced per-frame schedule |
| Round 3 timing child | `PK_TasMovieCreateTimingChild` | Create one child with one additional neutral presented frame |

The existing `PK_TasMovie` identity is retained specifically for complete-movie validation because that
is the operation the legacy kind fundamentally represented. Its old RTC-oriented descriptor and result
contract are not retained merely because the enum value is reused. The three new enum values will be
assigned when implementation begins; the names above are authoritative.

The implementation and live-validation order is:

1. complete-movie validation using `PK_TasMovie`;
2. reproduce, diagnose, and resolve the known movie desynchronization with the handcrafted root DTM;
3. establish the root DTM's final checkpoint through successful validation;
4. Round 1 segment recording;
5. Round 2 indexing/reduction; and
6. Round 3 one-frame timing-child creation.

## Phase-family workflow

The current design has three segment rounds plus a distinct complete-route validation program kind.

### Round 1: Record and semantically annotate a segment

Purpose: establish a baseline recording from one checkpoint to the next.

1. Start from the source checkpoint, or from boot for the genesis route.
2. When starting from a checkpoint, load its complete same-name DTM, ensure Dolphin is in playback mode,
   and switch to recording at the checkpoint's movie cursor before advancing the guest at all.
3. Record controller input into a complete DTM.
4. Enable the general dialog-advancer service for portions of the route that require it. Ordinary text is
   committed at the earliest eligible presented frame so Round 1 establishes the locally fastest baseline.
5. When the dialog advancer applies an input, record the current DTM input cursor and semantic event.
6. Always observe and annotate configured SeedProbe PCs and other segment-boundary breakpoints/scripts.
   Observation does not mean running a SeedProbe phase between TAS Movie segments.
7. Arm every terminal breakpoint in the global catalog and continue across dialogs and other nonterminal
   interruptions until the first one is reached.
8. Record the identity of the terminal breakpoint that actually ended the segment.
9. At that terminal breakpoint, capture the end savestate while movie recording is still active and record
   its exact movie input cursor. Do not publish the checkpoint pair yet.
10. Disable the dialog advancer and other progression services, disarm the segment breakpoints, acquire
    exclusive neutral gamepad override, reset the poll-observation latch, and step exactly one
    presented/player frame at a time while recording remains active until that latch reports a poll. The
    gamepad override owns the poll signal; an atomic boolean is sufficient for the initial implementation.
    There is no frame-count bound, although ordinary cancellation and runtime-health handling still apply.
11. Finalize the complete DTM only after the neutral guard poll has been observed.
12. Publish the finalized DTM as the captured savestate's exact same-name sidecar. The resulting pair
    intentionally contains a savestate cursor before the end of its DTM input list.

Multiple recording sessions may occur back to back. The last session before a battle, navigation phase,
or ship-battle phase is the expected source of free timing options, though later evidence may identify
other useful segments.

### Round 2: Build the presented-frame index and reduced schedule

Purpose: map the source DTM's controller-poll stream onto player-visible frames and reduce the segment to
one controller input per presented frame for deterministic Round-3 re-emission.

1. Materialize a temporary playback pair consisting of the segment-start savestate and the exact DTM
   produced by Round 1, placed as that savestate's same-name sidecar. Loading it restores the segment-start
   movie cursor while making the newly recorded tail available to playback. This temporary pair is not a
   new persisted checkpoint.
2. Ensure Dolphin is in playback mode.
3. Keep the dialog-advancer service disabled. Round 2 is passive playback of the exact recorded input
   stream and must not inject or replace input.
4. Arm every terminal breakpoint in the global catalog. Replay only this checkpoint-to-checkpoint segment;
   Round 2 does not replay the complete DTM from boot. The exact terminal breakpoint recorded by Round 1
   is the only successful terminal for this replay.
5. Step playback one presented frame at a time.
6. Record the movie input cursor before and after every presented-frame step.
7. Produce an index whose fundamental mapping is:

   `presented frame F -> DTM poll records [cursor_before, cursor_after)`

8. Assume each presented frame contains at least one controller poll. Take the last polled input in that
   frame as the reduced input for the frame. Earlier polls in that frame are intentionally discarded from
   the Round-3 schedule. If live evidence reveals a presented frame with no controller poll, assign an
   explicit neutral input to that frame and preserve it as a timing step.
9. Correlate Round-1 semantic cursor annotations with their presented frames and carry their stable semantic
   identities on the corresponding reduced-schedule entries.
10. Stop only upon reaching the terminal breakpoint recorded by Round 1 and record the observed terminal
    evidence. Reaching a different breakpoint or failing to reach the expected terminal is divergence.
11. Preserve the exact source recording and publish the derived index, reduced per-frame schedule, and
    terminal evidence through normal database artifact relationships.

The Round-2 index and reduced schedule are the source timing artifact used by Round 3. The DTM remains the
complete reproduction artifact; the reduced schedule is a derived segment-local replay artifact.

### Round 3: Replay and re-record a timing variant

Purpose: create a child movie in which a selected progression-commit frame occurs one presented frame
later.

1. Load the source checkpoint and the exact DTM indexed by Round 2.
2. Ensure Dolphin begins in playback mode.
3. Allow ordinary playback to continue until the annotated start-of-frame cursor of the selected
   presented frame. Do not consume any DTM poll record belonging to that source frame.
4. Switch from playback to recording at that start-of-frame cursor.
5. Record one presented frame of neutral input for this child edge.
6. Starting with the selected source frame, apply the Round-2 reduced input for each source frame and step
   by one presented/player frame between scheduled inputs. When applying a schedule entry with a semantic
   event identity, emit the corresponding child semantic observation with its new movie cursor.
7. After the last scheduled input, explicitly return the controller to neutral and allow the game to run.
8. Arm every terminal breakpoint in the global catalog. The exact terminal breakpoint recorded by Round 1
   is the only successful terminal for this child.
9. Keep the dialog-advancer service disabled throughout variant replay/re-recording. The source schedule,
   not newly generated dialog policy, is authoritative for the tail.
10. At the successful terminal breakpoint, capture the child savestate and its exact movie cursor while
    recording remains active.
11. Disable semantic/progression services, disarm the segment breakpoints, acquire exclusive neutral
    gamepad override, reset the poll-observation latch, and step exactly one presented/player frame at a
    time until a poll is observed. Then finalize the complete child DTM and pair it with the already
    captured savestate.
12. Relate the child recording, checkpoint, analysis, and selected delay decision to their parents in the
   database.
13. Re-index the child recording with Round 2 before using it as the parent of another timing mutation.

Round 3 does not mutate DTM bytes by guessing how many controller polls constitute a frame. Round 2 first
measures the real presented-frame boundaries and applies the explicit last-poll reduction rule; Round 3
then measures the inserted delay using actual presented-frame stepping while recording.

The selected frame's Round-2 `cursor_before` value is the authoritative playback-to-recording switch
cursor. Switching at the progression-commit poll inside that frame would be too late because it would
consume part of the source frame before the delay is introduced.

The exact safe transition from playback to recording and the runtime mechanism for applying the reduced
source schedule still require source-level design and live validation.

Round 2 relies on the persisted database hierarchy and Dolphin's TAS movie stability to select the correct
source checkpoint and DTM. It does not add a separate byte-for-byte prefix validation between the new DTM
and the source checkpoint's prior sidecar.

### Establish context, explore, then commit the selected path

Exploration phases do not directly define the canonical movie merely because one of their runs produced a
useful result.

This subsection records the interface between TAS Movie artifacts and adjacent context/exploration work.
The internal round structure, plan composition, and replay design of battle, navigation, ship battle, and
other domain phase families are outside the current TAS Movie planning scope.

1. Select the recording checkpoint that precedes the context and exploration pipeline. This may be earlier
   than the actual battle, navigation, or ship-battle entry.
2. Through a separate explicit operation, load that checkpoint, turn movie recording off without advancing
   the guest, and save a movie-inactive exploration checkpoint with no DTM sidecar. This operation is not
   an optional Round-1 output; it is invoked only when the checkpoint will actually be used.
3. Publish that bootstrap and end the creating job. The durable workflow schedules later context and
   exploration jobs, each of which explicitly loads its assigned checkpoint.
4. When the workflow has actually reached an exploration phase, run the required context phase or phases.
   Context phases include SeedProbe, BattleContext, and NavigationContext; they establish or describe the
   conditions under which the later domain phase will run. TAS Movie does not run SeedProbe between
   ordinary recording segments.
5. Allow enabled session services such as the dialog advancer to traverse any intervening cutscene or
   script work until the actual exploration-phase entry.
6. Run the battle, navigation, ship-battle, or other exploratory domain phase and retain its replayable
   candidate plans and outcomes.
7. Compare the durable candidates and choose the route continuation.
8. Invoke that domain phase family's dedicated end-to-end replay program kind from its recording start
   checkpoint with movie recording active. The replay program consumes the selected plan and performs the
   complete domain replay; TAS Movie does not take ownership of battle/navigation/ship-battle execution.
9. Verify that the recording replay reproduces the selected exploration result before accepting its output.
10. Record the chosen inputs into the complete DTM and establish the next recording checkpoint. A mismatch
   is a quarantined recording child under the same durable failure policy as other desynchronization.

For example, a navigation exploration may need to bootstrap at the last script-boundary checkpoint, run
SeedProbe there, allow the dialog handler to advance an intervening cutscene, and only then begin the
navigation phase. That sequencing is defined case by case rather than assuming the nearest phase-entry
breakpoint is always the correct exploration checkpoint.

Every route-relevant candidate selected from a context or exploration pipeline must have a replayable
decision or plan. Examples include a selected SeedProbe input, battle command plan, navigation input
schedule, or ship-battle plan. The later recording rerun is the canonical reproduction of that choice.

Each exploratory domain phase family is expected to contain multiple program kinds that optimize
individual rounds or subproblems plus one program kind that replays the selected result in its entirety
from the domain replay's start checkpoint. The optimization programs may be specialized; the replay
program is the authoritative executable composition of their selected outputs.

### Complete-movie validation

Replay the selected complete DTM from game boot. This validates that the accumulated route still
reproduces with correct timing independently of checkpoint acceleration. It is not the ordinary way
workers resume route construction. It uses the existing `PK_TasMovie` program-kind identity and is the
first implementation/validation slice for the phase family.

Every complete DTM/movie-variant record references one immutable ordered checkpoint-itinerary artifact.
Each entry contains only:

- the expected breakpoint PC; and
- the expected Dolphin movie input cursor at that breakpoint.

Movie frame and absolute VI are retained as ordinary evidence but are not validation predicates. Dolphin
deterministic mode is expected to reproduce them, and they do not add useful desynchronization information
beyond whether playback reached the right PC before passing its expected input cursor.

The handcrafted root's final checkpoint entry is authored or established explicitly. Round 1 appends the
terminal breakpoint and saved checkpoint cursor it actually observed to a new child itinerary artifact.
A successful Round-3 child copies the earlier entries into a new immutable artifact and replaces the final
entry with the cursor it observed at the same terminal breakpoint after applying its timing mutation.

Validation arms every breakpoint in the global segment-boundary catalog and periodically requests the live
movie input cursor from Dolphin while playback runs. Playback consumes controller records inside Dolphin's
movie layer and does not poll SAVOR's gamepad override, so the recording-side atomic poll latch cannot
drive validation. Let `(expected_pc, expected_cursor)` be the next itinerary entry:

1. While `current_cursor < expected_cursor`, continue playback. A catalog breakpoint reached at a smaller
   cursor does not satisfy the entry and does not by itself prove desynchronization; resume execution.
2. While `current_cursor == expected_cursor`, wait for `expected_pc`. Reaching that PC at that cursor passes
   the entry and advances validation to the next itinerary entry. If another catalog breakpoint is reached
   at the same cursor, resume and continue waiting for `expected_pc`.
3. If `current_cursor > expected_cursor` before the expected PC/cursor pair was observed, declare movie
   desynchronization. This catches an expected checkpoint that playback passed without reaching.
4. After the final expected PC/cursor pair passes, continue through later catalog hits and return `Valid`
   only when the owned movie reaches movie end. First-time root validation captures its checkpoint while
   paused at the accepted final pair, but publishes it only after the tail also succeeds.

The cursor comparison, not mere membership in the global breakpoint catalog, determines whether a
breakpoint belongs to the next expected checkpoint. This naturally tolerates later-added catalog PCs and
recurring PCs when they are reached before the next expected cursor.

This cursor-relative tolerance is initially specific to complete-movie validation. Round 2 and Round 3
retain their stricter segment contract: reaching an unexpected terminal breakpoint immediately fails and
quarantines that attempt. The same tolerant behavior can be considered for those rounds later if evidence
shows it is useful.

This provides coarse automatic desynchronization detection even when Dolphin cannot identify the first
fine-grained point at which game state diverged. The root DTM has fewer accumulated checkpoint observations
and therefore remains the hardest movie to diagnose.

The root DTM's successful validation captures its final savestate while read-only playback remains active.
The complete root DTM is preserved as the savestate's same-name sidecar, and the savestate serializes the
root's pre-end movie cursor at the required breakpoint. This output is published as the first recording
checkpoint used by Round 1. Other complete-movie validations publish validation evidence only and do not
create another savestate.

Root validation is mandatory to establish the first recording checkpoint. Validation of generated DTMs
is performed only when explicitly requested; it is not automatically scheduled after recording or timing
mutation.

A failed `VALIDATE` quarantines the source DTM branch itself. It cannot supply a checkpoint or parent movie
for further route construction until a later `VALIDATE` succeeds or the failure is otherwise resolved. A
later successful validation automatically restores eligibility only when it validates the exact same
content-addressed DTM; the earlier quarantine and validation history remain durable.
`ESTABLISH_ROOT_CURSOR` publishes a candidate only and does not alter trust or quarantine. Any future
diagnostic operation's eligibility effects are deferred with the rest of its contract.

Complete-movie validation does not capture a new savestate at its coarse failure-detection point. That point
can be later than the actual divergence. Instead, an invalid result identifies the last itinerary entry
proved correct, and the result processor resolves it to the corresponding known-good database checkpoint.
When no entry passed, investigation begins from the root DTM at boot. Detection-point capture remains part
of the deferred diagnostic-mode design.

Required durable validation evidence is intentionally narrow: source and effective DTM identities and
hashes, immutable itinerary identity, required terminal PC, typed outcome/failure reason, expected and
actual PC/input counts, optional last-verified itinerary index, last-known-good checkpoint when resolvable,
worker provenance, and exact Full Phase/module identities. Movie frame, absolute VI, elapsed time, and a
diagnostic savestate are not persisted by this validation ledger.

The earlier validation attempt encountered actual movie desynchronization. After the new validation
program kind is implemented, reproducing, diagnosing, and resolving that desynchronization is the first
live-validation task and a gate before implementing Round 1.

The root's initially unknown checkpoint cursor is not established while that potential desynchronization
remains unresolved. After the issue is fixed:

1. the user explicitly invokes `ESTABLISH_ROOT_CURSOR`, declaring that the potential desynchronization is
   believed fixed;
2. that run reaches the authored target PC and publishes a candidate immutable itinerary containing the
   observed checkpoint cursor, but no trusted route checkpoint;
3. the user invokes `VALIDATE` against that exact PC/cursor itinerary entry; and
4. only this second run may trust and publish the canonical root checkpoint.

The current `PK_TasMovie` contract distinguishes:

- `ESTABLISH_ROOT_CURSOR`: an explicit user-authorized post-fix run that produces the candidate root
  itinerary entry but does not establish the checkpoint; and
- `VALIDATE`: exact ordered-itinerary validation. Only successful root `VALIDATE` publishes the canonical
  root checkpoint; successful generated-DTM validation publishes validation status/evidence only.

Diagnostic execution is deliberately deferred. It will need to be flexible, and its request modes,
continuation behavior, trace artifacts, trust effects, and relationship to `PK_TasMovie` will be designed
only after more information has been gathered. The ordinary failure evidence already required from
`VALIDATE` does not imply a general-purpose diagnostic mode.

## General dialog-advancer service

The dialog advancer does not yet exist. It should be developed using the same evidence-driven process and
bounded input-control principles as the battle input macro.

It is a general worker/session service that can be enabled or disabled. It is not owned exclusively by the
TAS Movie phases:

- it is disabled by default and must be enabled explicitly by a program invocation;
- expected enabled uses include segment recording, navigation, and ship-battle-related phases;
- battle phases normally do not need it;
- Round-2 presented-frame indexing must disable it; and
- Round-3 timing-variant replay/re-recording must disable it.

The service must remain game-specific in its dialog policy while using generic runtime input, stop,
breakpoint, and interruption facilities. Its internal controller is expected to resemble an adaptive input
macro: wait for evidence-backed readiness points, apply bounded input pulses, verify guest observation and
completion, restore neutral input, and report structured events.

The referenced static analysis proposes a session-level interruption service below the former
PhaseScriptVM breakpoint scopes. The new Execution Runtime design should supply equivalent interruption
semantics without reviving PhaseScript-specific architecture.

### Timing events

The timing abstraction is a generic **progression-commit frame**, not a dialog event. A progression commit
is an input that causes the route to advance in a way whose presented-frame timing can affect later game
state or RNG context. The dialog advancer is one current producer of progression-commit observations, but
the TAS Movie contract must allow other services or mechanisms to identify them later.

Current known progression-committing inputs are:

- accepting a completed text prompt; and
- confirming a choice.

Text-acceleration presses and choice-navigation inputs are not themselves timing branch points, although
they remain part of the source schedule and may be useful annotation/checkpoint locations.

### Dialog identity

Ordinary dialog events can use a segment-local identity:

`recording-session lineage + chronological dialog occurrence + dialog kind`

The annotation also records its breakpoint/semantic site, movie cursor, and presented frame when known.

### Choice identity and decisions

Choices are different from ordinary text because the selected option can affect game state and therefore
the route itself.

A choice option is a first-class route decision that is fixed before timing exploration of that route
branch. Each selected option establishes a distinct baseline branch; dialog timing delays are explored
within that choice branch rather than silently changing the selected game-state outcome.

SpiceSct provides the likely durable lookup vocabulary. Its IR identifies an SCT source, a section by
index and name, and an instruction by section-relative `offset` plus file-payload `payloadOffset`.
SAVOR's current Dolphin integration can observe the live SCT file tag and convert the current runtime
instruction pointer into a section name and section-relative offset. The intended choice identity is
therefore based on:

`checkpoint VI frame + SCT artifact hash + section index/name + section-relative instruction offset`

The VI-frame prefix is the VI at which the associated choice-decision checkpoint is saved, immediately
after the relevant text-acceleration frame has completed. It distinguishes recurring visits to the same
choice instruction and directly relates the decision to its checkpoint. The raw runtime pointer and
file-payload offset remain useful evidence for resolving the identity but are not the sole persisted key.
The decision is scoped to its route branch so another branch can deliberately choose a different option
at the same underlying script site. Runtime-to-SpiceSct matching still needs live validation.

If execution reaches a choice for which no decision exists:

1. do not silently select an option;
2. remake the section with a checkpoint immediately after the presented frame containing the relevant
   text-acceleration input and before committing the choice;
3. expose that checkpoint as a user-decision branch point; and
4. let the chosen option become explicit route input for the continuation.

After the user selects an option, that decision becomes explicit route input and the remade continuation
establishes the corresponding baseline branch. Thus every executed choice is decided ahead of timing
exploration, while checkpoint-and-ask remains the discovery path for a choice that was not decided before
the original attempt.

## Timing-option search

A source session can contain multiple progression-commit frames. Variant generation should
eventually support:

- cumulative delays of `1..N` presented frames at one event;
- delay vectors across multiple segment-local progression commits; and
- user-selected choice branches whose game-state effects define separate continuations.

Timing delays are expanded iteratively rather than generated as unrelated direct children of the original
recording:

1. begin with an indexed parent recording;
2. create and store one child containing one additional neutral presented frame at one selected event;
3. run Round 2 on that child to regenerate its frame index and correlate its semantic annotations;
4. use the re-indexed child as the parent for the next one-frame delay; and
5. retain every cumulative child, producing a durable `+1`, `+2`, ... `+N` lineage.

For multiple progression-commit frames, the same rule creates a branching tree. Each edge represents one
additional one-frame timing decision applied to an already re-indexed parent. A branch can delay the same
event again or select another annotated event in the current parent. Frame identities and cursors are
always taken from that immediate parent, never projected through an older ancestor's index.

Workers execute bounded recording, indexing, and variant operations. Durable orchestration owns candidate
generation, branching, comparison, selection, follow-up SeedProbe work, and cancellation. A worker does
not decide which timing or choice branch becomes canonical.

The candidate-selection and pruning strategy is deliberately undecided. Initial implementation should
first prove one iterative edge: delay one identified progression event by one presented frame, reproduce
and store the child checkpoint/DTM, and successfully re-index that child for another possible edge.

### Failed or desynchronized children

A timing-variant attempt that fails to reach its expected terminal breakpoint or desynchronizes is not
silently discarded. Its failure and available evidence are recorded durably so the player can inspect the
branch and decide how to proceed.

Such a child is quarantined from further use:

- it cannot become a parent for another delay;
- it cannot feed a downstream SeedProbe, battle, navigation, or ship-battle phase; and
- it cannot be selected as an established checkpoint branch.

Quarantine applies only to that child. Its indexed parent remains eligible for retries and sibling timing
variants.

The durable failure evidence should include:

- the partial child DTM, when available;
- the exact parent DTM;
- the expected terminal breakpoint;
- the last observed PC;
- the expected and observed movie input counts;
- the stop or desynchronization reason; and
- the last-known-good source checkpoint reference.

No failure-side diagnostic savestate, movie-frame value, or absolute VI value is part of the approved
durable validation model.

The exact resolution actions are intentionally deferred. For the initial design, a quarantined child
simply remains unavailable for all further use.

The initial failure classification is intentionally small:

- **Expected terminal not reached:** the run ends or becomes unusable without reaching the source
  segment's recorded terminal breakpoint.
- **Unexpected terminal reached:** the run reaches another armed/known boundary instead of the expected
  terminal breakpoint.
- **Movie desynchronization:** Dolphin or runtime evidence reports that playback/re-recording no longer
  reproduces the source execution.
- **Unknown failure:** a durable child attempt fails in a way not yet classified.

When one of these occurs after a child attempt has begun and useful branch evidence exists, persist the
evidence and quarantine that child. A preflight failure that prevents loading/opening the source
checkpoint or DTM is a retryable execution failure with no child rather than a quarantined branch. Other
failure categories will be added only when encountered.

Round 2 and Round 3 stop immediately when any known terminal breakpoint is reached. If it is not the
source segment's expected terminal, the attempt is classified as `unexpected_terminal`; execution does
not continue in hopes of reaching the expected breakpoint later.

## Terminal-breakpoint catalog

The terminal-breakpoint catalog is global and monotonic: new valid boundary PCs may be added as route
construction encounters them, but an established terminal breakpoint is never removed. Every Round-1
invocation arms every breakpoint in that catalog rather than selecting a segment-specific subset.

Round 1 records which member actually ended the segment. That observed identity becomes part of the
segment contract. Round 2 and Round 3 also arm the complete catalog, but only the recorded member is the
expected successful terminal for that source segment. Any other terminal is an immediate quarantined
failure.

## SeedProbe relationship

SeedProbe remains a separate scalar context phase. It generates starting RNG options and helps define the
context used by a later battle, navigation, ship-battle, or other domain phase. It is not itself an
exploration phase. BattleContext and NavigationContext are other context phases.

TAS Movie work does not absorb SeedProbe seed comparison or selection behavior.
It also does not run SeedProbe merely because a TAS Movie segment reached or annotated a SeedProbe PC.
Context is generated only when the durable workflow reaches an exploration phase that requires it.

The relationship is:

- TAS Movie recording and playback observe configured SeedProbe PCs as segment boundaries or annotated
  internal points;
- a checkpoint can be materialized at the relevant boundary when required;
- SeedProbe runs from an established movie-inactive context checkpoint and reports RNG evidence;
- durable coordination compares the resulting seed options and selects or expands route branches.

Once the first battle route has been fixed, RTC is no longer a free timing variable. Later route segments
obtain timing options through controlled input timing, especially delayed progression-committing dialog
inputs that alter the Time Base Register conditions seen at later state/script transitions.

## Persistence and lineage

The legacy `state_tas_movie_variant` model has been replaced without row migration. The implemented State
model separates the two immutable route identities:

- `state_tas_movie_root` owns one successfully validated RTC-specific root: handcrafted source DTM, exact
  RTC-patched DTM, nonnegative RTC, immutable `TMI1` itinerary, required final PC, and canonical checkpoint;
- `state_tas_movie_trees` owns generated non-root movies, their exact DTM/itinerary/checkpoint, owning root,
  and optional same-root parent tree movie;
- `state_artifact` content-addresses DTM, savestate, and `TAS_MOVIE_ITINERARY` (`.tmi`) artifacts; and
- `state_savestate` reuses an exact artifact-backed checkpoint while State outbox events expose immutable
  root/tree creation.

Tree rows may be created by their producing recording round without complete-movie validation. Validation
trust is separately projected by exact effective DTM SHA-256: absent is untested, `VALID` is eligible, and
`QUARANTINED` blocks only those exact bytes. A later `Valid` restores `VALID` without deleting attempts or
alerts.

The Analysis TAS Movie ledger owns one immutable request per workflow step, append-only attempts keyed by
execution job and worker-terminal hash, and the current exact-hash validation projection. Establishment
publishes its one-entry `TMI1` candidate but does not change validation status. State writes precede the
final Analysis attempt/status transaction so recovery can verify and reconstruct a previously persisted
decision.

## Worker/program boundaries

The following are the approved program-kind boundaries and names:

| Program kind | Bounded worker responsibility | Principal output |
|---|---|---|
| Record segment (`PK_TasMovieRecordSegment`) | Extend a recording from source to terminal boundary with dialog service enabled as configured | End checkpoint, full DTM, semantic annotations |
| Reduce presented frames (`PK_TasMovieReduceSegment`) | Replay the exact segment, map frames to poll ranges, and select the last input poll per frame | Frame index, reduced per-frame schedule, exact terminal evidence |
| Create timing child (`PK_TasMovieCreateTimingChild`) | Playback to a selected frame, switch to recording, insert one neutral presented frame, and apply the reduced tail | Child checkpoint and full child DTM |
| Validate complete movie (`PK_TasMovie`) | Replay from boot and validate the ordered checkpoint PC/cursor itinerary | Validation result; genesis final checkpoint once |

Movie-inactive exploration-checkpoint creation is a separate, explicitly invoked state/movie operation.
It is not a fifth TAS Movie segment round and is not produced automatically by Round 1.

The end-to-end replay program kinds for battle, navigation, ship battle, and other exploratory domains
belong to those domain phase families, not to the TAS Movie program family.

## Existing implementation substrate

Current code already provides useful foundations:

- `SavestateService` captures immutable savestates with exact DTM sidecars and movie metadata.
- `MovieService` and its Dolphin backend expose playback/recording state and movie cursors.
- Dolphin savestates serialize movie cursor state and load a same-name DTM sidecar.
- Dolphin frame stepping can stop after a newly presented, non-duplicate frame.
- `InputMacroRuntime` provides breakpoint-qualified input, neutral-frame stepping, input observation
  evidence, exclusive ownership, and cleanup behavior.
- `ProgramRuntime`, `ExecutionEngine`, `StopPointRouter`, `InputArbiter`, and interruption-handler concepts
  are the intended refactored execution substrate.

Dolphin's ordinary DTM playback does not detect arbitrary divergence in guest game state. Its direct
movie checks cover structural conditions such as premature input exhaustion and savestate/movie prefix
mismatch; its source explicitly notes that correct playback depends on reproducing controller polling in
the recorded order. The current SAVOR backend's `ended` observation likewise means that a previously
playing movie became inactive, not that reproduction was proven correct. Therefore the ordered
breakpoint/cursor itinerary is the initial authoritative desynchronization detector. SAVOR's execution
snapshot already exposes PC, absolute VI, and movie input count. VI remains useful diagnostic evidence but
is deliberately not part of the checkpoint pass/fail predicate.

Dolphin savestates serialize `m_current_byte` and `m_current_input_count`. When a read-only movie state is
loaded, Dolphin permits the supplied DTM to extend beyond the saved cursor and verifies that the DTM prefix
through the saved byte cursor matches the movie history associated with the state. This supports capturing
the savestate at the checkpoint, appending a neutral guard frame afterward, and pairing the finalized
longer DTM with the earlier state, as long as no prefix input is changed.

When Dolphin subsequently records a new GameCube input at that restored cursor, `RecordInput` resizes the
movie input buffer to `current_byte + sizeof(ControllerState)` before writing. The first new recorded poll
therefore truncates/replaces the old guard suffix. The guard input is checkpoint safety padding, not a
permanent route input that must be replayed when the checkpoint is extended.

These foundations may be changed where necessary. They do not require preserving the legacy TAS Movie
program or descriptor.

## Open questions

1. What exact Dolphin transition and runtime action safely converts source playback into child recording
   while retaining access to the indexed source tail?
2. Does the dialog advancer run as one session interruption handler/service, and how does it compose with
   long-running navigation and ship-battle operations without taking their durable coordination role?
3. What exact SCT content identity and runtime evidence reliably map a live choice to SpiceSct's section
   index/name and section-relative instruction offset?
4. Which exact PCs and reserved scripts define the first implemented recording-session start and terminal
   boundaries?
5. Which semantic observations must be recorded at every annotation so a later replay can materialize an
   internal checkpoint exactly?
6. How should the iterative delay tree be bounded and pruned after one-frame child creation and child
   re-indexing are proven?
7. What exact runtime operation creates and publishes the movie-inactive exploration checkpoint at the
   same guest stop as its source recording checkpoint, with no DTM sidecar and without advancing the guest?

## Research and source evidence

Primary evidence currently includes:

- Published Dolphin DTM format, including the 8-byte GameCube controller record:
  `https://tasvideos.org/EmulatorResources/Dolphin/DTM`

- Dolphin movie/savestate interaction:
  `C:\Users\jahor\source\repos\jahorta\dolphin-2506a\Source\Core\Core\State.cpp`
- Dolphin movie cursor, controller polling, playback, and recording:
  `C:\Users\jahor\source\repos\jahorta\dolphin-2506a\Source\Core\Core\Movie.cpp`
- Dolphin presented-frame stepping:
  `C:\Users\jahor\source\repos\jahorta\dolphin-2506a\Source\Core\Core\Core.cpp`
- Dialog/control-stack research:
  `D:\SoAInvestigate\Analyses\20260723_1825_text_choice_breakpoints\20260723_2054_savor_breakpoint_stack_dialog_insertion_summary.txt`
- Current SpiceSct IR and parser:
  `third-party/SPICE/SpiceSct/SctModel.h` and `third-party/SPICE/SpiceSct/SctParser.cpp`
- Current SAVOR live SCT file/section observation:
  `SavorCore/Core/DolphinWrapper.cpp`
- Current runtime state/movie services:
  `SavorCore/Runner/Runtime/Services/Savestate/` and `SavorCore/Runner/Runtime/Services/Movie/`
- Current `PK_TasMovie = 2` identity and complete-validation behavior:
  `SavorCore/Runner/IPC/Wire.h`,
  `SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.*`, and
  `SavorDb/Execution/ProgramDB/TasMovieValidation/TasMovieValidationProgram.*`
- Current input macro substrate:
  `SavorCore/Runner/InputMacro/`
- Current persistence contracts and migrations:
  `SavorDb/State/`, `SavorDb/Analysis/`, and `SavorDb/migration/`
- Refactored program/runtime architecture guidance and implemented checkpoint:
  `planning/ExecutionRuntime/`

## Decision log

### 2026-08-02

- The handcrafted root DTM begins at game boot and is the genesis movie.
- A recording session is a checkpoint-to-checkpoint route segment.
- Recording checkpoints retain the complete DTM sidecar and act as cursors into that movie; exploration
  checkpoints are movie-inactive and have no DTM sidecar.
- User-facing delays are presented-frame delays, not VI-frame delays.
- Round 1 records and semantically annotates a baseline segment.
- Round 2 maps DTM input-poll ranges to presented frames.
- Round 2 is passive exact playback with the dialog advancer disabled.
- Round 2 transiently pairs the segment-start savestate with the newly generated Round-1 DTM so Dolphin
  restores the start cursor and can play the new tail; this does not create another persisted checkpoint.
- Round 2 trusts the persisted hierarchy and Dolphin's TAS movie stability; it does not perform an extra
  byte-for-byte DTM-prefix validation.
- Round 3 begins in playback, switches to recording at a selected frame, records neutral presented frames,
  re-emits the source tail, and produces a child checkpoint/DTM.
- The Round-3 playback-to-recording transition occurs at the selected frame's annotated start cursor,
  before any source poll from that frame is consumed; re-emission then begins with the complete selected
  source frame.
- Timing delays are created iteratively one frame at a time. Every cumulative child is stored and must be
  re-indexed before it can serve as the parent of another delay, yielding durable `+1` through `+N`
  lineage and a branch tree across multiple progression-commit frames.
- The dialog advancer is disabled during Round 3.
- The dialog advancer is a reusable enable/disable service needed outside TAS Movie work, including
  navigation and ship-battle phases.
- The dialog advancer is disabled by default and each program invocation must enable it explicitly.
- The timing target is a generic progression-commit frame. Text acceptance and choice confirmation are
  the current known producers, but future non-dialog producers can use the same abstraction.
- Round 1 advances ordinary text at the earliest eligible presented frame to establish the locally fastest
  baseline.
- Ordinary dialogs use segment-local occurrence identity.
- A choice option is a first-class route decision. Each selected option establishes a distinct baseline
  branch before timing exploration.
- Choice lookup should map live SCT file/section/pointer evidence onto a SpiceSct identity containing SCT
  artifact hash, section index/name, and section-relative instruction offset, prefixed by the VI-frame
  number to distinguish recurring choices.
- The VI prefix is the VI of the associated checkpoint saved immediately after text acceleration has
  completed.
- Choice decisions are scoped to a route branch, allowing different branches to make different decisions
  at the same underlying script site.
- An unknown choice must not be silently answered; the section is remade with a checkpoint before choice
  commitment so the user can define the branch.
- Every segment boundary is a selected Dolphin PC breakpoint. Dolphin supplies its native stop semantics;
  SAVOR chooses breakpoint PCs compatible with the required checkpoint/transition state rather than
  configuring a separate before/after mode.
- A child that misses the expected terminal breakpoint or desynchronizes is recorded durably and
  quarantined from all further use. Its parent remains available for retries and siblings. Resolution
  semantics are deferred.
- Failure evidence includes the partial child DTM, parent DTM, expected terminal breakpoint, expected and
  observed movie input counts, last PC, failure reason, and the last-known-good source checkpoint. It does
  not add a failure-side diagnostic savestate, movie-frame value, or absolute VI value.
- A segment-end checkpoint is saved while movie recording is active and before the recording is finalized.
  When option exploration is needed, a separate explicit operation derives a movie-inactive checkpoint
  with no DTM sidecar from the appropriate recording checkpoint without advancing the guest.
- The exploration checkpoint can be upstream of the actual exploration-phase entry. Context phases and
  enabled services may carry it through intervening script/cutscene work case by case.
- Creating an exploration checkpoint always publishes it and ends that job responsibility. A later job
  explicitly loads it from its own artifact baseline.
- SeedProbe is a context phase, as are BattleContext and NavigationContext; it is not an exploration phase.
- TAS Movie records SeedProbe-PC annotations but does not run SeedProbe between ordinary movie segments.
  Required context phases are scheduled only when the durable workflow reaches an exploration phase.
- Battle, navigation, ship-battle, and similar domain phases perform exploration. Once a path is selected,
  that domain family's dedicated end-to-end replay program kind runs from its recording start checkpoint
  with recording active so its inputs become part of the canonical DTM.
- Each exploratory domain phase family can split optimization across multiple round-specific program kinds;
  its dedicated replay program consumes their selected outputs and performs the full replay.
- A selected context/exploration result must include a replayable decision or plan. The recording rerun
  must match the selected result or its child is quarantined.
- Exploration checkpoints retain database lineage to the exact parent recording checkpoint and parent DTM
  despite having no physical DTM sidecar.
- Match acceptance is defined independently by each domain phase family.
- The choice identity's checkpoint VI is Dolphin's absolute emulated VI counter saved in the state, not a
  movie frame count or presented-frame index.
- Existing database lineage is expected to support source/child recordings, analyses, artifacts, and
  checkpoints without a new branching model.
- The phase family has four individual program kinds: complete-movie validation, Round 1 segment recording,
  Round 2 frame reduction/indexing, and Round 3 timing-child creation.
- The existing `PK_TasMovie` enum value is retained for complete-movie validation. The three rounds receive
  new program-kind identities and descriptors.
- Implementation begins with complete-movie validation, then proceeds in round order: Round 1, Round 2,
  and Round 3.
- A Round-1 invocation accepts a set of terminal breakpoints. The known catalog is additive: established
  terminal breakpoints may be supplemented but are never removed.
- Round 1 records the actual terminal breakpoint reached. Round 2 and Round 3 must reach and stop at that
  exact terminal boundary.
- Round 2 replays only its checkpoint-to-checkpoint segment, not the complete DTM from boot.
- Round 2 derives one scheduled controller input per presented frame by choosing the final input poll in
  that frame. The index, reduced schedule, and terminal evidence form its derived output.
- Round 3 applies the reduced source inputs between presented/player-frame steps. After the final scheduled
  input, it lets execution continue until the exact recorded terminal breakpoint.
- Round 3 initially delays generic progression-commit frames. Dialog handling can identify such frames but
  does not define or own the progression-commit abstraction; other sources may be added later.
- Movie-inactive exploration-checkpoint creation is a separate explicit operation, not an optional output
  of Round 1.
- Initial durable failure categories are expected-terminal-not-reached, unexpected-terminal-reached,
  movie desynchronization, and unknown failure. Source checkpoint/DTM preflight failures that create no
  child are retryable execution failures rather than quarantined branches.
- Complete-movie validation always requires reaching the DTM's recorded final PC breakpoint.
- Generated recording checkpoints are captured at the terminal breakpoint before the DTM is finalized.
  Recording then holds neutral and steps by presented frames until an atomic gamepad-poll latch reports a
  poll. The resulting finalized DTM is paired with the earlier savestate, so the saved cursor intentionally
  precedes a neutral guard input in the DTM. The loop has no frame-count bound but remains cancelable.
- The checkpoint/neutral-until-polled/finalize/pair endpoint contract applies to every complete-DTM
  producer, including Round 1, Round 3, and domain replay program kinds.
- Successful genesis-DTM validation produces its final savestate. Later complete-movie validations publish
  validation evidence only.
- Validation evidence records exact DTM/itinerary and program identities, typed outcome/failure facts,
  expected and actual PC/input counts, last verified itinerary index, and worker provenance. It does not
  persist movie frame, absolute VI, elapsed time, or a diagnostic savestate.
- The previous full-movie attempt suffered actual movie desynchronization. Reproducing and resolving it
  after implementing the validation kind is the gate before Round 1 work begins.
- Round 2 assumes every presented frame has at least one controller poll and uses its last poll as the
  reduced frame input. If a no-poll frame is discovered, its fallback reduced input is neutral.
- After Round 3 applies the final reduced input, it explicitly restores neutral before free execution to
  the terminal boundary.
- Reduced schedule entries carry stable semantic identities. Round 3 re-emits the child semantic
  observation and captures its new cursor; the child's Round 2 then correlates that observation with its
  measured frame boundaries.
- Every Round-1 invocation arms every breakpoint in the global additive terminal catalog. Round 2 and
  Round 3 likewise arm the full catalog while accepting only the source segment's recorded terminal.
- Round 2 or Round 3 stops immediately and quarantines its attempt as `unexpected_terminal` when another
  known terminal breakpoint is reached first.
- Each complete DTM/movie-variant record owns its required final PC. The root value is authored, a Round-1
  baseline receives its observed terminal, and a successful Round-3 child inherits that terminal from its
  immediate parent.
- Root validation captures its final savestate while read-only playback remains active, preserving the
  complete root DTM sidecar and serialized pre-end cursor, and publishes it as the first recording
  checkpoint.
- Before advancing from any source checkpoint, Round 1 loads its complete DTM, ensures playback mode, and
  switches to recording exactly at the saved movie cursor.
- A failed complete-movie validation quarantines the source DTM branch from further route construction
  until validation later succeeds or the failure is otherwise resolved.
- Root validation is mandatory. Generated complete movies are validated only when explicitly requested.
- The new program-kind names are `PK_TasMovieRecordSegment`, `PK_TasMovieReduceSegment`, and
  `PK_TasMovieCreateTimingChild`; round numbers remain explanatory workflow terminology.
- Full-movie validation arms the complete global segment-boundary catalog and validates an ordered
  checkpoint itinerary of expected breakpoint PCs and movie input cursors before accepting the final PC.
- While the live cursor is below the next expected checkpoint cursor, validation continues even when a
  catalog breakpoint is hit. At cursor equality, the expected PC passes the checkpoint. Advancing beyond
  the expected cursor without that PC/cursor observation is coarse evidence of desynchronization.
- A non-expected catalog breakpoint hit at cursor equality is resumed; validation continues waiting for
  the expected PC and fails only if the cursor advances past that expectation first.
- Cursor-relative breakpoint tolerance initially applies only to complete-movie validation. Round 2 and
  Round 3 retain immediate `unexpected_terminal` failure semantics.
- Checkpoint validation uses only expected breakpoint PC and movie input cursor. Movie frame and absolute
  VI remain evidence, not pass/fail predicates.
- A successful revalidation removes a DTM branch's active quarantine only when the successfully validated
  artifact has exactly the same DTM content hash. Historical quarantine and validation records remain.
- Desynchronized validation refers to the last checkpoint proven correct rather than capturing a new state
  at the later, coarse failure-detection point.
- The root cursor is established and then verified by a second run only after the potential root movie
  desynchronization has been fixed. Pre-fix runs are diagnostic and do not establish a trusted cursor or
  route checkpoint.
- Each complete DTM references one immutable ordered checkpoint-itinerary artifact containing only
  `(breakpoint PC, movie input cursor)` entries. Round 1 appends an entry through a new child artifact;
  Round 3 creates a new artifact with its newly observed final cursor.
- Movie-input positions are persisted as input counts. The DTM record type defines the fixed encoded
  length; supported GameCube controller records are 8 bytes, so byte offsets are derived rather than stored
  as annotation identity.
- Input count `N` is the boundary after `N` complete records and before zero-based record `N`; a Round-3
  transition at count `N` occurs before source record `N` is consumed.
- Worker-consumed DTM annotations are several separate immutable typed artifacts bound to the exact DTM,
  including checkpoint itineraries, semantic observations, and presented-frame indexes.
- Manual bookmarks are mutable, user-facing semantics only. Workers never consume them as invocation input,
  validation authority, or route evidence.
- `PK_TasMovie` currently distinguishes `ESTABLISH_ROOT_CURSOR` and `VALIDATE`. Explicit user selection of
  `ESTABLISH_ROOT_CURSOR` declares that the root desync is believed fixed.
- Both operations use one Full Phase entrypoint, which branches on that typed operation value.
- The descriptor registers three exact step kinds: `tasmovie.establish_root_cursor`,
  `tasmovie.validate_root`, and `tasmovie.validate_tree`. All map to `PK_TasMovie` and the single immutable
  entrypoint. The step kind determines the operation; there is no free-form operation argument.
- Materialization snapshots the operation and exact movie, DTM, itinerary, Full Phase, and root-capture
  authority in an immutable Analysis DB validation-request record. The execution job references that record,
  and reconstruction rejects identity drift.
- Each explicit user command creates one immutable validation request, business job, and sealed singleton
  workset. Recovery of the same activation reuses them; a later explicit command creates a new immutable set,
  even for the same DTM. Infrastructure execution attempts may accumulate beneath the unchanged job.
- Every typed TAS Movie result, including `Invalid`, finalizes the business job as `SUCCEEDED`. `Invalid`
  means validation executed successfully; quarantine records the domain failure. Job `FAILED` is reserved for
  worker/execution failures and may be explicitly requeued.
- There are no automatic retries. A terminal worker/execution failure alerts the user, who decides whether
  to requeue the same immutable job.
- Complete-movie validation is the only singleton step in its workflow and has no next-step continuation.
- Root RTC ranges are launcher convenience only: each RTC value becomes a separate singleton workflow,
  request, job, and one-item workset so independent validations can run concurrently.
- SAVOR expresses an RTC value as GameCube-visible seconds since 2000-01-01 and
  accepts the complete inclusive `u32` domain (`0..4294967295`). The DTM keeps
  its 64-bit Unix `recordingStartTime`; patching adds the GameCube epoch before
  writing that field. Values outside the `u32` domain are rejected so distinct
  persisted values cannot alias to the same Dolphin RTC.
- A successful first validation of a source-DTM/RTC pair publishes the exact patched DTM, its canonical
  checkpoint with same-name DTM sidecar, and one immutable root row. Revalidating an existing root and
  validating a tree movie request no checkpoint capture and reject unexpected worker artifacts.
- Only typed establishment/valid/invalid results create Analysis DB validation-attempt rows. Infrastructure,
  preflight, cancellation, and contract failures remain Execution DB evidence and do not affect eligibility.
- The module request contains only worker-execution facts. Database identities, artifact hashes, and
  game/disc identity remain descriptor/coordinator facts verified before dispatch.
- Validation derives its required terminal from the last expected itinerary entry; the request does not
  duplicate that PC in a separate field.
- `ESTABLISH_ROOT_CURSOR` publishes only a candidate itinerary. The following exact root `VALIDATE` run is
  the only run that may publish the canonical root checkpoint.
- Diagnostic-mode design is deferred until more evidence is available. No diagnostic request shape,
  continuation policy, trace artifact, or trust effect is currently approved.
- Complete-movie playback detects cursor overrun by periodically requesting Dolphin's current movie input
  cursor. It cannot use the recording-side gamepad poll latch because Dolphin's internal movie layer
  supplies playback input without polling SAVOR's gamepad override.
- Guard-tail production disables progression services, disarms segment breakpoints, and gives a true
  neutral gamepad override exclusive ownership while stepping one presented frame at a time until its
  atomic poll latch is set.
- The complete-movie checkpoint-breakpoint catalog is defined directly inside its immutable Full Phase
  module. It is not descriptor-supplied invocation data or a separately mutable catalog. Each entry uses
  the existing stable semantic-point identity plus exact-PC representation and is verified against the
  module's pinned capability pack.
- Expanding that catalog is additive and does not invalidate or require re-indexing existing DTM
  itineraries. Validation passes over an armed checkpoint breakpoint when the DTM's ordered itinerary has
  no exact `(breakpoint PC, movie input cursor)` corollary for the hit.
- Complete-movie validation extends the existing canonical `ExecutionContinueUntil` action with an
  optional movie-cursor completion condition and a tagged breakpoint/cursor-overrun/movie-ended result;
  it does not introduce a parallel movie-specific continuation action.
- The TAS Movie module also supplies its exact movie-session handle to that action. Handle presence always
  enables termination when the owned playback session ends; an optional expected cursor additionally enables
  overrun detection. This is internal module wiring, not a job-configurable movie-end flag. Callers without a
  movie handle remain breakpoint-only.
- That canonical result contains only its reason, optional routed-stop receipt, current PC, movie input
  cursor, and immutable workset epoch. Movie frame, absolute VI, and raw movie state remain lower-level evidence.
- Simultaneous completion priority is routed breakpoint, then cursor overrun, then movie end. A movie ending
  above the expected cursor is therefore a desynchronization; one ending at or below it did not reach the
  expected terminal.
- A DTM's expected itinerary is supplied to the immutable module as a bounded typed list of
  `(breakpoint PC, movie input cursor)` entries, with an initial maximum of 4,096 entries.
- Establishment requires an empty itinerary and no output path. Validation requires a nonempty itinerary,
  every PC must belong to the embedded catalog, and cursors must increase strictly and remain within the
  source DTM. Violations fail preflight rather than returning domain `Invalid`.
- Checkpoint-catalog admission must establish that no two checkpoint breakpoints can occur on the same
  presented/player frame. Different cursors alone do not prove this because one player frame may contain
  multiple controller polls. Discovering an overlap requires reassessing both breakpoint choices; treatment
  of already-used artifacts is deferred until such a case exists.
- Expected validation mismatches are typed domain results so their factual evidence can be persisted.
  ProgramRuntime failure remains reserved for infrastructure, contract, cancellation, and backend
  failures.
- Validation worksets carry an exact `ReadOnlyMovie` artifact baseline: the DTM and, when required by the
  DTM, its exact startup savestate. The module uses `EstablishBaseline`; worker infrastructure boot is
  not a phase baseline and cannot satisfy the workset.
- `MoviePrepareReadOnlyPlayback` validates and stages the exact movie pair, reserves movie input, stops
  only Dolphin's guest core, and returns at its uninitialized boundary. `MovieStartPlayback` consumes that
  scoped preparation and remains the sole phase state-establishing action. Both preserve the wrapper,
  controllers, user directory, render surface, `EmulationSession`, and `WorksetEpoch`.
- The operation-specific checkpoint stop group is installed after the core stops and before
  `Movie::PlayInput` and `BootCore`, matching the proven DolphinQt debugger sequence.
- The entrypoint branches only to select the first-battle or global-catalog immutable stop configuration,
  rejoins to prepare and stop the movie core, subscribes once as an ordinary passive scoped group, starts
  playback once, and then enters the selected operation loop. Pre-stop ingress is drained before the
  subscription; post-boot validation proves the physical plan without force-reapplying it.
- Every validation workset contains exactly one job. Multiple long validations are parallelized as
  separate worksets that can run on different workers.
- Root-cursor establishment is a one-time bootstrap. It arms only the first-battle terminal breakpoint and
  returns that breakpoint's single `(PC, movie input cursor)` candidate pair; it does not traverse the
  global segment-boundary catalog.
- `VALIDATE` returns `Valid`, or `Invalid` with a closed reason (`MovieDesynchronized`,
  `ExpectedTerminalNotReached`, or `Unknown`), the next expected `(PC, cursor)`, and the actual PC and movie
  cursor at failure, plus an optional last-verified itinerary index. The worker internally proves each
  accepted checkpoint and does not return the matched checkpoint prefix, movie frame, or VI frame.
- The entrypoint returns one typed record with outcome `RootCursorEstablished`, `Valid`, or `Invalid`, an
  optional candidate pair present only for establishment, and optional failure diagnostics present only for
  invalidation. Other field combinations are rejected; no sentinel PC or cursor values are used.
- The only initial capture instruction is an optional caller-declared `final_checkpoint_output`, present only
  for root validation. Its presence requests capture; no redundant capture boolean is used. The initial
  validator has no diagnostic-capture output.
- The save action synchronously captures complete savestate and exact DTM-sidecar bytes while playback is
  paused, then transfers those bytes to the worker-owned output transaction. The module receives only the
  canonical pending receipt and may continue its movie-tail validation; it never owns the capture.
- A completed state capture is appended to `ProgramResult.artifacts` only after the tail succeeds and the
  worker finalizes and commits every staged output. `ProgramExecutionFinished` is not job completion, and
  no later item may begin before the authoritative `WorkerWorksetItemTerminal` is retained. The artifact
  reference is not duplicated in the typed TAS Movie result. A requested capture or finalization failure is
  an infrastructure/backend failure, returns no domain `Invalid`, and does not quarantine the movie.
- The movie session and selected stop group are released through normal scoped cleanup after any required
  root capture; the module does not add an explicit `MovieStopPlayback` path.
- Adding only checkpoint breakpoints changes the computed module and Full Phase hashes but does not manually
  increment program version or contract revision. Explicit versions change with contract, schema, or
  execution-semantics changes.
- The legacy TAS Movie runtime path, descriptor, and workflow aliases have been removed and replaced by the
  closed complete-validation surface; the removed implementation is not a compatibility constraint.
- The read-only legacy comparison repo's `DolphinWrapper::startMoviePlayback` remains lifecycle evidence:
  it stopped and rebooted the guest core on the same wrapper/system/window state rather than shutting down
  Dolphin. The current core-restart primitive preserves that ownership boundary without retaining the
  legacy PhaseScript path.
