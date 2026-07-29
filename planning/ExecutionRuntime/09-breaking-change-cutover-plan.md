# 09 - Breaking-Change Cutover Plan

## Scope

This document describes dependency slices, production cutover, and legacy deletion required by the
Execution Runtime refactor. Breaking changes are expected. The slices are an
adaptable implementation order, not approval gates, release numbers, or calendar commitments.

Repository code defines the current system. Documents 02 through 08 provide the target constraints and
design direction; implementation may adapt their concrete shape while preserving the core boundaries.

## Purpose and non-goals

The plan replaces the current worker execution path with one universal typed runtime without leaving a
permanent second controller behind. It is ordered to establish ownership and test seams before migrating
domain behavior.

This plan does not:

- require backward-compatible worker-internal C++ APIs or worker messages;
- change SavorDb SQL/schema or migrations, durable queue/claim lifecycle, workflow persistence,
  result-projection transaction boundaries, or artifact-storage interfaces. The pre-6A hard cutover
  intentionally removes obsolete timing fields from public authoring interfaces and newly generated
  arguments; private neutral insert shims satisfy the unchanged physical columns. Ordered batch claim,
  exact-set lease renewal, claim/start validation, and targeted terminal reconciliation are the other
  database-interface exceptions;
- rewrite existing rows. Program-kind handlers consume recognized semantic payload/result fields but do
  not reinterpret obsolete timing keys as runtime policy;
- migrate a live `PhaseScriptVM` instruction pointer or live Dolphin session;
- discard or re-author existing queued jobs;
- define exact pull-request boundaries or dates; or
- implement Navmesh Survey before the common foundation and current-phase parity exist.

## Current code evidence

The current replacement surface is concrete:

- `SavorWorker/SavorWorker.cpp` constructs `DolphinWrapper` and `PhaseScriptVM`, selects programs by
  numeric `ProgramKind`, decodes program-specific payloads, runs the VM, and permits visual-control code
  to call emulator methods from another thread.
- `SavorCore/Runner/Script/PhaseScriptVM.*` owns program flow, direct Dolphin access, breakpoint scopes,
  input publication, one snapshot, capture lifecycle, and the separate input-macro runtime.
- `SavorCore/Phases/Programs/ProgramRegistry.cpp` switches on wire program kind for program construction,
  payload decoding, and retry metadata.
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc` mixes core language operations with emulator,
  capture, TAS, battle, macro, savestate, and Navigation Context operations.
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h` combines queue persistence, runtime
  initialization, result mapping, and workflow transition handling under program kind.
- Current workflow services already provide transactional lifecycle, dynamic step insertion, terminal
  advancement, output routing, retries, recovery, and restart tests. Those behaviors are migration
  assets, not rewrite targets.
- The router analysis in
  `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router` supplies a staged extraction
  order for session ownership, physical stop points, routing, execution, and input arbitration.

The analysis's final “shrink PhaseScriptVM” and separate `InputMacroEngine` target is superseded. This
plan replaces both with `ProgramRuntime` and one `ProgramExecutor`.

## Current target decisions

### Global cutover rules

1. There is exactly one production interpreter at any moment.
2. Every supported phase is authored directly as a native typed module. Existing `PhaseScript` builders,
   providers, tests, and opcodes are orientation and behavioral evidence, not compiler inputs or
   executable comparison targets.
3. There is no temporary PhaseScript translator, compatibility executor, mandatory legacy differential
   harness, or per-job selection between old and new execution.
4. New features, including Navmesh Survey, are prohibited in the legacy execution code.
5. An active `ProgramInstance`, resource scope, `StateEpoch`, Dolphin session, or in-flight worker command
   is never migrated across runtime versions.
6. Existing persisted rows are not rewritten. Program-kind handlers translate recognized semantic
   payload/result fields at the runtime boundary; obsolete timing keys are ignored and are not rewritten
   as stored `ProgramModule` or `ProgramInvocation` records.
7. Coordinator, worker protocol, module catalog, action/type registries, and runtime-facing program-kind
   adapters deploy as one compatibility-checked application release set.
8. Rollback means stopping the release and restoring the matched application build. No database rollback
   or schema state is part of this refactor, and rollback never enables an old interpreter inside the new
   worker.
9. SavorDb migrations, physical schema, durable queue/claim lifecycle, workflow persistence,
   result-projection transaction boundaries, and artifact contracts remain unchanged throughout the
   cutover. Public authoring interfaces lose obsolete timing fields while private neutral insert shims
   satisfy the unchanged columns. Other database interfaces may change only for ordered batch claim,
   exact-set lease renewal, claim/start validation, and targeted terminal reconciliation; coordinator
   grouping, capacity accounting, staging, and acknowledgements remain transient.
10. `SubmitWorkset` is the sole production program-dispatch path. A `WorkerWorkset` is a bounded,
    transient, ordered envelope over one or more independently durable jobs; it is not a persisted
    aggregate, a workflow step, or a worker-owned scheduler.

### Completed prelude: shared production program composition

Before runtime extraction, the current DB-facing program catalog was centralized in
`SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.*`. This prelude is complete and establishes:

- one fixed full-catalog registration order: TAS Movie, SeedProbe, Battle Context, Battle Single Turn,
  Battle End, then Navigation Context;
- atomic construction plus validation of canonical numeric winners, all sixteen production step kinds,
  and required workflow descriptor capabilities;
- the same complete catalog in SavorQt and every DB-backed SavorE2E scenario, with scenario behavior
  retained through config overrides;
- one shared SavorE2E `DBService` guarded by quiescent workflow boundaries before and after each
  DB-backed scenario and repeat; and
- no SavorDb descriptor or workflow step kind for the direct-worker `battle_macro_probe` scenario.

The prelude adds no schema migration, stored-representation change, database interface, queue/claim
change, coordinator filter, or new project. Focused partial registries remain valid only for
development tests and specialized tools such as SavorPredict.

### Just-in-time characterization and contract definition

The validated Release solution build and shared SavorE2E production-composition run are the starting
baseline. Characterization and contract work proceeds with the implementation slice that needs it rather
than as a separate all-system phase.

Before changing a runtime seam:

- re-read the affected current code and identify the programs and observable behavior that cross it;
- preserve representative payload, result, artifact, and live-fixture parity for those affected programs;
- define the logical contract fields and ownership rules needed by that slice, consistent with documents
  03 through 06 and the unchanged workflow-integration boundary;
- add the deterministic fake-backend events and traces needed to test that slice; and
- resolve any ambiguity that would change single-owner, cleanup, typed-runtime, no-dual-runtime, or fixed
  SavorDb constraints before implementing it.

This does not require freezing every future C++ declaration or completing every phase fixture before the
first `WorkerRuntime` change.

**Stable current module identities:**

| Current phase family | Target module/entrypoint |
|---|---|
| SeedProbe | `soa.seed_probe/probe` |
| Navigation Context | `soa.navigation.context/capture` |
| TAS playback/checkpoint | `soa.tas_movie/play_and_checkpoint` |
| TAS frame detector | `soa.tas_frame_detector/detect` |
| Battle Context | `soa.battle.context/capture` |
| BattleMacroProbe | `soa.battle.macro_probe/probe` |
| BattleSingleTurn | `soa.battle.single_turn/execute` |
| Battle completion | `soa.battle.completion/complete` |
| Battle results screen | `soa.battle.results_screen/advance` |

**Completion checks:**

- the affected seam has focused characterization for its inputs, outputs, side effects, and cleanup;
- the slice's contract and ownership rules are executable in focused tests or a fake-session fixture; and
- unresolved details are deferred only when they cannot change the current slice's architecture.

