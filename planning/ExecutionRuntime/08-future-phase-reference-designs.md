# 08 - Future Phase Reference Designs

## Scope

This document collects six non-gating reference designs; none of these future programs is implemented.
They are examples and architectural stress tests, not deliverables or completion criteria for the current
Execution Runtime refactor. Their useful constraint is the division between bounded programs, actions,
and session services. Workflow descriptions illustrate that ownership boundary only and do not schedule
changes to SavorDb storage, interfaces, queues, claims, or orchestration.

If Navmesh Survey is implemented, its domain specification comes from
`planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`.
If a Survey detail conflicts, follow that domain document. The current repository code defines
implemented behavior.

The example semantic IDs used here are:

| Design | Program module | Entrypoint |
|---|---|---|
| Navmesh Survey anchor work | `soa.navigation.survey` | `establish_anchors` |
| Navmesh Survey probes | `soa.navigation.survey` | `probe_geometry` |
| Navigation replay | `soa.navigation.replay` | `replay_route` |
| Collision-oddity search | `soa.navigation.collision_search` | `probe_candidates` |
| Cutscene fast-forward | `soa.cutscene.fast_forward` | `run_slice` |
| Overworld exploration | `soa.overworld.expand` | `expand_node` |

In these examples, IDs identify programs, not controllers, and each program would execute through the
same `ProgramExecutor`.

## Purpose and non-goals

The reference designs have two purposes:

1. demonstrate that a new phase can be built from typed programs and reusable actions without another
   worker controller or domain opcode; and
2. make the program/workflow boundary concrete enough that the runtime refactor does not accidentally
   optimize only for current battle phases.

They may inspire later domain work, but implementing or validating any one design is not required to
complete the current refactor.

Every design may reuse the same predicate composition library for checkpoints, progress observations,
qualification, fail-fast requirements, and accumulated evidence. Reuse means lowering pure conditions
and explicit check policies into ordinary IR, actions, scoped router subscriptions, and emissions; none
of these designs receives a predicate controller or runtime.

The designs do not finalize:

- static navigation/refinement algorithms;
- collision anomaly objectives;
- cutscene-specific skip tactics;
- complete overworld mechanics;
- generalized trigger handling;
- authored program syntax or UI; or
- any change to SavorDb SQL/schema, migrations, stored representations, database-service interfaces,
  durable queue/claim contracts, workflow persistence, transaction boundaries, or artifact-storage
  interfaces. Narrow workset-specific coordinator behavior remains governed by the main architecture.

Worker wire representation remains an implementation detail of the runtime refactor. In this document,
**schema** means a runtime program/type schema unless explicitly qualified as a database schema.

Native C++ may implement bounded actions or a pure reducer for these designs. It may not implement a
`NavmeshSurveyRunner`, `CutsceneController`, `OverworldController`, private emulator loop, or another
phase-sized execution path.

### Domain candidate lists and WorkerWorksets

Several reference entrypoints deliberately accept an ordered, bounded **domain candidate list**. That
list belongs to one scalar invocation: the module interprets its elements, applies one domain policy and
budget, and produces one invocation result containing its declared observations or child artifacts.

A `WorkerWorkset` is different. It is the sole production dispatch envelope containing one or more
independently identified scalar invocations whose exact `WorkerWorksetExecutionKey` matches; only multi-item
co-dispatch is optional. The worker does not expose the workset to a module, flatten candidate lists
across invocations, or let one invocation consume another's result. Every child keeps its own
cancellation, failure, cleanup, artifact, and terminal-result boundary.
Consequently, a future program may have a domain candidate list inside each scalar request while several
such requests are independently co-dispatched in one workset.

## Current code evidence

- `SavorCore/Phases/Programs/NavigationContext/NavigationContextScript.h` implements the draft
  Navigation Context capture program, including qualification, context capture, output writing, and a
  matching output savestate.
- `SavorTests/test_navigation_context_framework.cpp` verifies that capture ordering and failure behavior,
  and `SavorTests/test_navigation_context_codec.cpp` verifies the `.nctx` contract.
- The concrete `a101b` `.sav`/`.nctx` export exists outside the repository and is named below.
- `PhaseScriptVM` currently has a direct `WRITE_U32` operation used by BattleSingleTurn for an RNG seed
  override, but there is no implemented scoped byte-write/executable-patch/teleport Survey service.
