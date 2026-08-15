# Complete-Movie Validation Implementation Record

Status: implemented design record. Current code and the TAS Movie living document are authoritative;
this record preserves the decisions that produced the initial validator.

This document is the working discussion surface for the first TAS Movie program kind. It retains the
existing `PK_TasMovie` identity and the previously agreed complete-DTM-validation purpose.

Only these implementation directions are currently approved:

- remove and replace the legacy TAS Movie runtime path rather than preserving it; and
- express the strictly moderated checkpoint-breakpoint catalog directly inside the immutable Full Phase
  module; and
- represent every module-owned checkpoint breakpoint using the existing stable semantic-point identity
  plus exact-PC contract, verified against the pinned capability pack. A raw-PC-only stop configuration is
  not needed;
- extend the existing canonical `ExecutionContinueUntil` contract for movie-cursor completion rather than
  introduce a parallel movie-specific continuation action;
- pass each DTM's expected itinerary to the module as a bounded typed list of `(breakpoint PC, movie input
  cursor)` entries; and
- return expected validation mismatches as typed domain results. ProgramRuntime failure remains reserved
  for infrastructure, contract, cancellation, and backend failures;
- explicitly opt this Complete Validation phase into an exact `ReadOnlyMovie` workset baseline because
  it intentionally begins at the DTM-declared origin; this is not a default for TAS Movie phases. Use
  `EstablishBaseline`, with module-owned
  `MoviePrepareReadOnlyPlayback` owning the core stop and `MovieStartPlayback` remaining the sole
  state-establishing action;
- install the operation-specific checkpoint stop group after the core reaches its uninitialized boundary
  and before the prepared movie is loaded and booted;
- implement that ordering by branching to select one of two immutable passive stop-group configurations,
  rejoining to prepare/stop once, subscribing once, and then consuming the preparation with
  `MovieStartPlayback` before entering the selected operation loop;
- place exactly one validation job in each workset so multiple long validations can be scheduled on
  different workers;
- treat root-cursor establishment as a one-time bootstrap that arms only the first-battle terminal
  breakpoint and returns its single `(PC, cursor)` candidate pair; and
- expose both operations through one Full Phase entrypoint that branches on the typed
  `ESTABLISH_ROOT_CURSOR` or `VALIDATE` operation value; and
- keep the module request limited to worker-execution facts, leaving database identities, artifact hashes,
  and game-identity verification with the descriptor;
- require establishment requests to have no itinerary or output path, and validation requests to have a
  nonempty itinerary whose PCs all belong to the embedded catalog;
- require expected itinerary cursors to increase strictly; equal-cursor checkpoint entries are rejected
  rather than tolerated;
- derive the validation terminal from the last expected itinerary entry rather than sending a redundant
  `required_final_breakpoint_pc`; and
- use one typed result record with a closed outcome and outcome-specific optional payloads; and
- represent the only initial capture instruction as an optional caller-declared final-checkpoint output
  path, absent during establishment and generated-DTM validation;
- publish successful state captures through the runtime's existing `ProgramResult.artifacts` collection
  rather than duplicating an artifact reference in the typed TAS Movie result;
- treat a caller-requested capture as mandatory: capture failure is an infrastructure/backend failure and
  does not produce a domain `Invalid` result or quarantine the movie;
- release the movie session and selected stop group through normal scoped cleanup, without an explicit
  `MovieStopPlayback` path;
- register `tasmovie.establish_root_cursor` and `tasmovie.validate` as two descriptor step kinds that both
  select the existing `PK_TasMovie` and its single branching entrypoint;
- make each materialized job reference an immutable Analysis DB validation-request record that snapshots the
  selected operation and exact movie, DTM, itinerary, Full Phase, and root-capture authority;
- make one explicit user command own one immutable validation request, business job, and sealed singleton
  workset; coordinator recovery reuses those exact rows, while a later explicit command creates new rows;
- finalize every typed TAS Movie domain result, including `Invalid`, as business job `SUCCEEDED`; reserve job
  `FAILED` for worker/execution failures that may be explicitly requeued;
- place validation in a workflow containing exactly one singleton step and no possible next step;
- perform no automatic retry after a terminal worker/execution failure; alert the user, who may explicitly
  requeue the same immutable job;
- write the Analysis DB validation-attempt ledger only for typed domain results, never for infrastructure,
  preflight, cancellation, or contract failures;
- bound the expected itinerary at 4,096 `(PC, cursor)` entries; and
- let checkpoint-catalog-only additions change computed module/Full Phase hashes without manually bumping
  program version or contract revision; and
- keep `VALIDATE` output minimal: `Valid` on success, or `Invalid` plus failure diagnostics. Proof of each
  accepted checkpoint remains internal to the worker and is not returned as a matched-prefix list.

Legacy `PlayTasMovie`, legacy payload/context keys, and legacy TAS descriptor behavior are evidence only.
There is no compatibility requirement.

The sections below record the implemented module control flow, workset shape, descriptor responsibilities,
persistence model, workflow surface, business-job mapping, and retry policy. Later changes remain subject
to collaborative review.

## Settled program responsibility

`PK_TasMovie` has two explicitly requested operations:

- `ESTABLISH_ROOT_CURSOR`: replay the root DTM with only the first-battle terminal breakpoint armed and
  publish that breakpoint's single `(PC, cursor)` candidate pair. This is a one-time root bootstrap and
  never publishes a trusted route checkpoint.
- `VALIDATE`: replay a DTM against its immutable ordered checkpoint itinerary. A successful exact root
  validation may publish the canonical root checkpoint. Generated-DTM validations publish evidence only.

This program does not record or mutate a movie, diagnose a desync, run SeedProbe, advance dialogs, or
continue automatically into another job.

## Current code observations

The current refactor already supplies most of the required mechanics:

- `FullPhaseProgramRegistry` and `IFullPhaseProgramDefinition` provide the native program-definition
  boundary used by SeedProbe.
- `ProgramKindDescriptor` provides the production materializer, workset-reconstruction, and result-handler
  contracts.
- `MovieStartPlayback` starts read-only DTM playback and performs Dolphin's required movie-aware reboot.
- the shared `SPS1` codec describes the allowed semantic endpoints and
  `ExecutionContinueUntil` installs its own operation-scoped foreground wait;
  passive capture/progress observes the same routed identity without owning
  playback.
- `ExecutionEngine` already polls `BackendExecutionSnapshot` during maintenance, and that snapshot already
  contains PC, VI, movie state, and `movie_input_count`.
- `StateSaveImmutableArtifact` captures an idle-paused state while movie playback remains active. The state
  finalizer already publishes the `.sav` and its exact same-name `.dtm` sidecar together.

One observed gap is that ordinary `ExecutionContinueUntil` returns only a routed breakpoint receipt.
It cannot complete when the movie input cursor advances past an expected value, and its public result does
not expose the cursor, VI, or movie state from the terminal execution snapshot.

### Accepted `ExecutionContinueUntil` extension

Extend the existing canonical `ExecutionContinueUntil` action. Do not add a parallel movie-specific
continuation action.

Its request contains:

- the exact passive stop-group handle;
- an optional exact movie-session handle; when present, termination of that owned playback session is always
  a completion condition;
- an optional expected movie-input cursor whose strict overrun (`current > expected`) is a completion
  condition;
- the existing current-point, interruption, throttle, cancellation, and immediate-reentry policies.

Its successful typed result is a closed observation record:

- reason: `BREAKPOINT`, `CURSOR_OVERRUN`, or `MOVIE_ENDED`;
- optional exact routed-stop receipt;
- current PC;
- movie input cursor;
- state epoch.

This remains an execution mechanism, not TAS validation policy. `ExecutionEngine` should evaluate cursor overrun
from the snapshots it already requests during its maintenance pump. The Full Phase module decides whether
a breakpoint/cursor observation passes, is tolerated, or fails the itinerary.

Movie-end termination is structural module behavior, not a workset/job option. Both TAS Movie operations
pass the exact movie-session handle returned by `MovieStartPlayback`. Establishment omits the cursor ceiling;
validation supplies the next expected cursor. A caller with no movie-session handle retains ordinary
breakpoint-only continuation behavior.

The canonical result schema must therefore evolve from an unconditional routed-stop receipt into a tagged
result capable of representing a routed breakpoint, cursor overrun, or movie end. Movie frame, absolute VI,
and the raw movie-state enum remain lower-level snapshot/trace facts and are not part of this canonical
result.

The execution ordering must give an already-routed breakpoint completion priority over a cursor-overrun
check during pause confirmation, and cursor overrun has priority over movie end. Thus a movie ending above
the expected cursor reports `CURSOR_OVERRUN`; one ending at or below it reports `MOVIE_ENDED`. The module
then evaluates a breakpoint's observed cursor, including an observation that is already greater than the
expectation.

## Typed Full Phase contract

Suggested native location:

- `SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.h`
- `SavorCore/Phases/Programs/TasMovieValidation/TasMovieValidationModule.cpp`

Suggested identities:

- module canonical ID: `soa.tas_movie_validation`
- entrypoint: `validate`
- Full Phase canonical ID: `savor.full_phase.tas_movie_validation`
- program kind: existing `PK_TasMovie`
- initial native program version: a new version owned only by this implementation

The single entrypoint accepts a typed operation discriminator and branches into the one-time
`ESTABLISH_ROOT_CURSOR` path or the ordinary `VALIDATE` path.

### Invocation policy

- state policy: `EstablishBaseline`
- execution intent: `Replay`
- movie playback: allowed
- movie recording: forbidden
- ordinary input: forbidden
- capture: allowed because exact root `VALIDATE` may save the final checkpoint
- guest-core stop: allowed only through `MoviePrepareReadOnlyPlayback`
- movie load and guest-core start: allowed only through `MovieStartPlayback`
- dialog/progression services: absent

The workset stages and verifies its exact `ReadOnlyMovie` DTM/startup-savestate artifact pair without
starting playback. `MoviePrepareReadOnlyPlayback` stops the guest core and drains its old ingress;
the module then installs its passive scoped stop subscription before `MovieStartPlayback` loads the DTM
and boots the guest core. Post-boot validation proves the physical plan without force-reapplying it or
changing `WorksetEpoch`. The worker's earlier plain
infrastructure boot is not observable phase state and is never a workset baseline.

### Request proposal

The workset reconstruction adapter should produce a typed request containing:

- operation: `ESTABLISH_ROOT_CURSOR` or `VALIDATE`;
- materialized DTM path;
- expected itinerary entries `(breakpoint PC, movie input cursor)` for `VALIDATE`;
- optional caller-declared `final_checkpoint_output`, present only for root `VALIDATE`.

The checkpoint catalog is not request data: it is compiled into the Full Phase module. The expected
itinerary is request data and is passed as a bounded typed list. The validation terminal is the final
itinerary entry; it is not duplicated as a separate request field. TAS movie IDs, artifact IDs and hashes,
and game/disc identity remain descriptor/coordinator facts and do not cross into the module request.
Artifact format, persistence ownership, and the descriptor's reconstruction steps remain undecided.

The itinerary list has a fixed maximum of 4,096 entries. `final_checkpoint_output` must be absent for
`ESTABLISH_ROOT_CURSOR` and generated-DTM validation; its presence is the root-capture instruction, so the
request does not carry a redundant capture boolean. The initial validator has no diagnostic-capture output.

Request decoding/preflight enforces:

- establishment has an empty itinerary and no final-checkpoint output;
- validation has 1..4,096 entries;
- every validation PC belongs to the module's embedded checkpoint catalog;
- movie input cursors increase strictly and stay within the source DTM's available input records; and
- only validation may carry a final-checkpoint output. The descriptor, not the worker request, proves that
  only the root DTM receives that output.

Malformed or catalog-incompatible input is a contract/materialization failure before playback, not a domain
`Invalid` result.

### Result

Return one typed domain result for both validation success and expected validation failures. `VALIDATE`
returns only `Valid`, or `Invalid` with:

- a closed failure reason: `MovieDesynchronized`, `ExpectedTerminalNotReached`, or `Unknown`;
- the next expected `(breakpoint PC, movie input cursor)` pair; and
- the actual PC and movie input cursor observed at failure; and
- an optional `last_verified_itinerary_index` identifying the last checkpoint the worker proved correct.

It does not return the matched checkpoint prefix, movie frame, or VI frame; proving every accepted
checkpoint is internal module behavior. A desync, cursor overrun, or premature movie end must not be
converted into a ProgramRuntime structured failure. Establishment success returns the single candidate
`(first-battle breakpoint PC, movie input cursor)` pair rather than a partial or multi-entry itinerary.

The entrypoint's single declared output type is a record containing:

- a closed outcome: `RootCursorEstablished`, `Valid`, or `Invalid`;
- an optional candidate `(PC, cursor)`, present only for `RootCursorEstablished`; and
- optional failure diagnostics, present only for `Invalid`.

The module and host decoder reject every other field-presence combination rather than using sentinel PC or
cursor values.

State captures are not duplicated in this typed record. `StateSaveImmutableArtifact` synchronously
serializes the complete savestate and exact DTM sidecar while paused, but its program-facing result remains
pending. `WorkerRuntime` adopts the actor-only staged output, lets the module complete its tail validation,
and appends the authoritative reference to `ProgramResult.artifacts` only after the execution draft is
clean and every output finalizer has committed. The finalized workset-item terminal is the sole job
completion boundary. The active movie's materialized source `dtm_path` is provenance only; finalization
always writes the captured DTM bytes to the requested checkpoint's canonical `<sav path>.dtm` companion.

Infrastructure, artifact preflight, cancellation, runtime-contract, and worker-health failures remain
execution failures. They do not fabricate a domain validation result or a quarantined child.

## Module algorithm

### Common setup

1. Switch on the operation to select either the first-battle-only or complete-catalog immutable
   `StopGroupStaticConfig`.
2. Rejoin with that common config value, prepare the exact DTM, and stop the guest core.
3. Drain pre-stop ingress and subscribe the selected passive scoped group once while the core is
   uninitialized.
4. Consume the preparation to start exact read-only playback and validate the retained physical plan.
5. Switch into the operation-specific execution loop while retaining the movie-session and stop-group
   handles.
6. Never acquire an input lease and never enable the dialog advancer.

The stopped-core boundary advances dispatch/physical generations and drains old ingress before the group
exists. Boot-time JIT notifications and an explicit post-start check validate the newly installed physical
plan without force-reapplying it. Abandoning an unconsumed preparation taints and retires the worker.

### `ESTABLISH_ROOT_CURSOR`

1. Use the selected passive stop group containing only the first-battle terminal breakpoint.
2. Keep the movie-session and stop-group handles scoped through the operation.
3. Call the extended `ExecutionContinueUntil` with no cursor expectation.
4. On that breakpoint, read the movie cursor and return the single candidate `(PC, cursor)` pair without
   capturing a savestate.
5. If the movie ends before that breakpoint, return the typed expected-terminal failure.

The result handler canonicalizes that pair into the root's candidate itinerary artifact. The root remains
unavailable for route construction until a later exact `VALIDATE` run succeeds. Establishment does not arm
the global checkpoint catalog, collect intermediate hits, or need a repeated-hit policy.

### `VALIDATE`

Use the selected passive stop group containing every checkpoint PC embedded in the module and keep its
handle scoped through optional state capture.

For each next expected entry `(P, E)`:

1. Call the extended `ExecutionContinueUntil` with cursor expectation `E` and the module's complete
   checkpoint stop group.
2. If the action reports `CURSOR_OVERRUN`, fail at the current expected entry.
3. If it reports `MOVIE_ENDED`, fail because the expected checkpoint was not reached.
4. If it reports breakpoint `(Q, C)`:
   - `C < E`: record diagnostic observation if desired, then resume without advancing the itinerary;
   - `C == E && Q == P`: accept the entry and advance to the next one;
   - `C == E && Q != P`: resume without advancing the itinerary;
   - `C > E`: fail as a cursor overrun/desynchronization.
5. Accept the movie only after the last itinerary entry passes. The last entry must be the required final
   PC.

On successful exact root validation only:

1. leave read-only playback active at the accepted final breakpoint;
2. invoke `StateSaveImmutableArtifact` while execution is idle-paused;
3. publish the finalized state through the runtime's existing `ProgramResult.artifacts` path;
4. allow normal scoped cleanup to stop playback only after capture.

If the request includes a capture output, that capture is mandatory. A `StateSaveImmutableArtifact` failure
is an infrastructure/backend failure, produces no domain `Invalid`, and does not quarantine the movie; the
normal execution retry policy may apply. The earlier proposal to capture a diagnostic state at every coarse
failure point has been rejected for the initial validator: that state may be later than the actual
divergence. Instead, `Invalid` identifies the last verified itinerary index. The result processor resolves
that segment-local index through the immutable itinerary and database hierarchy and durably references the
corresponding known-good checkpoint. If no itinerary entry passed, the index is absent and investigation
begins from the root DTM at boot. Detection-point capture remains a possible future diagnostic operation,
whose design is deferred.

## Global segment-boundary catalog

Accepted direction: checkpoint breakpoints are defined directly by the immutable Full Phase module. They
are not supplied by the descriptor, copied into an invocation, loaded from a DB table, or owned by a
separate mutable catalog service. The compiled module therefore contains the exact moderated breakpoint
set that it arms, and the module's existing immutable identity covers that set.

The C++ module definition will author an explicit, reviewable set of stable semantic-point identities and
exact PCs and serialize it into the module's constant `StopGroupStaticConfig`. Runtime decoding must reject
an identity/PC pair that does not exactly match the pinned capability pack. Nothing automatically imports
all points from `BpRegistry`, so catalog membership remains controlled by the TAS Movie module.

Catalog moderation must establish before use that two checkpoint breakpoints cannot occur on the same
presented/player frame. This is stronger than merely requiring different PCs or movie input cursors, because
a player frame may contain more than one controller poll. The itinerary therefore requires strictly
increasing cursors, but catalog admission still needs evidence about player-frame separation. If an already
used breakpoint is later found to violate this invariant, both breakpoint choices require reassessment; the
legacy-artifact policy is deferred until such a case actually occurs.

Catalog additions do not invalidate or require re-indexing existing movie itineraries. The module arms the
expanded checkpoint set, but validation obligations still come only from the DTM's ordered itinerary. A
breakpoint hit with no exact `(breakpoint PC, movie input cursor)` corollary in that itinerary is resumed
and passed over. The expanded module identity is retained as run evidence, not as a reason to make the
existing itinerary stale.

The one-time root-establishment operation is deliberately narrower: it arms only the first-battle terminal
breakpoint and therefore does not traverse this catalog. A catalog-only addition changes the computed
module hash and Full Phase canonical hash, recording the exact catalog used, but does not manually increment
the program version or contract revision. Those explicit versions change only with contract, schema, or
execution-semantics changes.

Current source evidence: SeedProbe defines a `constexpr` semantic-point array inside its module constructor
and serializes each stable semantic-point identity plus exact PC into a constant
`StopGroupStaticConfig` value. That constant is part of the compiled `ProgramModule` and therefore part of
the immutable module hash. TAS Movie will use that implemented representation without introducing another
catalog owner.

## Persistence model

The current `state_tas_movie_variant` schema encodes legacy RTC/bookmark/neutral-frame mutations and does
not represent a complete DTM, checkpoint itinerary, validation lifecycle, or quarantine cleanly. Since no
compatibility is required, replace that API and migrate the table to a purpose-fit movie aggregate instead
of adding more nullable legacy columns.

### State DB aggregate

Proposed `state_tas_movie` fields:

- `tas_movie_id`
- `name`
- `dtm_artifact_id`
- optional `parent_tas_movie_id`
- `required_final_breakpoint_pc`
- optional `candidate_itinerary_artifact_id`
- optional `validated_itinerary_artifact_id`
- optional `checkpoint_savestate_id`
- lifecycle: `UNESTABLISHED`, `CANDIDATE`, or `VALIDATED`
- creation/update timestamps

Extend `state_artifact.artifact_kind` with an exact itinerary kind rather than hiding it under `OTHER`.
Use a small canonical binary format (for example `TMI1`) containing version, ordered `(PC, cursor)` entries,
and no redundant VI/movie-frame facts. Artifact SHA-256 is the itinerary identity.

The cursor field is a DTM input count, not a byte offset. For the supported GameCube DTM input-record type,
each record has the specification-defined fixed length of 8 bytes; any payload/file byte position is
derived from the count and record type. The codec must not persist a second independently authoritative
byte-offset anchor. Count `N` is the stream boundary after `N` complete records and before zero-based record
`N`; Dolphin increments the public count only after supplying or recording that record.

The itinerary is one of several separate immutable typed worker artifacts bound to the same exact DTM. It
is not the legacy generic `DTMINI` bookmark document and does not absorb semantic-observation or
presented-frame-index data. Manual bookmarks remain a separately mutable user-facing document and are
never materialized as worker input or accepted as validation evidence.

The DTM remains a separate content-addressed artifact. A root checkpoint's same-name sidecar must hash to
that exact DTM artifact. The movie aggregate links the checkpoint savestate and DTM; later workset staging
can materialize them as the required same-name pair.

### Analysis DB validation ledger

Before dispatch, materialization creates or resolves an immutable validation-request record containing the
operation, source TAS movie ID, exact DTM artifact/hash, exact itinerary artifact/hash when applicable, exact
Full Phase identity, and root-checkpoint-capture authority. The execution job references this request rather
than a mutable TAS movie row. Workset reconstruction rejects any disagreement with that snapshot.

One explicit user command owns one immutable validation request, one immutable business job, and one sealed
singleton workset. Re-entering materialization for the same workflow activation after interruption resolves
those exact rows. A later explicit command creates a new immutable request/job/workset even when it targets
the same DTM and itinerary. Infrastructure execution-attempt records may accumulate beneath the immutable
job without changing its definition.

Add an append-only validation-attempt table keyed idempotently by source execution job/terminal hash. Store:

- TAS movie ID, DTM artifact ID/hash, operation, and exact Full Phase module identity;
- expected/candidate itinerary artifact ID/hash;
- outcome and failure classification;
- expected and reached/last PC/cursor evidence;
- final movie cursor/frame/VI and elapsed statistics;
- optional diagnostic savestate ID and canonical root checkpoint savestate ID;
- timestamps and worker-terminal provenance.

Only typed `RootCursorEstablished`, `Valid`, and `Invalid` domain results create rows in this ledger.
Infrastructure, artifact preflight, cancellation, and contract failures remain durable Execution DB evidence
and do not change movie eligibility or create a validation-attempt row.

Add a current eligibility projection keyed by TAS movie ID and exact DTM hash. A failed `VALIDATE` changes
that exact hash to `QUARANTINED`; a later successful `VALIDATE` of the same hash changes it to `ELIGIBLE`.
The append-only attempt history is never deleted. Establishment leaves the root `UNESTABLISHED`/`CANDIDATE`
rather than making it eligible.

Other route-producing descriptors must eventually consult this projection before using a movie as a
parent. The validation descriptor itself must remain able to run a quarantined movie so it can be
revalidated.

## Production descriptor

Suggested location:

- `SavorDb/Execution/ProgramDB/TasMovieValidation/`

The existing legacy `TasMovie/` adapter and registration should be removed or replaced, not registered in
parallel.

### Job materializer

- accept exactly `tasmovie.establish_root_cursor` and `tasmovie.validate`, both registered to `PK_TasMovie`
  and its single Full Phase entrypoint;
- require one workflow input binding with `ref_kind = state.tas_movie` and the exact movie aggregate ID;
- derive the closed operation from the registered step kind, not from a free-form activation argument or
  the movie's current lifecycle;
- permit establishment only for the root while it has no candidate cursor. A failed establishment attempt
  does not close that opportunity, but a successfully established candidate does;
- create or resolve the immutable Analysis DB validation-request snapshot and use it as the execution job's
  domain reference;
- create exactly one validation job in one sealed singleton workset. Parallel validation is achieved by
  dispatching multiple singleton worksets to different workers, never by grouping long validations;
- derive a deterministic fingerprint from the immutable validation request and exact Full Phase identity;
- never schedule generated-DTM validation automatically;
- require validation to be the only singleton step in its workflow and expose no next-step continuation.

### Workset reconstruction

- re-read the exact movie and operation referenced by the durable job;
- resolve and verify the DTM artifact, then materialize it under the workset root;
- parse the DTM header and verify the supported Skies of Arcadia Legends US identity;
- decode and verify the exact candidate/validated `TMI1` artifact when required;
- rely on the checkpoint set embedded in the exact Full Phase module rather than reconstructing external
  catalog input;
- construct the phase's explicitly selected `ReadOnlyMovie` workset baseline containing the exact DTM
  and only its DTM-declared optional startup savestate, with complete compatibility and lineage, plus an
  `EstablishBaseline` invocation;
- require the scalar TAS Movie request and the workset baseline to identify the same materialized DTM and
  startup savestate;
- declare deterministic state/diagnostic output paths beneath the workset artifact root;
- reject any durable identity drift rather than silently rebuilding a different job.

### Result handler and recovery

- decode and verify the durable worker terminal and typed module result;
- ensure all returned DTM/itinerary identities and the worker's exact Full Phase identity match the durable
  job;
- persist the validation attempt idempotently;
- for establishment success, canonicalize/store the candidate itinerary and move the root to `CANDIDATE`;
- for exact root validation success, import the finalized `.sav`, verify its `.dtm` sidecar hash equals the
  source DTM hash, create the canonical checkpoint, attach it to the root movie, publish the validated
  itinerary, and make that exact hash eligible;
- for generated validation success, record evidence and make that exact hash eligible without creating a
  new checkpoint;
- for validation failure, retain any diagnostic state, append the attempt, and quarantine only the exact
  DTM hash;
- implement `RecoverPersistedOutcome` so an interruption after domain persistence cannot duplicate the
  validation attempt, itinerary, or checkpoint.

Every persisted typed domain result finishes the business job as `SUCCEEDED`, including `Invalid`.
`Invalid` means the worker successfully completed validation and proved the movie unusable; the durable
quarantine carries that domain meaning, and the succeeded job is not casually requeueable. Job `FAILED` is
reserved for worker/execution failures. No terminal failure is retried automatically: the user is alerted
and may explicitly requeue that same immutable job, creating another execution attempt beneath it.
Infrastructure failures do not quarantine the movie or create Analysis DB validation-attempt rows.

Validation is authored as a workflow containing exactly one singleton step and no next step, so a succeeded
domain result never continues into another phase.

## Implementation slices

### Slice 1: domain types and pure codecs

- after the related contracts are approved, define modes, outcomes, request/result types, marker
  validation, and any itinerary codec;
- author the approved checkpoint set as part of module construction and test that the compiled module
  contains exactly that set.

### Slice 2: extend `ExecutionContinueUntil`

- extend the existing request policy, tagged result schema, host dispatch, execution predicate, and
  terminal-result encoding;
- add scripted-backend tests for breakpoint below/equal/above expectation, wrong PC at equality, cursor
  overrun without a stop, movie end, cancellation, and the breakpoint-versus-overrun pause race;
- confirm SeedProbe retains its existing stop-driven semantics through the extended contract.

### Slice 3: Full Phase module

- implement typed module construction, verification, envelope, invocation compatibility, and registry entry;
- test single-breakpoint root establishment, validation tolerance rules, terminal enforcement, root-only
  capture, minimal `Valid`/`Invalid` results, bounded list/budget behavior, and cleanup with playback still
  active through state capture.

### Slice 4: persistence replacement

- migrate the legacy State TAS schema/API to the complete-movie aggregate and itinerary artifacts;
- add Analysis validation-attempt and eligibility persistence with idempotent commands;
- update queued DB wrappers, outbox payload resolution, archive/export coverage, and DB tests.

### Slice 5: production descriptor and workflow surface

- implement singleton materialization, reconstruction, result handling, and recovery;
- register `PK_TasMovie` and `tasmovie.validate` in `ProductionProgramKindRegistry`;
- register the Full Phase definition in `FullPhaseProgramRegistry::ProductionRegistry()`;
- replace the legacy workflow unit's inconsistent `tasmovie.play`/`tas_movie` step names with the exact new
  step kind, a required `state.tas_movie` input, the explicit operation argument, and validation-oriented
  outputs;
- remove the legacy PhaseScript/payload/descriptor registrations and rewrite affected tests.

### Slice 6: first live validation gate

1. supply the initial boundary catalog and root final PC;
2. implement and run the validator far enough to reproduce the known root movie desync;
3. stop and investigate/fix that desync as its own approved task;
4. explicitly run `ESTABLISH_ROOT_CURSOR` only after the desync is believed fixed;
5. inspect the candidate itinerary;
6. run an exact second `VALIDATE`;
7. accept the root only if it reaches every expected `(PC, cursor)` marker and publishes the movie-active
   canonical checkpoint with the exact root DTM sidecar.

Round 1 work remains gated on this live result.

## Verification bar

- build `SAVOR.sln` with the configured VS 18/v145 MSVC toolchain;
- run focused runtime, module, descriptor, DB migration, result-recovery, and production-registry tests;
- run the relevant complete test binary after focused tests pass;
- inspect the persisted State, Analysis, Execution, and artifact rows from the live root attempts;
- verify the published root `.sav` and same-name `.dtm` exist and that the sidecar SHA-256 is the exact root
  DTM hash;
- load the published checkpoint with its sidecar in Dolphin/SAVOR and confirm the serialized cursor is at
  the accepted final breakpoint and before the remaining root guard/tail input.

## Remaining implementation-time choices

Only one policy value remains genuinely unsettled: the exact initial BP keys in
`TasMovieBoundaryCatalog`, including the authored root final PC. Terminal worker/execution failures have
no automatic retry and await an explicit user requeue decision.

Neither changes the architecture above. Diagnostic-mode behavior remains explicitly deferred.