### Dependency slice 1: WorkerRuntime and EmulationSession seam

**Implement:**

- separate process/transport concerns from one serialized `WorkerRuntime` command actor;
- make `WorkerRuntime` own exactly one explicit `EmulationSession`;
- replace production worker traffic with little-endian protocol-version-1 frames whose header is the
  four-byte `WRMS` magic, 16-bit protocol version, 16-bit message kind, 32-bit payload length, and 64-bit
  request ID, with payloads rejected above 64 MiB before allocation;
- use one reader to decode frames into typed actor commands and one outbound publisher for command
  results, progress, host events, and terminal events; callbacks only enqueue events;
- route cancellation, screenshots, session lifecycle, and encoded module/invocation envelopes through
  that actor;
- narrow `DolphinBackend` to backend capabilities rather than policy; and
- introduce typed command/result, cancellation-token, session-disposition, and `StateEpoch` primitives.

Slice 1 is a hard production disconnect. `SavorWorker` no longer constructs, includes, or calls
`PhaseScriptVM`, and the old VM is not placed behind the new seam as a compatibility executor. Its source
and focused behavior tests remain orientation evidence for direct native reconstruction, but no
production worker command reaches it. The old worker tags remain disconnected and fail locally rather
than emitting legacy frames.

The Slice 1 worker advertises session lifecycle, screenshot, host-event, cancellation-protocol, and
shutdown capabilities, but not `ProgramInvocation` or interactive visual debugging. Pause, resume, and
frame-step requests fail as unsupported until `ExecutionEngine` owns them in slice 3. Guest-instruction
stepping is never promoted into the target control surface. Ordinary visual rendering and host-event
publication may continue through the standard session and protocol paths.

Until `ProgramRuntime`, current programs, and handler adapters complete their later slices, worker-backed
execution is intentionally unavailable. Callers must fail capability preflight before starting DB-facing
work or creating scenario records; an unavailable worker scenario is neither skipped nor passed. This
temporary hard-cutover interval changes no SavorDb schema, storage, interface, queue, claim, workflow, or
transaction contract.

**Completion checks:**

- no pipe-reader, visual-control, telemetry, or background thread directly mutates Dolphin or VM state;
- malformed, truncated, oversized, wrong-magic, wrong-version, and unknown-kind `WRMS` frames fail without
  session mutation;
- command serialization and cancellation races pass deterministic concurrency tests;
- boot, shutdown, error, and cancellation leave a known session disposition; and
- the full solution compiles and focused ownership, session, protocol, process, and capability-gate guards
  pass even though production-worker SavorE2E is not yet runnable.

### Dependency slice 2: single physical stop-point ownership and routing (completed)

Slice 2 is complete as an architectural checkpoint. The current C++ names may still evolve with later
slices, but the ownership and control boundaries established here remain the guide:

- each open `EmulationSession` owns exactly one `PhysicalStopPointManager` and one `StopPointRouter`;
- `PhysicalStopPointManager` is the sole owner of regular Dolphin PC breakpoints and memchecks, and its
  physical plan is the union of the router's logical subscription groups;
- logical subscriptions carry source identity, delivery mode, priority, lifetime, routing policy,
  bounded hit-time behavior, and `StateEpoch` policy without allowing one source to clear another;
- MinHook remains an implementation detail in SavorProbe, but its JIT and post-write memcheck detours
  publish only through the dependency-neutral `INativeStopSink` boundary rather than a
  `ProbeRuntime` singleton;
- retained capture-profile processing is passive behind a router adapter: it may observe the shared hit
  identity, sample, record, and request physical reconciliation, but it does not own Dolphin sites or
  pause, resume, step, or schedule execution; and
- boot, state replacement, restore rollback, and JIT revalidation preserve one session-owned routing
  boundary and reject stale-epoch delivery.

`RequestInterruptionHandler` is intentionally present as a typed routing outcome and
`StopInterruptionHandlerRequest`, not as a second controller. The router identifies a verifier-known
handler but does not execute it. Suspending the foreground operation, executing the bounded handler, and
resuming or terminating the parent belongs to the Slice 3 `ExecutionEngine`. This is session-local
handling for events such as known short cutscenes or text boxes; durable phase and workflow transitions
remain above the worker.
Likewise, the production Dolphin adapter fails closed when unmanaged regular breakpoints or memchecks are
present; it does not adopt, overwrite, or globally clear them.

Focused fake-backend/router guards cover physical-plan ownership, deterministic delivery, source-scoped
cleanup, ingress overflow, restore reconciliation, and sink lifetime. Deterministic hook-seam guards retain
exactly-once forwarding and decision-combination coverage. The focused live-Dolphin guard boots without
TAS input, executes a frame before registration, and then verifies passive Observe plus foreground Wake
delivery through the physical manager, JIT hook, and router at the recurring game-mode-controller entry
`0x801DC288`. Exact `prebattle.BeforeRandSeedSet` and live post-write memcheck validation are deferred
until deterministic execution and input can drive the game to those witnesses.

This slice does not restore production `ProgramInvocation`, interactive visual debugging, or
production-worker SavorE2E. Those remain unavailable until their later slices. It also changes neither
the Slice 1 `WRMS` protocol nor any SavorDb schema, storage representation, database-service interface,
queue, claim, workflow, transaction, or artifact-storage contract.

### Dependency slice 3: ExecutionEngine as sole emulator-advancement owner (completed)

**Implement:**

- introduce one session-owned, actor-driven engine with typed continue, frame-step,
  input-synchronized-advance, safe-pause, and interactive-resume operations;
- route every operation and every accepted stop receipt through the same control-thread event loop and
  router, with no engine thread or nested blocking loop;
- represent movie end, cancellation, requested completion, intercepted stops, and
  interruption-handler requests as structured results. The pre-6A hard cutover later replaces the
  initial request-owned timeout/VI-stall implementation with centralized core health;
- support requested interruption handlers as a structurally bounded stack whose trusted descriptors
  declare allowed nesting/recursion, whose absolute depth is at most eight, and whose only policy
  outcomes are `ResumeParent` or `AbortParent`;
- expose primitive movie/VI/throttle observations through the private execution-backend facet without
  moving movie lifecycle into the engine;
- define the fake-tested opaque input-advance collaboration, while leaving production input publication
  unsupported until Slice 4's `InputArbiter`;
- hard-disconnect current VM run-until, frame, opcode, tape, and macro advancement rather than bridging
  the production-disconnected interpreter into the engine; and
- extend WRMS v1 additively with capability-gated execution control/result/state messages. A session
  opened with visual intent may use serialized pause, resume, and frame-step controls while remaining
  `Ready`; a separate execution snapshot reports whether it is idle-paused or interactively running.

At the Slice 3 checkpoint, `InteractiveResume` was the sole intentionally unbounded engine operation.
The pre-6A cancellation/health cutover removes the other guest-operation elapsed deadlines as well;
bounded host confirmation and cleanup remain distinct. Guest PowerPC instruction stepping is not part
of the forward engine contract: the implementation does not switch temporarily to Interpreter, call a
JIT block an instruction, or expose a guest-step action. Production `ProgramInvocation`, DB-backed
visual replay, and SavorQt wiring remain unavailable. Production input advancement was deliberately
unsupported at the Slice 3 checkpoint and is supplied by Slice 4's `InputArbiter` without moving pad
publication into the engine.

**Completion checks:**

- repository search finds no Dolphin run/step call outside `ExecutionEngine`/backend implementation;
- every advancement mode remains interceptor-aware;
- cancellation and interruption-handler tests pass at every suspension boundary;
- focused fake-backend and protocol tests cover visual-intent controls without creating a render window;
- the live JIT64 guard is headless and exercises engine-owned frame advancement and routed continue at
  recurring `0x801DC288`; and