- The Navigation Context workflow planning package explicitly records that Navmesh Survey, runtime
  refinement, prediction, control solving, and validation are not implemented.
- Current workflow code can append dynamic steps and already uses persisted successor savestates in
  BattleSingleTurn. This is useful precedent for overworld frontier orchestration, but not a complete
  frontier implementation. Any generalized frontier implementation is a separate SavorDb/workflow
  follow-on effort, not part of this refactor.

## Reference design details

### Reference design 1: Navmesh Survey

#### Goal and immutable bootstrap

`soa.navigation.survey` creates runtime-verified spatial evidence for one loaded area: usable positions,
passability, collision boundaries, connectivity, and door portals with access conditions.

The first `a101b` slice uses exactly this common bootstrap:

- savestate:
  `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`
- adjacent Navigation Context:
  `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

Every worker reloads the untouched savestate and consumes the exact adjacent `.nctx`. Neither file is
overwritten. A positional anchor does not own another savestate.

The Survey is per-area. An area load produces a destination/boundary reference for a separate area file;
it does not become a positional anchor that carries live state across files.

#### Program inputs and outputs

`establish_anchors` consumes a bounded `NavigationAnchorTask`:

- exact common bootstrap artifacts and runtime/disc profile;
- exact static world/refinement-candidate identity;
- area identity;
- source anchor position and optional facing;
- one intended door/portal candidate or an ordered bounded domain candidate list;
- expected selected TBLID and known door-specific witnesses;
- known lock BitVar identity/polarity when applicable;
- movement, interaction, settle, and runner-safeguard policies; and
- attempt and provenance identity.

It emits zero-to-many immutable:

- `NavigationSurveyAnchorCandidate`;
- `NavigationDoorPortalObservation`;
- `NavigationRuntimeModificationReceipt`;
- rejected-candidate diagnostics; and
- exact spatial evidence used to reach, cross, and settle.

The existing workflow integration validates, deduplicates, and accepts anchors using current SavorDb
operations. It may schedule additional bounded anchor tasks only through existing dynamic-step
interfaces and representations. If those are insufficient, extending them is a separate workflow
project rather than part of this runtime refactor.

`probe_geometry` consumes a bounded `NavigationProbeBatch`:

- the exact common bootstrap;
- one accepted positional anchor;
- exact static geometry/refinement-candidate identity;
- an ordered, bounded set of spatial probe specifications;
- required Survey modification profile; and
- settle, capture, and safeguard policies.

It emits one immutable `NavigationGeometryObservation` per completed probe plus diagnostics for any
uncompleted probe. It does not update a shared navmesh.

#### Scoped Survey modifications

Survey modifications are named scopes managed through `GuestMutationService`, not raw instructions in
the program:

1. **Encounter suppression**
   - checked `u8` write of value `0` at `0x8030b7ad`;
   - record original byte, requested value, readback, runtime/disc identity, application, and restoration;
   - failure to write or verify fails the job before positive spatial evidence is accepted.
2. **Normal trigger suppression**
   - paused executable-word patch at `0x80117e8c`;
   - expected enabled/original instruction `0x480F86C5` (`bl 0x80210550`);
   - suppressed instruction `0x48000018` (`b 0x80117ea4`);
   - record expected, observed, written, read-back, restored values, state epoch, and instruction-cache/JIT
     invalidation result.
3. **Door activation window**
   - approach and face the intended door while trigger commit is suppressed;
   - pause and restore `0x480F86C5`;
   - run one bounded interaction window;
   - pause and immediately reinstall `0x48000018`;
   - verify readback before continuing the door script;
   - reject the activation if the selected object does not match the intended door.
4. **Known BitVar unlock**
   - use a checked, masked read-modify-write of only the declared bit;
   - record locked/unlocked polarity, original word/bit, override, readback, and restoration;
   - keep the modified live state disposable; only the access-condition evidence is durable.

The executable toggle is not a breakpoint, code cave, permanent hook, or generalized trigger controller.
`eventhook` is outside the first slice. Door behavior is limited to the first-slice path represented by
`motscpt`, `wallmot`, and `goscript`.

#### Door `4101` contract

For the first `a101b` slice:

- the intended door TBLID is `4101`;
- mode-1 interaction selection leaves the selected object pointer at `0x8034744c`;
- the pointer must be non-null and readable, and `[object + 8]` is checked as the selected TBLID;
- generic activity at `0x80347408` is not sufficient identity evidence;
- BitVar `2556` is the lock condition at word `0x80310c78`, mask `0x10000000`, and the Survey clears only
  that bit when the door is initially locked;
- BitVar `1555` is at word `0x80310bfc`, mask `0x00080000`, and is the door-specific witness that the
  open/collision motion completed; and
- selected TBLID, open witness, physical crossing, and far-side settle are all required for success.

The portal records `initially_locked`, the controlling BitVar and polarity, original and override values,
and all activation/crossing evidence. Unlocking the disposable job exposes the complete physical navmesh;
gameplay-aware routing can still enforce the recorded initial access condition.

#### Wave 1: establish anchors

Wave 1 is a workflow-owned, finite anchor-establishment stage. Its integration must use existing SavorDb
workflow/storage contracts:

1. seed the initial positional anchor from the common Navigation Context bootstrap;
2. schedule bounded `establish_anchors` tasks for reachable, untested in-area door candidates;
3. in each task, reload the common bootstrap and reinstall Survey controls;
4. teleport to the task's accepted source anchor when it is not the initial position, then require a
   normal game settle;
5. approach the intended door and use only the bounded activation window;
6. if necessary, record that the door was initially locked, clear only its known BitVar in this
   disposable job, and retry the intended interaction;
7. prove selected TBLID, opening witness, crossing, and stable far-side position;
8. propose the far-side position and portal evidence; and
9. validate that proposed anchor by reloading the untouched common bootstrap, reapplying Survey controls,
   teleporting to the proposed position, and requiring another usable settle.

A wrong TBLID, no-open path, settle correction to the wrong side, fall, snap-away, or failed physical
crossing produces no successor anchor. The domain barrier closes only after every known reachable
in-area portal candidate is accepted, rejected with evidence, or terminally unresolved under the
declared first-slice policy. Implementing a new persisted barrier model is outside this refactor.

Anchor replay relies on the game to rebuild ground/collision selection during ordinary updates. It does
not serialize a ground-selector record, worksheet pointer, ground pointer, or any other live handle.

#### Wave 2: probe geometry

After the Wave 1 barrier:

1. deterministically partition spatial candidates by accepted anchor and bounded domain candidate list;
2. fan out `probe_geometry` invocations;
3. have every worker reload the same common baseline and apply the same Survey controls;
4. teleport to its assigned anchor and require the same settle check;
5. execute only its bounded spatial probe list; and
6. emit immutable observations and cleanup receipts.

The existing workflow integration performs deterministic fan-in where current contracts support it. A
deterministic reducer over exact ordered anchor, observation, static-world, runtime-modification,
algorithm, and coordinate-policy identities emits one `NavigationWorldRefinement` artifact. A
program-kind adapter may publish it only through an existing artifact/domain representation; otherwise
durable publication, or any new persisted barrier, binding, or reduction model, is separate workflow
work.

#### Survey artifact-content rule

Published Survey artifact content is spatial:

- requested, resulting, and settled positions;
- facing when relevant;
- pass, block, fall, slide, correction, warp, interruption, wrong-target, or settle-failure outcomes;
- surface/resource attribution where available;
- connectivity, boundary, portal, and coverage evidence; and
- modification, BitVar, worker, runtime, and attempt provenance.

It contains no inferred probe clock, VI-frame cost, movement schedule, or optimization model. Deadlines,
timeouts, and retry counters remain runtime diagnostics only.

There are no per-anchor savestates, serialized ground-selector records, shared mutable navmesh objects, or
`eventhook` requirement.

### Reference design 2: navigation replay

`soa.navigation.replay/replay_route` executes one exact versioned route/control artifact against one
explicit state artifact or baseline.

Inputs:

- exact route/control artifact and schema;
- exact producer module/model/refinement lineage;
- exact source state artifact and runtime profile;
- expected area/epoch and route checkpoints;
- execution, interruption, capture, and safeguard policies.

Outputs:

- `NavigationReplayResult`;
- ordered verification witnesses and deviations;
- terminal state artifact when requested;
- interruption/transition evidence; and
- exact module, action, route, runtime, and input provenance.

The program performs one bounded replay. It does not choose a newer route, repair the route, retry itself,
or schedule a return navigation. Existing workflow operations may compose outbound and return jobs; the
program-kind handlers translate current stored inputs/results to and from the runtime contracts. No new
stored binding representation is required by this design.

Reproduction requires the same module revision/hash, action versions, route artifact hash, source state,
runtime profile, and policies. A semantically equivalent but differently versioned module is a new run.

### Reference design 3: collision-oddity search

`soa.navigation.collision_search/probe_candidates` evaluates one ordered, bounded domain candidate list
from one exact anchor/state context.

Inputs:

- static geometry/refinement identities;
- exact source baseline/state and positional anchor;
- domain candidate list and candidate-generator version;
- bounded probe strategy;
- anomaly-observer schema; and
- runtime modification and safeguard policies.

Outputs:

- ordinary negative/expected observations;
- zero-to-many typed `CollisionOddityObservation` artifacts;
- optional proposed refinement/search children;
- coverage and diagnostics; and
- reproduction provenance.

The program never performs an unbounded neighborhood search. Ranking unresolved boundaries, generating
follow-up batches, deduplicating equivalent candidates, and determining completion belong outside the
worker. Any new durable frontier policy for that work is a separate orchestration project and is not an
Execution Runtime deliverable.

Collision-oddity evidence remains separate from the first Navmesh Survey contract. A later versioned
policy may promote validated observations into a descendant refinement, but the first Survey does not
become a timing- or anomaly-only search.

### Reference design 4: workflow phase switching

Phase switching is demonstrated with any three modules `A -> B -> A`; there is no phase-switch program.
Existing workflow records and operations select the jobs, while program-kind handlers translate
inputs/results and construct explicit runtime state policy in memory.

Required sequence:

1. A runs and returns with all invocation resources unwound.
2. The existing result/transition handler projects A's outputs and the B handler constructs its exact
   runtime invocation from current persisted data.
3. B starts through the same `ProgramRuntime` from its own exact artifact baseline.
4. B returns and fully unwinds.
5. The A handler invokes the exact original A module revision and entrypoint again.
6. The runtime proves that the second A owns only its newly acquired resource scopes.

The proof checks stop-point subscriptions, physical sites, input leases and neutral state, capture/movie
handles, guest data/code patches, pending continuations, state epoch, and session taint. A cleanup failure
forces a new worker/state load; it never becomes an invisible phase-switch side effect.

### Reference design 5: cutscene fast-forward

`soa.cutscene.fast_forward/run_slice` advances one bounded cutscene strategy slice as quickly as the
declared policy safely permits.

Inputs:

- exact source state or authorized continued session;
- game/runtime profile and cutscene capability pack;
- bounded strategy version;
- permitted dialog/input/skip actions;
- expected control-return, script/context-transition, or terminal witnesses; and
- execution budget and interruption policy.

Outputs:

- `CutsceneSliceResult` with outcome `ControlReturned`, `CutsceneContinues`, `Transitioned`,
  `Unsupported`, `BudgetExhausted`, or `Failed`;
- terminal state artifact when continuation in another job is required;
- handled dialog/interruption events;
- exact input and execution trace; and
- diagnostics/provenance.

Reusable interruption handlers requested by router interceptors and `InputArbiter` actions may dismiss
known dialogs or satisfy input polls. Only `ExecutionEngine` advances Dolphin. A native fast-forward
reducer may choose the next bounded action from completed observations but cannot run a private loop.

If a cutscene exceeds one slice, current dynamic-step facilities may schedule the next slice from the
projected state artifact. If current workflow contracts cannot represent that continuation, adding a new
workflow model is separate future work.

### Reference design 6: overworld arbitrary-wave savestate DFS

This section is a runtime-side bounded-expansion reference only. Its durable DFS stack, visited set,
node/edge lineage, and restart behavior would require follow-on workflow/frontier persistence work. They
are not dependencies, deliverables, or completion checks of the Execution Runtime refactor.

`soa.overworld.expand/expand_node` expands one overworld frontier node. Overworld uses
`soa.overworld` capabilities and rules; it does not fall through to the Dungeon/field Survey model.

Inputs:

- one exact source `StateArtifact`;
- exact overworld context/world/model identities;
- one bounded action/transition domain candidate list;
- expansion, observation, and terminal-policy versions; and
- execution and artifact budgets.

Outputs:

- zero-to-many child `StateArtifact` and `OverworldTransitionEdge` pairs;
- state fingerprints and typed observations available to a follow-on workflow's canonical fingerprint
  policy;
- local goal, terminal, invalid, or dead-end evidence; and
- complete reproduction provenance.

A follow-on durable-workflow effort would own:

- the arbitrary-depth DFS stack;
- visited fingerprints and duplicate edges;
- parent/child lineage;
- child ordering and strategy version;
- goal/pruning rules;
- node claims, retry, and crash recovery;
- wave count and concurrency; and
- final result selection.

Every invocation is bounded regardless of total search depth. Each accepted branch has its own immutable
savestate because overworld search nodes represent branching game state. This does not change the
Navmesh Survey rule: Survey anchors remain positions replayed from one common baseline and do not own
savestates.

### WorkerWorkset applicability

The scalar entrypoints above remain unchanged whether dispatch uses a workset:

| Reference design | Domain work inside one scalar invocation | Expected workset fit |
|---|---|---|
| Navmesh Survey anchor work | One portal or bounded portal candidate list | Strong when independent tasks share the exact untouched bootstrap and runtime/modification policy |
| Navmesh Survey geometry probes | One ordered `NavigationProbeBatch` domain list | Strong when independently ready probe invocations share the exact bootstrap and Survey policy |
| Navigation replay | One route replay | Typically singleton; dependent outbound/return replays are not one workset |
| Collision-oddity search | One bounded collision candidate list | Possible for exact-key independent searches; the coordinator does not merge their candidate lists |
| Cutscene fast-forward | One bounded continuation slice | Typically singleton; successive slices are causally dependent |
| Overworld expansion | One frontier node with one bounded action candidate list | Typically singleton because distinct frontier nodes normally carry distinct state artifacts |

Workset eligibility is determined only by the exact generic key, not by this expectation table or a
domain-family allowlist. Durable Survey partitions, collision refinement, cutscene continuation, and DFS
frontiers remain workflow concerns even when their already-ready scalar invocations use co-dispatch.

## Interfaces and ownership affected

| Capability | Reusable target owner | Used by |
|---|---|---|
| Paused checked data writes | `GuestMutationService` | Survey, battle RNG override, future experiments |
| Reversible executable patches | `GuestMutationService` plus backend cache/JIT support | Survey trigger toggle |
| Teleport and settle | `soa.navigation` actions through `ExecutionEngine` | Survey, collision search |
| Bounded interaction and input | `InputArbiter` plus game actions | Doors, replay, cutscenes, overworld |
| Typed runtime observations | Game capability packs and capture/telemetry services | All designs |
| Predicate Group composition | Module-building library lowering exact Execution Packages to IR, observation actions, scoped router qualifications, and `ConditionObservation` emissions | Survey, replay, collision search, cutscenes, and overworld |
| Savestate load/save and epochs | `SavestateService` | Replay, cutscenes, overworld; Survey baseline reload only |
| Route/control playback | Navigation subprograms/actions | Replay and validation |
| Frontier scheduling | Separate future workflow/frontier project | Collision search and overworld DFS |
| Deterministic reduction | Existing workflow integration where supported; otherwise a separate future workflow project | Survey and collision refinement |

All action requests pass through `ProgramRuntime`; all emulator progress passes through
`ExecutionEngine`; all durable topology remains outside workers. This refactor leaves its SavorDb
representation and interfaces unchanged.

## Failure and cleanup behavior

- Every program has a deadline and operation budget, but Survey publishes no timing inference.
- Every exit path unwinds input, router, capture, movie, state-handle, data-write, and executable-patch
  scopes in reverse acquisition order.
- A failed executable-patch precondition, readback, cache/JIT operation, or restoration taints the session
  and quarantines all positive observations from that invocation.
- A masked BitVar write that changes any undeclared bit invalidates the Survey job.
- A state restore increments `WorksetEpoch`; pre-restore handles, selected-object pointers, and pending
  observations are invalid and must be reacquired.
- Wrong door TBLID, missing open witness, failure to cross, or failed settle yields no successor anchor.
- Replay divergence is a typed domain outcome; transport/emulator failure is an infrastructure outcome.
- A collision probe with no anomaly is a successful negative observation.
- Predicate Group member reaction decides whether `Failed` records progress, changes a domain outcome, branches, or
  accumulates evidence; observation failure is not rewritten as false.
- An unsupported cutscene tactic is a typed result that allows workflow fallback; it is not permission to
  bypass session ownership.
- A bounded overworld expansion cannot own or corrupt a durable DFS stack. Durable retry behavior belongs
  to the separate orchestration project.
- Cleanup failure remains an infrastructure failure even when the domain outcome otherwise succeeded.

## Architectural implications illustrated

Cross-domain predicate reuse adds no executor, opcode, session service, or persistence contract. Its
lowered behavior is visible through the same module hash, dependency closure, router scopes, branch
trace, and emission schemas as equivalent hand-composed IR.

- If implemented, each design would use the universal module/IR, action ABI, invocation/result, and
  resource-scope contracts in documents 02 through 06, plus the fixed SavorDb boundary in document 06.
- Survey would additionally require checked `u8`/masked writes, reversible executable patching with readback
  and cache/JIT handling, teleport/settle actions, selected-object/TBLID observation, and spatial artifact
  schemas.
- Navigation replay would require input sequences and interruption handling to be ordinary
  `ExecutionEngine` operations.
- Cutscene fast-forward would depend on reusable dialog/interruption capabilities; it must not revive a
  separate input-macro runtime.
- End-to-end Overworld DFS would depend on follow-on workflow/frontier persistence work. It is not a
  dependency of the Execution Runtime refactor.
- None of these designs adds a worker controller, `ProgramKind` switch arm, or core domain opcode.

Navmesh Survey is the intended first new program after the migration, but it is not a completion condition
for this refactor. If implemented, it should target the new architecture directly rather than the legacy
interpreter.

## Example validation scenarios

These checks illustrate how each design could be validated when implemented. They do not gate completion
of the current backend refactor.

### Navmesh Survey example first slice

- Load the exact `navigation-context-41.sav` and adjacent `.nctx` without modifying them.
- Apply checked encounter suppression at `0x8030b7ad`.
- Install and verify `0x48000018` at `0x80117e8c`, briefly restore `0x480F86C5` only for the intended
  door interaction, and suppress immediately afterward.
- Verify selected-object TBLID `4101` through `0x8034744c` and `[object + 8]`.
- Detect the initial lock, clear only BitVar `2556` at `0x80310c78` mask `0x10000000`, and retain
  `initially_locked` plus original/override evidence.
- Observe BitVar `1555` at `0x80310bfc` mask `0x00080000`, cross, and settle on the far side.
- Replay the far-side anchor from the untouched common bootstrap by position teleport and settle.
- Demonstrate bounded Wave 2 probe invocations from the initial and far-side anchors; end-to-end
  workflow fan-out uses current SavorDb contracts or is separate follow-on work.
- Emit one deterministic per-area refinement artifact without per-anchor savestates, ground-selector
  records, timing evidence, or `eventhook`; durable publication uses an existing representation or is
  separate follow-on work.

### Cross-design architecture checks

- Navigation replay reproduces the same action and branch trace from the same complete invocation
  identity and reports checkpoint deviation explicitly.
- Outbound and return navigation compose through exact state/route artifacts without a special runner.
- Collision search executes one bounded domain candidate list without owning restart/deduplication
  topology.
- `A -> B -> A` leaves zero resources from either earlier invocation.
- Cutscene fast-forward can continue across bounded slices and be inserted or removed from a workflow
  without worker-runtime changes.
- Overworld expansion executes one bounded node without owning a DFS stack or visited set.
- If implemented, each runtime-side design uses program modules, runtime type schemas, reusable actions
  where genuinely needed, and program-kind integration through existing SavorDb contracts.

Restart-safe collision/frontier/DFS orchestration belongs to follow-on workflow work and is not a check
for this backend refactor.

## Deferred work

- Generalized trigger identities, allowlists, automatic trigger-boundary characterization, and
  `eventhook`.
- Initialization/environmental controllers and broader MovingObject/platform behavior.
- Exact teleport settle tolerances and candidate partition sizes.
- Collision-oddity objective, scoring, refinement-promotion, and sampling policy.
- Route/control artifact schema and return-navigation selection policy.
- Cutscene tactic catalog, supported dialog decisions, and fastest-safe policy.
- Overworld action set, fingerprint inputs, goal conditions, child ordering, and parallel DFS width.
- Exact local CPU executor used for deterministic refinement reduction.

## Source references

- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/03-navigation-context-contract.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
- `SavorCore/Phases/Programs/NavigationContext/NavigationContextScript.h`
- `SavorCore/Phases/Programs/NavigationContext/NavigationContextPayload.cpp`
- `SavorCore/Runner/Script/PhaseScriptVMMemory.cpp`
- `SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
- `SavorTests/test_navigation_context_framework.cpp`
- `SavorTests/test_navigation_context_codec.cpp`