- no acceptance step launches or controls a GUI, compares rendered output, or requires user observation.

Do not run production-worker SavorE2E for this slice. `ProgramInvocation` remains intentionally
unadvertised until the later runtime, program-migration, and adapter slices restore it.

The completed checkpoint centralizes post-open advancement in the session-owned engine, adds the
capability-gated Ready-session WRMS control seam, and leaves `ProgramInvocation` unavailable. The
unused guest-instruction control surface is removed before Slice 6A. Full Debug and
Release x64 solution builds, focused and retained guards, and the headless recurring-`0x801DC288` JIT64
integration guard passed without creating or controlling a GUI.

### Dependency slice 4: scoped session services (implemented)

Slice 4 establishes generic session services and hard-disconnects the corresponding legacy VM escape
hatches. It does not restore `ProgramInvocation`.

- `StateService` is the sole authority for boot/reboot/restore and worker-local `StateEpoch`. It owns
  bounded immutable memory handles and caller-declared immutable file artifacts with SHA-256,
  runtime/disc compatibility, parent/edge/producer lineage, and optional exact movie continuation.
  Successful replacement advances the epoch exactly once; recoverable failure rolls back without
  advancing, while unproven post-replacement integrity keeps the new epoch and taints the session.
- A read-only checkpoint embeds the exact DTM history and verified SHA-256, game identity,
  starts-from-savestate fact, and any known input/frame continuation. External import is explicit:
  `NoMovie` and `ReadOnlyPlayback` are supported, while `Unspecified` and `Recording` are rejected. On
  restore, the backend materializes and hash-validates the embedded DTM before state mutation; an
  already-active read-only movie must have the same tracked DTM identity, and commit verifies the
  prepared identity, resulting mode, and any known cursor. Dolphin restores an unknown cursor from the
  savestate, and `MovieService` records the authoritative observed position. No file artifact captures
  or restores an in-progress recording; only a same-session in-memory handle may rewind one.
- `MovieService` validates and stages `Movie::PlayInput` before the single backend boot when initial
  playback is supplied through `SessionOpenOptions`. The same request on an already-open session
  legitimately uses a `StateService` reboot. Both paths propagate a DTM starting savestate, verify the
  resulting mode, and hold an unsuspendable movie-exclusive input reservation. Recording finalization
  publishes a new caller-declared DTM and any `<dtm>.sav` companion.
- `InputArbiter` owns epoch-bound leases and the sole pad-publication path. It supports priority,
  suspension, fresh publication tokens, poll acknowledgement, two-phase neutral release,
  movie-exclusive reservation, bounded `IInputAdvancePort` retries, and both interruption-borrow
  policies: preserve held input until borrower publication, or consume a fresh typed neutral witness
  issued by the arbiter for the exact parent lease/publication/epoch.
- `GuestMemory` supplies paused, epoch-checked generic reads. `GuestMutationService` supplies checked
  `u8`/`u16`/`u32` and masked data writes plus aligned executable patches. Data mutations restore unless
  explicitly committed; executable patches are always reversible and require symmetric JIT/cache
  invalidation and readback.
- One session-owned `CaptureService` accepts at most one opaque `savor.capture.profile/1` attachment.
  It preserves the existing profile parser and behavior behind passive router observation, performs
  actor-side dynamic reconciliation, survives restore by rebinding at the new epoch, and finalizes once
  on detach/shutdown. Mandatory finalization failure taints the session, blocks another attachment and
  session reuse, and requires a full rebuild. Capture cannot create a wake or advance Dolphin.
- `ScreenshotService` owns one synchronous actor-thread bounded screenshot request. Active in-flight
  cancellation remains deferred until nonblocking backend/actor ingress exists. `TelemetryBus` owns
  bounded/coalescing diagnostics, preserves monotonic sequence order when a coalesced replacement takes
  its fresh chronological position, and fails closed on required-event overflow.
- A standalone actor-owned `SessionResourceLedger` owns session/synthetic scopes, atomic typed receipt
  registration, promotion, reverse-order unwind, cleanup continuations, state-epoch end/rebind policy,
  and clean/diagnostic/taint disposition. Slice 5 adds the program-resource binding seam onto this same
  ledger.

Game capability packs are not part of Slice 4. They arrive with the typed action/module surface in Slice
5 and consume these narrow generic services. No broad game facade or placeholder pack is introduced.

Focused validation covers the service contracts with deterministic backends, actor/thread checks,
fault injection, and headless ownership guards. A JIT64 executable-patch guard at recurring
`0x801DC288` is the narrow live target: validate expected instruction, apply NOP, invalidate/read back,
restore/invalidate, and never advance while patched. Live state-plus-DTM continuation and live
post-write capture remain deferred until deterministic program execution/input can reach authoritative
witnesses without a GUI or user observation.

This slice adds no WRMS message or worker capability, no project or project reference, and no SavorDb
schema, migration, persistence, database-service, queue, claim, workflow, transaction, or artifact-store
change. At the Slice 4 checkpoint, production `ProgramInvocation`, capability packs, DB work, and
production-worker SavorE2E remain unavailable.

### Dependency slice 5: canonical typed ProgramRuntime (implemented foundation)

Slice 5 now establishes the canonical development surface without restoring the production worker path.

**Implemented:**

- immutable typed `ProgramModule`, CFG/basic-block IR, `ProgramInvocation`, `ProgramResult`, bounded value
  graphs/arena, and the `ProgramInstance` execution state;
- exact little-endian `SPRM`, `SPRI`, and `SPRR` version-1 envelopes with canonical ordering, bounded
  decoding, and SHA-256 module identity over canonical `SPRM` bytes with the declared hash omitted;
- `ProgramDefinitionStore`, `ProgramVerifier`, `ProgramExecutor`, `ActionRegistry`,
  `TypeSchemaRegistry`, and `CapabilityPackRegistry` under one `ProgramRuntime`;
- an actor-queued program action request/completion seam, resource-binding table, and concrete internal
  `SessionProgramActionHost` over the existing Slice 4 service/ledger ownership, without giving
  `ProgramRuntime` an `EmulationSession` or backend;
- generic `runtime.session` plus source-backed `soa.field`, `soa.battle`, and `soa.navigation` packs for
  the supported USA compatibility, including coherent battle/navigation queries and the pure
  `soa.battle.materialize_turn_input` reducer; and
- reusable semantic-observation, interaction, and predicate composition frontends that lower to
  ordinary IR before verification.

Semantic observation defines capability-pack-owned `SemanticPointDefinition`, `SemanticAwaitDefinition`,
`SemanticPointReceipt`, `AddressExpression<T>`, `ObservationDefinition<T>`, and `ObservationUse<T>`.
It lowers exact point alternatives, bounded hit-time samples, paused reads/registered coherent queries,
explicit later-point or frame-granular behavior, required/optional evidence, baselines, and epoch checks into ordinary IR,
actions, scoped router subscriptions, and emissions.

Interaction composition defines versioned `InteractionDefinition<State, Output>`, a finite
verifier-known segment set, pure initialization/advancement reducers, and typed
`InteractionSegmentResult`. It lowers static and adaptive input sequences into ordinary subprogram CFG,
semantic awaits/observations, input/execution actions, branches, and emissions. Predicate composition
consumes typed observation results and lowers pure conditions plus explicit `Check` policies through the
same surface. All three composers finish lowering before `ProgramVerifier` validates the resulting
module and exact dependency closure.

**Focused checkpoint coverage:**

- model/value-graph and `SPRM`/`SPRI`/`SPRR` canonical codec guards;
- definition-store, registry, dependency-closure, and verifier rejection guards;
- deterministic executor quantum, action suspension/correlation, cancellation/unwind, and budget guards;
- `ProgramRuntime` preparation/invocation/result, malformed-envelope, stale-epoch, and actor-sink guards;
- actor action-protocol, resource-binding, and WorkerRuntime queue/correlation guards;
- exact source-pack inventory, compatibility, query, and pure battle-turn materialization guards; and
- focused lowering guards for all three composition frontends, including atomic failure and interaction
  ordering.

This list records the focused guard surface in source; it does not substitute for the current
solution-build and test-run results.

This checkpoint deliberately does not register `soa.cutscene` or `soa.overworld`, translate a legacy
phase, construct the runtime or its implemented action host in production `SavorWorker`, advertise
`ProgramInvocation`, run a live game-program smoke, or run production-worker SavorE2E. It adds no
SavorDb schema, migration, persistence, database-service, queue, claim, workflow, transaction, or
artifact-store change. Those omissions are deferred boundaries, not skipped Slice 5 validation claims.

### Pre-6A hard cutover: cancellation-driven execution and centralized core health

Complete this slice before any native phase module:

- remove `run_ms`, `run_timeout_ms`, `vi_stall_ms`, timing overrides, macro/memory/tail timeouts, TAS
  timing headroom, and every derived active wall-clock budget from runtime contracts, phase payloads,
  adapters, fingerprints, workset keys, SavorPredict, SavorE2E configuration, and UI;
- remove active elapsed limits from `ProgramBudgets`, action requests, `ExecutionRequestPolicy`,
  interruption descriptors, semantic/interaction composition, workset item templates, and aggregate
  workset limits. Keep verifier structural limits, finite semantic retry/poll counts, resident-resource
  limits, and explicitly classified bounded host operations;
- classify guest-dependent work as `CancellationDriven` and filesystem/state/protocol/screenshot/
  pause-confirmation/finalization/cleanup/shutdown work as `BoundedHostOperation`. Only the latter may
  return an infrastructure timeout;
- install one actor-owned session health monitor. Begin eligibility only after engine-owned advancement
  is confirmed running; VI/CoreTiming progress or a changed synchronous host-activity generation resets
  its baseline;
- make native routing, bounded sampling, synchronous capture observation, reconciliation, state
  replacement, handler suspension, and paused host actions ineligible. CPU-hook activity registration is
  atomic, allocation-free, and `noexcept`; background artifact finalization does not mask guest health;
- emit `SuspectedCoreStall` after ten eligible seconds, confirm after ten more, and warn on one continuous
  host activity after ten seconds and every thirty seconds thereafter. When actor-owned activity blocks
  publication, retain its duration and emit every crossed threshold on the next actor pump. Retain
  completed-scope diagnostics in a bounded lock-free buffer and diagnose saturation explicitly.
  Confirmed stall safely pauses and
  produces `CoreStalled` plus `CleanWithDiagnostics` when integrity is proven; otherwise taint and shut
  down the session. Preserve the existing WRMS numeric terminal value and add no message or capability;
- remove timing fields from public SeedProbe/TAS/Battle Run authoring commands, snapshots, editors, and
  identity. Do not change migrations or DDL. The six existing `NOT NULL` columns remain physically
  present and ignored; only the three private SQLite inserts bind neutral `0,0` values. A separate
  database refactor removes the columns and shims; and
- revise the unpublished runtime/workset v1 codecs in place, reject obsolete legacy payload revisions
  before session mutation, and give newly generated native fingerprints a timing-free revision/namespace.

Validate this hard cutover with full Debug and Release solution builds; injected-clock health/race tests;
runtime, composition, workset, authoring, ProgramDB, SavorPredict, protocol, and codec guards; and the
headless Release JIT/router guard. Do not run SavorE2E while production invocation remains unavailable.

### Pre-6A dependency prelude: production process and pipelined WorkerWorkset foundation

Build the real process boundary before reconstructing an individual phase:

- construct the implemented `ProgramRuntime` and concrete `SessionProgramActionHost` in production
  `SavorWorker`; this is the one runtime/action composition later slices extend, not a test-only or
  compatibility composition;
- replace the not-yet-activated scalar submission with one `SubmitWorkset` path accepting 1..N immutable
  invocation templates; reserve the old direct-invocation discriminator and reject it before session
  mutation. Keep WRMS at version 1 and likewise reserve/reject the removed guest-instruction-step
  discriminator;
- negotiate the exact protocol/runtime/dependency manifest, the currently installed module/entrypoint
  catalog, and hard active/staged/cache/finalizer/completion-ledger limits. Catalog state is explicitly
  `Partial` or `CompleteExact`. Transfer one canonical test-only module through the ordinary module
  preparation protocol for the initial `Partial` real-process smoke; it is never one of the nine
  production modules and cannot open the coordinator data plane;
- give `WorkerRuntime` ownership of at most one active session-mutating workset, at most one immutable
  host-only staged successor package, one worker-global completion/acknowledgement ledger, and one
  active child `ProgramInvocation`/`ProgramInstance`; neither staged nor completed children are visible
  to `ProgramRuntime`;
- require one exact `WorkerWorksetExecutionKey` for a multi-item workset, covering module, entrypoint,
  dependency closure, runtime/session profile, exact `ProgramBaselineKey`, and execution/service
  compatibility. Its `ProgramBaselineDefinition` orders the savestate, exact movie continuation, and
  runtime-facing program-kind adapter-declared derived-state components;
- allow staging to decode and validate envelopes, resolve immutable cached definitions, read/hash
  immutable artifacts, and acquire bounded state-cache leases. Staging cannot restore guest state, bind
  `StateEpoch`, capture a baseline, acquire session-effect resources, construct a `ProgramInstance`, or
  advance Dolphin;
- prepare the active key's exact source state once; retain one composite baseline definition and its
  required cache lease at active-workset scope only for a multi-item workset, and run the first child
  from the prepared state. Before every later child, `RestoreBaseline` restores the savestate/movie
  continuation and every declared derived-state component as one transaction, returns one
  `PreparedProgramBaselineReceipt`, and advances `StateEpoch` exactly once. A one-item workset skips
  unnecessary reusable-baseline capture;
- after a durable dependent transition commits, ordinary affinity may prefer the same clean worker and
  attempt exact `ContinueSession`; any lineage/epoch/movie/`SessionResourceLedger` cleanup mismatch falls
  back to a cache-assisted or ordinary immutable-artifact restore. Pending host-only finalizers or
  acknowledgements do not invalidate the warm path while global credit remains. A producer and
  dependent successor never share one workset;
- bind each child to the authoritative session and current `StateEpoch` immediately before activation,
  and require complete child unwind before activating another child. Validate claim/start authority for
  the complete finite membership before acceptance; once accepted, run clean children in order without
  a per-item coordinator authorization pause, using exact asynchronous cancellation for later lease
  loss, supersession, or user cancellation;
- capture immutable state bytes synchronously while paused, but permit bounded host-only hashing,
  sidecar/file publication, and validation after promotion into the completion ledger. No authoritative
  terminal containing that artifact is published until finalization succeeds;
- maintain one outbound sequence across worksets and assign a separate actor-owned terminal-order
  ordinal when synchronous execution completes or an item is classified unstarted. Stream each
  authoritative child terminal as soon as all required outputs are complete without letting a later
  ordinal overtake it, retain it in the worker-global ledger until its exact durable acknowledgement,
  and stop later admission when negotiated ledger count/byte credit is exhausted;
- after every child has produced a completed execution record with required immutable output captured
  or is classified unstarted, and the active workset's invocation/baseline scopes release cleanly,
  promote the staged successor even while older outputs finalize or terminals await acknowledgement.
  The old workset's bookkeeping summary waits for its output finalizers and acknowledgements but no
  longer owns the session; and
- allow no dynamic item addition, worker-side reordering, durable successor selection, or worker access
  to SavorDb.

Use the fixed configurable defaults: 16 items and 32 MiB encoded bytes per workset; 64 total worker item
credits and 32 active-plus-staged items; 16 state-cache entries/512 MiB; two finalizer threads with eight
pending captures/256 MiB; 32 retained authoritative terminals/128 MiB; two concurrent worker startups;
and at most one coordinator-buffered additional workset per negotiated Ready worker. No workset carries
an elapsed guest-execution budget.

Each 6A-6I slice adds its module and exact dependency manifest to this real process catalog and runs an
unattended one-item `SubmitWorkset` process test through the production worker/runtime/action-host seam.
Diagnostic modules use that same path. These incremental process tests do not advertise a complete
production catalog and do not authorize DB-backed worker dispatch.

Implement the adjacent coordinator pipeline now, while keeping it gated until Slice 7:

1. replace initial all-worker serial preflight with progressive startup: negotiate one compatible
   worker as the eventual data-plane gate, then start the remaining desired workers with at most two
   concurrent startups. The gate remains closed until that worker reaches `CompleteExact`;
2. publish per-worker item-capacity credits and derive claim demand from unreserved credit plus a
   coordinator buffer capped at one additional workset per negotiated Ready worker. One credit remains
   consumed from assignment through staging/residency/execution/finalization/terminal retention until
   exact durable acknowledgement;
3. claim up to that demand in one real ordered batch transaction while retaining an independent
   claim/lease/attempt identity for every returned job;
4. materialize and index claimed jobs by stable priority/claim order and exact compatibility so staging
   does not rescan every candidate or issue a database read for every worker/candidate comparison.
   Choose the highest-priority, oldest eligible anchor, search only that priority class inside a
   configured count/byte window, retain exact-key peers in durable claim order, prefer an already
   compatible warm worker, and resolve otherwise-equal choices by queue time, job ID, worker ID, and
   item ordinal;
5. form at most one host-only staged successor package per worker from adapter-declared eligible items
   with an exactly matching `WorkerWorksetExecutionKey`; persisted affinity remains a hint rather than
   compatibility proof;
6. keep every staged, resident, or otherwise admitted but unstarted item in its current durable
   `CLAIMED` state, validate claim/start authority for the complete finite membership in one exact-set
   operation before `SubmitWorkset`, and renew exact lease sets afterward. A lease-loss, supersession,
   or user-cancellation disposition sends an exact asynchronous cancellation; absent that notice,
   accepted authority persists without a per-item admission roundtrip;
7. consume the worker's ordered informational item-start event and append the current `JobStarted` event
   before processing that child's later terminal; the worker emits start immediately before effects and
   does not wait for a coordinator decision, acknowledgement, or permission to admit the next clean
   child;
8. count active, staged, coordinator-buffered, outbound, finalizing, worker-resident nonterminal, and
   unacknowledged-terminal items/bytes against negotiated capacity;
9. project each streamed child result immediately through the existing per-item result/artifact
   transaction, then acknowledge that exact item after durable projection. Publish the affected
   workflow-step identity and commit sequence to the targeted advancement path without delaying the
   acknowledgement. Worker/session availability is driven by the clean active-workset snapshot rather
   than result projection, because the global ledger retains the terminal independently; and
10. use event notifications for ordinary ready/materialized/result/acknowledgement wakeups and process
    targeted terminal advancement in commit-sequence then stable-ID order. Retain bounded periodic scans
    for cross-process discovery, missed-notification repair, lease/recovery safety, and terminal
    reconciliation.

Completed items are never rolled back because a later item fails. A host-output finalization failure
produces one infrastructure terminal for the affected item and may continue later items when the
session remains proven clean. Workset rejection, cancellation, transport loss, worker loss, lease loss,
or session taint sends each remaining nonterminal job through the existing per-job recovery/requeue
behavior. Duplicate terminal delivery uses the current attempt/idempotent publication rules and is then
acknowledged. A tainted worker promotes no staged successor.

This prelude adds no SQL/schema migration, persistent workset/cache/ledger identity, aggregate attempt
or result, queue state, workflow record, or new artifact format. Narrow execution interfaces may support
ordered batch claim, exact-set lease renewal, pre-submission exact-set claim/start authority validation,
and targeted terminal
reconciliation; they preserve existing per-job lifecycle and result-projection/per-item semantics while
changing only claim transaction granularity, and they do not grant workers database access.

**Focused completion checks:**

- production `SavorWorker` constructs exactly one canonical runtime/action host, negotiates an explicit
  `Partial` catalog, transfers and executes the canonical test-only module in an unattended one-item
  process workset, and never counts that module among the nine production modules or enables
  coordinator data-plane work;
- one-item and multi-item worksets produce identical per-job lifecycle, result, retry, artifact,
  transition, and recovery observations;
- deterministic ordering, one-active-child ownership, exact-key rejection, every declared limit,
  singleton baseline avoidance, just-in-time epoch binding, composite savestate/movie/derived-state
  baseline restore, one `PreparedProgramBaselineReceipt`, and complete unwind are covered with fakes;
- host-only staging cannot mutate the session, a clean staged successor promotes without waiting for
  older finalizers or acknowledgements, full completion-ledger capacity halts admission, and
  cross-workset outbound and terminal ordering remains deterministic;
- state-cache hit/miss equivalence, lease/eviction behavior, asynchronous artifact-finalization
  ordering/failure, and no-terminal-before-finalization are covered;
- progressive startup, exact item-credit accounting/release, one-transaction ordered batch claim,
  stable indexed selection with starvation protection, exact-set lease renewal, claim/start races,
  targeted reconciliation, event wakeups, and bounded safety polling are covered;
- workset admission never marks every child running; the worker's ordered start event precedes that
  child's effects without a coordinator round trip, and the coordinator appends `JobStarted` before
  processing the ordered terminal;
- one exact-set authority validation occurs before acceptance, no authorization request occurs between
  clean children or at staged promotion, and later lease loss/supersession produces exact cancellation;
- boundary tests enforce the fixed item/byte/structural-limit, total/staged-credit, cache, finalizer/pending-
  capture, retained-terminal, startup, and coordinator-buffer defaults;
- WRMS remains version 1, and both the old `SubmitInvocation` and guest-step discriminators reject before
  session mutation;
- cancellation, transport loss, worker loss, stale/duplicate completion, lease loss, and taint preserve
  independent durable recovery; and
- no persistent workset/cache/ledger or unrelated database/workflow contract appears in the diff.

### Dependency slices 6A-6I: native phase migration

Before 6A, remove the unused guest-instruction-step surface from `ExecutionEngine`, the action catalog,
semantic-observation and interaction composition, WRMS controls, `ProcessWorker`, and focused tests.
Reserve its former WRMS numeric discriminator rather than renumbering any surviving message. Receiving
that discriminator rejects the request before session mutation. Source-stop departure uses exact
current-receipt suppression and ordinary routed continuation. Where causality after a guest instruction
matters, the module declares a semantic successor; frame stepping is used only for genuinely
frame-granular behavior.

This prelude removes the newer typed guest-step API and protocol surface. The older
`visual_step_vm()` method remains an already disconnected legacy facade that always fails locally; it is
never ported and is physically deleted with the rest of that facade in Slice 8.

`DolphinWrapper::stepBootCoreForStateLoadBlocking` is the sole narrow exception. It remains a private
state-replacement preflight for a paused zero-timebase boot core whose guest state is immediately
replaced. It is not exposed through `ExecutionEngine`, an action, ProgramRuntime, WRMS, or debugging.
Its current ignored completion-wait result is a backend correctness gap: propagate timeout/failure
through the existing typed state-load failure before final activation, without redesigning or exposing
the helper.

Each phase slice is vertical. It provides the native typed module, any action or composition refinement
that the phase proves necessary, the existing program-kind adapter input/result projection where
applicable, and focused headless validation. Current PhaseScript builders, providers, opcodes, and tests
are read as orientation and behavioral evidence only. They are not compiler inputs, executable
references, or a required trace oracle. Existing stored macro, predicate, address-program, payload,
result, and capture-profile representations are translated or consumed in memory without changing their
storage contracts. Each existing program-kind adapter also declares whether its independently durable
jobs are eligible for multi-item dispatch and derives the exact `WorkerWorksetExecutionKey`; module
inputs and results remain scalar regardless of the surrounding workset.

Direct process program/workset execution is available throughout 6A-6I through the production
runtime/action-host seam and an explicitly partial catalog. Coordinator data-plane work remains
unavailable. `CompleteExact` is advertised only after exactly all nine planned production and diagnostic
module IDs/hashes and their exact dependency manifests are present, no extra module is installed, and
the catalog can be activated atomically in Slice 7.

#### Slice 6A - SeedProbe

- Implement native `soa.seed_probe/probe` for neutral, grid, unique, prebattle, and field-return
  configurations.
- Adapt the existing payload/result representations at the runtime boundary and preserve optional state
  publication and exact state lineage.
- Validate RNG observation, branching, input cleanup, cancellation, and state publication with typed
  module tests, existing codecs/artifacts, and focused headless integration.

#### Slice 6B - Navigation Context

- Implement native `soa.navigation.context/capture`.
- Compose current-point qualification, semantic waiting, coherent navigation observation, and atomic
  NCTX plus matching-state publication.
- Project through the existing Navigation Context handler and workflow representation without changing
  persistence or interfaces.
- Validate immediate and awaited capture paths, qualification failures, NCTX/state lineage, and cleanup.

#### Slice 6C - TAS Movie

- Implement native `soa.tas_movie/play_and_checkpoint` over `MovieService`.
- Make boot/state policy, configured stop, movie-completion/failure policy, and checkpoint publication
  explicit in the typed contract.
- Adapt the existing TAS Movie payload/result representations without moving movie ownership into the
  module.
- Validate lifecycle, stop-versus-movie-end precedence, checkpoint lineage, cancellation, and cleanup
  headlessly.

#### Slice 6D - TAS Frame Detector

- Implement direct diagnostic `soa.tas_frame_detector/detect`.
- Reuse the TAS Movie lifecycle and use only bounded frame stepping for per-frame observation.
- Add no SavorDb descriptor, program kind, workflow step, or stored representation.
- Validate detection bounds, frame observations, movie-end behavior, and unattended one-item
  production-process activation through the partial catalog.

#### Slice 6E - Battle Context

- Implement native `soa.battle.context/capture`.
- Accept an exact current point when valid, otherwise await `TurnInputs`, then acquire one coherent typed
  battle-context observation.
- Project the result through the current Battle Context handler; durable fan-out remains in the existing
  workflow services.
- Validate both entry paths, query coherence, result projection, state lineage, and cleanup.

#### Slice 6F - Battle Macro Probe

- Implement direct diagnostic `soa.battle.macro_probe/probe` through semantic-observation and interaction
  composition.
- Reconstruct every current step-off or held-opcode intent with receipt suppression or a declared
  semantic successor under the same publication.
- Preserve request acknowledgement, real release witnesses where behavior depends on guest-observed
  neutral, baseline-before-advance, and bounded adaptive reducer behavior.
- Add no SavorDb descriptor or workflow step. Validate focused macro branches and unattended one-item
  production-process activation through the partial catalog.

#### Slice 6G - Battle Single Turn

- Implement native `soa.battle.single_turn/execute`.
- Compose checked RNG mutation, command interaction, unchanged capture-profile attachment, typed
  observation/predicate checks, bounded local retry, output state, and structured result.
- Project inputs/results through the current Battle Single Turn handler while leaving durable turn-wave
  workflow behavior unchanged.
- Validate mutation restoration, retry epoch behavior, capture compatibility, predicate progress and
  rejection, command outcomes, output lineage, and mandatory unwind.

#### Slice 6H - Battle Completion

- Implement native `soa.battle.completion/complete`.
- After state restore, while paused, acquire the phase input lease and publish neutral for the new epoch
  before any resume. Require successful host-side publication; do not require a guest neutral-poll
  acknowledgement or release witness.
- Establish causal post-store evidence only through `0x8006F554 -> 0x8006F558` or
  `0x8006F590 -> 0x8006F594`, then verify `battleInputState == 2`.
- Complete reward entry/commit, BCMB publication, and output-state publication through the existing
  handler projection.
- Validate both causal routes, mismatched successors, neutral-before-resume ordering, reward branches,
  BCMB/state lineage, cancellation, and cleanup.

#### Slice 6I - Battle Results Screen

- Implement native `soa.battle.results_screen/advance` directly from the split Results Screen contract.
- Consume BCMB and state lineage, preserve real UI request/release acknowledgement semantics, and publish
  BERB plus final state through the current handler projection.
- Do not call, wrap, or reconstruct the monolithic victory-to-results provider path.
- Validate the split entry boundary, required/full adaptive policies, UI acknowledgement and release,
  lifecycle invariants, BERB/state lineage, cancellation, and cleanup.

There is no Slice 6J, `soa.battle.legacy_path`, legacy BattleRunner module or adapter, or monolithic
BattleEndResults module. `PK_BattleEndResultsRunner` remains only an alias for the Results Screen
representation where existing compatibility requires it.

### Dependency slice 7: final production activation

**Implement:**

- promote the incrementally characterized catalog to `CompleteExact`: exactly the planned nine module
  IDs/hashes, their exact dependency manifest, and no extra installed module;
- advertise the `CompleteExact` production gate over the already-active encoded
  module/workset/per-item-result/acknowledgement transport; do not advertise or retain a scalar-only
  production invocation capability;
- use the program-kind adapter projections completed vertically in 6A-6I;
- keep current persisted `ProgramKind`, payload/result codecs, job identity, affinity, queue, claim, and
  workflow representations while removing `ProgramKind` from worker execution selection; and
- negotiate one `CompleteExact` worker before opening coordinator data-plane work, then start the
  remaining desired pool progressively with at most two concurrent startups. Every later worker must
  pass the same protocol, runtime, dependency-manifest, exact-catalog, and
  active/staged/cache/finalizer/ledger-limit checks before contributing capacity.

Existing payload/result bytes remain valid production data. Program-kind handlers may retain codecs
solely to translate that representation into and out of runtime values; those codecs neither execute nor
select the legacy interpreter.

**Completion checks:**

- workers may negotiate explicit partial catalogs for pre-Slice-7 direct tests, but the coordinator
  rejects protocol, runtime-profile, dependency-manifest, negotiated-limit, any missing production
  module, or any extra installed module before data-plane work;
- the first compatible worker opens the data plane without waiting for the entire desired pool, remaining
  compatible workers add capacity progressively, and rejected later workers never inflate claim
  capacity;
- one-item worksets cover every direct or singleton production dispatch, with no scalar-only fallback
  and no mixed production fleet lacking workset support;
- existing jobs, queues, workflows, results, artifacts, retries, and restart behavior remain compatible
  without a database migration or record conversion; obsolete timing keys are ignored rather than
  converted into native execution policy;
- multi-item worksets preserve independent claim/start/attempt/result/retry/transition behavior and
  immediate per-item projection;
- program-kind handlers project through timing-free authoring/runtime-facing SavorDb interfaces and the
  existing transaction boundaries;
- the Release SavorE2E `all` matrix passes, followed by separate `battle_end` and
  `navigation_context` scenarios without adding either to `all`;
- TAS Frame Detector retains focused direct validation and Battle Macro Probe retains its direct-worker
  boundary; and
- no `ProgramKind` value selects worker execution behavior after materialization.

### Separate follow-on: workflow/frontier generalization

Generalized typed workflow policies, persisted frontier nodes/edges/leases, new barrier models,
DFS/BFS/best-first scheduling, persisted deduplication, and new workflow transaction shapes are not a
slice or prerequisite of this refactor. They require separate planning under the SavorDb
migration/workflow planning surfaces.

This refactor establishes only the negative worker boundary: a bounded program cannot mutate durable
topology or keep an unbounded search frontier in worker memory. Existing SavorDb workflow and dynamic-step
behavior remains unchanged.

### Dependency slice 8: remove the legacy execution path

Slice 8 is deletion only. It adds no replacement behavior.

**Delete:**

- `PhaseScriptVM` as an interpreter/controller;
- the central legacy opcode dispatch and domain opcodes;
- obsolete `PhaseScript` program builders and providers after their native modules are active;
- `ProgramRegistry` construction and payload-decoding switches;
- `PSContext` as public invocation/result ABI;
- the peer `InputMacroRuntime` execution path and VM macro-host inheritance;
- obsolete direct-read/stop/macro paths superseded by semantic-observation and interaction
  composition;
- numeric `ProgramKind` worker dispatch and any planned `PK_UserScript` path;
- direct program ownership of Dolphin, breakpoint sets, input, capture, or one hidden snapshot;
- retained legacy worker APIs, including the disconnected `visual_step_vm()` facade;
- legacy BattleRunner source plus its registry/payload-dispatch path; and
- the monolithic BattleEndResults compatibility implementation.

Existing payload/result codecs may remain behind SavorDb program-kind handlers to translate current
persisted records during production materialization and result writing. They cannot select or invoke the
legacy worker interpreter. Compatibility aliases such as `PK_BattleEndResultsRunner` may remain only
where existing stored Results Screen representations independently require them. The private
`stepBootCoreForStateLoadBlocking` state-replacement preflight remains isolated and is not generalized
into a runtime action.

**Completion checks:**

- repository architecture tests fail if a second executor/controller or forbidden dependency is added;
- no executable production path can instantiate the old interpreter;
- all supported program-kind handlers construct exact runtime invocations and consume typed runtime
  results through existing SavorDb contracts; and
- complete Debug and Release x64 solution builds plus deletion/architecture guards pass. Because this
  slice removes only source already disconnected from the Slice 7 production path, it does not repeat
  the full SavorE2E matrix unless deletion unexpectedly changes active production composition.

### Separate post-cutover follow-on: ProgramRuntime debugging

A future `StepProgramInstruction` operation may advance one verified ProgramRuntime IR instruction or
terminator through the actor-owned `ProgramExecutor`, conceptually with a one-instruction pump quantum.
An awaited action is dispatched exactly once and treated atomically; completion returns the program to a
debug-paused state before the next IR instruction. Results identify the program instruction and its
source-map location.

This is program stepping, not emulator instruction stepping. Cancellation and mandatory unwind cannot
be manually stepped or stranded. No WRMS shape, `ProcessWorker` API, SavorQt UI, or implementation is
part of phase migration or production cutover.

### Post-refactor example: Navmesh Survey

If Navmesh Survey is implemented after the current runtime migration:

- implement `soa.navigation.survey/establish_anchors`;
- implement `soa.navigation.survey/probe_geometry`;
- add only genuinely reusable `soa.navigation` actions and generic mutation capabilities;
- integrate through existing SavorDb workflow/dynamic-step operations where they are sufficient, without
  changing storage or interfaces; and
- validate the bounded runtime portions of the exact `a101b` slice defined in documents 08 and 10, using
  a harness or unchanged existing SavorDb contracts.

Survey should not require changes to `WorkerRuntime`, `ProgramRuntime`, `ProgramExecutor`,
`ExecutionEngine`, core IR, router ownership, or a program-kind switch. If it exposes a genuinely
general missing primitive, revise the reusable architecture instead of adding a Survey-specific
controller.

**Example checks:**

- the bounded-runtime `a101b` acceptance scenario passes;
- Survey evidence remains spatial;
- no per-anchor savestates, serialized ground-selector state, permanent hook, or `eventhook` dependency
  exists; and
- end-to-end durable two-wave orchestration is not a Survey completion requirement unless current
  SavorDb contracts already support it.

### Separate follow-on: authored-program frontend

After the universal runtime, migrated phases, and legacy deletion, a separate follow-on may:

- define authored source syntax and publication workflow;
- compile it to the same `ProgramModule`;
- add authoring validation/debugging without a `PK_UserScript` executor.

Any persistence for authored source, revisions, canonical modules, or publication state is outside this
refactor. That project must not reopen the runtime ABI around the shape of the current `PhaseScript` or
`PSContext`.

## Interfaces and ownership affected

| Existing surface | Cutover disposition |
|---|---|
| Worker wire `ProgramKind` activation | Replaced by exact invocation/module identity |
| `ProgramRegistry` switch | Replaced by definition store, verifier, and dependency registries |
| `PhaseScriptVM` | Replaced by `ProgramExecutor` plus session actions |
| Opcode table | Current-source orientation until Slice 8; then deleted |
| `PSContext` | Removed as the worker runtime ABI; a codec may remain behind SavorDb handlers where existing stored records require it |
| Current stop/read/address-program/query/baseline helpers | Translate in memory through semantic-observation composition; generated behavior uses router subscriptions, execution actions, guest reads or registered coherent queries, ordinary values, and emissions |
| `InputMacroRuntime` peer engine | Existing plans/providers translate in memory through interaction definitions, pure reducers, verifier-known segments, and common subprograms/actions |
| `PhaseScriptVM` predicate table and evaluator | Existing records translate at the module-builder boundary; predicates consume semantic-observation results and generated execution uses ordinary IR, actions, branches, and emissions; the legacy evaluator is deleted with the VM |
| `savor.capture.profile/1` | Representation and semantics remain unchanged behind passive `CaptureService`; router/engine retain wake and control authority |
| SavorDb program-kind handler implementations | Adapt existing records to/from runtime contracts without changing their interfaces or storage |
| Worker program transport | `Partial`/`CompleteExact` catalog negotiation plus one `SubmitWorkset` path for 1..N independently correlated invocation templates, host-only staging, globally ordered non-lossy per-item terminals, and exact acknowledgements |
| Worker output/state locality | Bounded immutable `StateCacheKey` leases and host-only artifact finalization behind the worker-global completion ledger; every restore still uses `StateService` and a terminal never precedes required output validation |
| Coordinator materialization and worker bookkeeping | Progressive startup, negotiated item credits, deterministic exact-key assembly, one staged successor, ordered item-start mapping to current `JobStarted`, projection, acknowledgement, and existing recovery |
| Narrow SavorDb execution operations | Real ordered batch claim, exact-set lease renewal, claim/start validation, and targeted known-terminal reconciliation over current records and per-item semantics |
| Workflow lifecycle/outbox/recovery | Preserved unchanged; no typed-binding or frontier persistence is added |
| Existing and historical jobs/artifacts | Stored representation remains unchanged; no conversion |

## Failure and cleanup behavior

- During development, a failed native phase characterization or integration guard returns to focused
  investigation, never per-job production selection of the old VM.
- A protocol/runtime/dependency/limit mismatch rejects the affected process before boot/load/mutation.
  An explicit `Partial` catalog may run direct process guards, but any `CompleteExact` mismatch rejects
  coordinator data-plane activation before claim/materialization threads.
- Immediately before effects, the worker publishes an ordered child-start event without waiting for the
  coordinator. The coordinator appends the existing per-job `JobStarted` before processing that child's
  ordered terminal. A completed child enters the global ledger as a terminal or reserved pending-output
  entry, is projected and acknowledged independently when authoritative, and is never rolled back
  because a later child fails.
- Workset rejection, cancellation, transport loss, worker loss, or taint sends every remaining
  nonterminal child through current per-job recovery/requeue behavior; taint prevents another child from
  starting.
- Running legacy worker executions are drained before deployment; persisted and queued jobs remain in
  place and are translated by the updated program-kind handlers.
- A worker that fails mandatory cleanup is tainted and retired; retry starts in a fresh/known session.
- No database or artifact migration occurs. Application rollback leaves existing records untouched.

## Dependencies and migration implications

The slices follow this dependency direction:

```text
existing E2E baseline + just-in-time characterization
  -> serialized worker/session ownership + WRMS process/session protocol
  -> physical stop ownership/router
  -> execution ownership
  -> scoped generic services
  -> universal program runtime + modular capability packs
  -> production process + pipelined WorkerWorkset prelude
  -> cancellation-driven execution + centralized core health hard cutover
  -> native phase slices 6A-6I
  -> final production activation
  -> legacy deletion
```

Slices may overlap where their interfaces and ownership boundaries are clear. The order may be adjusted
when implementation evidence supports it, but downstream work must not compensate for an unresolved
upstream owner. In particular:

- ProgramRuntime cannot compensate for unresolved emulator ownership.
- Current phase migration cannot retain direct Dolphin escape hatches.
- The predicate composition library arrives with the typed module-builder surface and is exercised by
  current-phase migration; it is not another dependency slice, execution owner, database service, or
  persistence project.
- Semantic-observation and interaction composition arrive in that same builder slice before current-phase
  migration. They are frontends that disappear into verified IR, not new runtime owners, schedulers,
  opcodes, query VMs, database services, or persistence projects.
- Capture extraction wraps the existing profile language and behavior behind passive `CaptureService`;
  designing a generalized replacement capture plan is a separate future decision.
- A bounded expansion program cannot schedule its own durable children; any generalized frontier work is
  a separate project.
- Survey cannot become the justification for a phase-specific controller.

## Functional acceptance

- The final Release `SAVOR.sln` build, production-worker SavorE2E `all` matrix, and separate
  `battle_end` and `navigation_context` scenarios pass; the two separate scenarios remain outside
  `all`.
- A production release contains one program executor and one emulator-advancement owner.
- All current supported phases use the stable module IDs and entrypoints listed above.
- Current workflows retain transactional transitions, retries, outbox delivery, restart recovery, and
  dynamic-wave behavior.
- Current battle predicate rejection, progress, and scoring behavior remains available through the
  shared library, with no predicate-specific executor, controller, runtime service, domain opcode, or
  persistence model.
- Current semantic waits, address/query observations, adaptive interactions, input acknowledgement,
  baselines, and memory-change waits remain available through the shared composers with no peer
  observation/interaction runtime or hidden controller.
- Existing `savor.capture.profile/1` parsing, sampling, control-observation, window/recorder, progress, and
  artifact semantics remain compatible behind passive `CaptureService`.
- `SubmitWorkset` is the only production program-dispatch path; one-item and multi-item worksets retain
  identical per-job durable lifecycle, result, retry, transition, and recovery behavior.
- Negotiated capacity and exact-set claim leases account for active, staged, finalizing, resident, and
  unacknowledged work. A clean staged successor may promote while the worker-global ledger retains older
  terminals for projection and acknowledgement.
- One `CompleteExact` worker gates coordinator activation; remaining desired workers join with at most
  two concurrent startups and contribute capacity only after identical negotiation.
- In-flight legacy executions are drained/canceled rather than migrated.
- Exact invocation/result/module compatibility is enforced after current claim/materialization and before
  worker activation or guest-state mutation.
- No SavorDb migration, persistent workset/cache/ledger record, queue-state or workflow-persistence
  change, per-item lifecycle change, or artifact-format change is introduced. Obsolete public authoring
  timing fields and newly generated timing arguments are removed; the six physical columns remain behind
  private neutral insert shims. Only that cleanup and the documented narrow ordered-claim, exact-set
  lease, claim/start validation, and targeted-reconciliation interfaces change.
- The legacy interpreter, public `PSContext` ABI, peer macro engine, and program-kind execution switch are
  absent from production after dependency slice 8.
- Rollback changes only the matched application release, leaves existing data untouched, and never
  enables dual runtime dispatch.

Focused unit, concurrency, fault-injection, architecture, native module, codec/artifact, and headless
integration tests are development tools for risky seams and reconstructed behavior. They inform
implementation but are not a separate release approval process.

Intermediate hard-cutover slices are accepted by solution compilation plus focused guards for the seam
being changed. After the pre-6A prelude, every 6A-6I module also receives an unattended one-item
production-process guard through its explicitly partial catalog. DB-backed production-worker SavorE2E
remains unavailable until Slice 7 activates the complete production path.

## Deferred work

- Exact commit/PR grouping and deployment calendar.
- Any future incompatible envelope version beyond the fixed `WRMS` header and canonical
  `SPRM`/`SPRI`/`SPRR` version-1 formats.
- ProgramRuntime instruction-debug protocol, process API, and SavorQt UI design.
- Empirical tuning beyond the fixed configurable defaults for workset depth/bytes/budget, worker and
  staged credits, cache, finalizers/pending captures, retained terminals, two-worker startup concurrency,
  one-buffered-successor-per-Ready-worker policy, worker-pool size, and throughput.
- Any generalized capture-plan authoring language or replacement for `savor.capture.profile/1`.

Authored-source persistence/UI, historical retention policy, workflow/frontier generalization, and
distributed frontier sharding are separate projects, not deferred implementation choices in this
refactor.

## Source references

- `planning/ExecutionRuntime/02-target-execution-architecture.md`
- `planning/ExecutionRuntime/03-program-modules-ir-and-types.md`
- `planning/ExecutionRuntime/04-actions-effects-and-session-services.md`
- `planning/ExecutionRuntime/05-invocation-result-versioning-and-artifacts.md`
- `planning/ExecutionRuntime/06-workflows-frontiers-and-phase-composition.md`
- `planning/ExecutionRuntime/07-current-phase-migration-matrix.md`
- `planning/ExecutionRuntime/08-future-phase-reference-designs.md`
- `SavorWorker/SavorWorker.cpp`
- `SavorCore/Runner/Script/PhaseScriptVM.h`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Runner/Runtime/ProgramRuntime`
- `SavorWorkflow/Worker/ProcessWorker.cpp`
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
- `SavorDb/Execution/Workflow/SqliteExecutionDb.cpp`
- `SavorDb/Execution/Workflow/WorkflowCoordinatorService.cpp`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorDb/Execution/Workflow/WorkflowRecoveryService.cpp`
- `SavorDb/Execution/Workflow/SqliteWorkflowOrchestration.cpp`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router`
