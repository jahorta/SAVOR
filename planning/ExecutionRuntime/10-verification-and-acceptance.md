# 10 - Verification and Acceptance

## Scope

This document is a toolbox of checks for implementing the Execution Runtime refactor, migrating current
phases, and deleting the legacy runtime. Use the checks that help develop or diagnose the seam being
changed. Final functional acceptance is the full Release solution build plus production-worker
SavorE2E, not completion of every possible test category listed here.

Current tests describe current behavior. Some encode current ownership or API shape and
will be replaced rather than carried forward as target architecture requirements.

## Purpose and non-goals

Verification should establish, where relevant to the implementation slice:

- ownership invariants, not only happy-path outputs;
- deterministic program control flow under a deterministic backend event trace;
- complete cleanup under return, failure, cancellation, timeout, and injected restoration faults;
- exact module/action/type/state/artifact provenance;
- parity for every supported current phase;
- unchanged SavorDb workflow, queue, persistence, idempotency, and restart behavior;
- safe phase switching through one executor; and
- for future Survey work, the concrete `a101b` bounded-runtime behavior.

This document does not define performance targets, game-specific search quality, or the exact
authored-program source syntax. It does not change SavorDb database layout, stored representations,
database-service interfaces, queues, claims, workflow persistence, transaction boundaries, or
artifact-storage interfaces.

## Current code evidence

The repository has reusable test coverage, but it is divided along current implementation boundaries:

- `SavorTests/test_phase_script_opcodes.cpp` checks the current opcode catalog and builder contracts.
- `SavorTests/test_input_macro_runtime.cpp` checks adaptive macro gates, memory waits, failure mapping,
  authorization, and cleanup.
- `SavorTests/test_navigation_context_framework.cpp` checks capture qualification, execution, registry
  construction, failures, and output ordering.
- `SavorTests/test_navigation_context_codec.cpp` checks portable `.nctx` encoding and malformed input.
- `SavorTests/test_savordb_fixture_sqlite.cpp` checks workflow schema, idempotent dynamic steps, terminal
  advancement, graph routing, BattleSingleTurn next-wave creation, archive/rehydration, and savestate
  persistence.
- `SavorTests/test_savordb_phase3_nonfixture.cpp` checks coordinator materialization, terminal scans,
  restart/replay behavior, concurrency, and workflow unit activation.
- `SavorTests/test_worker_runtime_materialization.cpp` checks worker runtime snapshots and materialization
  reuse.
- Existing phase tests and live captures supply current-output parity evidence.

The target keeps valuable behavior tests but replaces assertions that depend on direct VM breakpoint
replacement, global `PSContext` keys, peer macro execution, or numeric program-kind dispatch.

## Current target decisions

### Test architecture

The implementation shall provide five test surfaces:

1. **Pure contract tests**
   - runtime type schemas, canonical encoding/hash, verifier rules, IR semantics, reducers, and
     deterministic execution ordering;
   - no Dolphin or database required.
2. **Deterministic fake session**
   - a `FakeDolphinBackend` supplies scripted boot, PC stop, memory stop/change, input poll, frame
     boundary, movie end, state load/save, capture, and backend-fault events;
   - the real router, engine, arbiter, state/mutation services, action registry, and program executor run
     over it.
3. **SavorDb boundary regression**
   - current SQLite schema, services, queues, workflow interfaces, and artifact operations with fake
     worker completions;
   - tests program-kind handler translation plus unchanged transactionality, outbox/replay, leases,
     retry, dynamic-step behavior, result persistence, and restart;
   - adds no migration or replacement persistence test model.
4. **Legacy differential harness**
   - the frozen old reference and new runtime receive equivalent source state, inputs, and scripted/live
     observations;
   - compares domain results, persistent artifacts, requested actions, branch trace, input trace, and
     cleanup behavior;
   - exists only through the bounded migration window.
5. **Focused live SavorE2E**
   - uses the production worker, modules, actions, services, and workflow materialization;
   - validates Dolphin integration and game-specific witnesses that a fake backend cannot prove.

A test fixture cannot implement phase behavior on behalf of production code. A custom SavorE2E scenario
is an invocation and assertion harness, not a substitute `NavmeshSurveyRunner`.

### Implemented composition-prelude guards

The production-composition prelude adds a focused SQLite-fixture test that constructs the complete
registry, checks canonical numeric names and all sixteen step-kind mappings, preserves first-wins
`PK_SeedProbe` behavior, rejects missing dependencies without changing the caller's registry, rebuilds
against distinct runtime roots, and verifies that composition creates neither runtime directories nor
database changes.

DB-backed SavorE2E scenarios now use that full production composition while sharing one `DBService`.
Before and after each scenario and repeat, the harness fails closed with workflow/step diagnostics if it
finds Pending or Running workflows, Ready steps, active materialized work, or terminal steps awaiting
reconciliation. A scenario must wait for its own workflow to become terminal; the harness may not
cancel or delete work to pass the gate. `battle_macro_probe` remains a direct-worker macro-development
scenario and does not participate in this DB boundary. Battle End and Navigation Context compile
through the shared factory when selected directly but remain outside the `all` matrix until their live
validation is scheduled.

### Canonical deterministic trace

Fake-session and replay tests that exercise control-flow determinism produce a normalized trace with the
semantic fields needed for comparison:

- invocation, module revision/hash, entrypoint, and dependency closure;
- sequence number and `StateEpoch`;
- program block/instruction or source-map location;
- requested action ID/version and canonical typed inputs;
- resource acquire/release and scope identity;
- execution request and routed stop/interceptor outcome, including logical point, physical evidence, stop
  sequence, and shared routed-hit identity;
- ordered observation acquisition, availability, baseline update, and interaction request/release
  receipts;
- completed action output;
- branch/call/return/fail decision;
- emitted record/artifact identity;
- cancellation/deadline event; and
- final infrastructure, domain, and cleanup/session status.

Host timestamps, OS thread IDs, storage locators, and other declared nondeterministic fields are
normalized out. Given the same verified module, typed invocation, dependency closure, and backend event
trace, the requested-action and branch trace must be byte-for-byte canonical and equal.

An action descriptor must declare any nondeterministic output fields. A program may branch on one only
when the completed observation records it; replay then supplies that recorded value.

### Architecture and dependency tests

Static or link-time architecture checks shall fail when:

- any component other than `ExecutionEngine`/backend implementation calls a Dolphin run or step
  primitive;
- any component other than `PhysicalStopPointManager` manipulates physical breakpoints or memchecks;
- any component other than `InputArbiter` publishes pad state;
- a program, action, reducer, or capability pack imports worker protocol, workflow persistence, or raw
  Dolphin interfaces;
- a program or action creates a private thread/event loop for execution;
- a domain opcode is added to core IR;
- a second interpreter/controller is registered;
- an observation or interaction composer survives lowering as a runtime, scheduler, query VM, opcode
  family, or hidden whole-sequence action;
- `CaptureService` or a capture profile acquires wake/control authority or advances emulation;
- production activation depends on `ProgramKind` or `PK_UserScript`; or
- the legacy interpreter is linked into a post-cutover worker.

Runtime assertions supplement, but do not replace, dependency enforcement.

### Program module and verifier tests

Cover at minimum:

- valid module with every core instruction and structured unwind;
- unknown IR version, action, type, capability pack, entrypoint, or import;
- action signature/version mismatch;
- input/output/emitted-record schema mismatch;
- invalid CFG target, unreachable invalid block, fallthrough, call arity, return type, local type, and
  uninitialized local;
- inconsistent stack/scope shape at a merge;
- action-await resume type mismatch and duplicate/late completion;
- undeclared effect/capability/resource use;
- invalid budget and statically provable unbounded loop;
- canonical hash stability under canonical serialization;
- hash change after any semantic module/dependency change;
- canonical semantic-observation and interaction lowering, including exact imports and source maps;
- hash change after any semantic point, observation, interaction segment, reducer, policy, or emission
  change;
- source-map validation without making source maps execution-authoritative; and
- rejection before boot, state load, input, capture, or guest mutation.

### Semantic-observation composition tests

The reusable semantic-observation library receives focused pure-contract and fake-session coverage:

- capability-pack point identities lower to exact router subscriptions and
  `runtime.execution.continue_until` requests;
- exact alternatives, current-point acceptance, deadlines, rearm/current-instruction suppression, and
  unrelated-stop behavior are preserved;
- `SemanticPointReceipt` records logical identity, PC/memory/synthetic evidence, stop sequence,
  `StateEpoch`, and only the declared bounded hit-time samples;
- `HitTimeSample` executes before program handling within the router's bounded CPU-thread restrictions;
- `PausedAtPoint` reads execute while paused, and post-instruction acquisition occurs only after an
  explicit step and subsequent observation;
- ordered observations stay ordered, while coherent multi-field values use one registered query rather
  than separate reads presented as atomic;
- compatibility-pinned addresses, checked offsets/dereferences, receipt fields, and registered symbols
  resolve through declared `AddressExpression<T>` imports;
- optional unavailability remains distinct from false and zero, while required missing evidence is a
  structured failure;
- named `First` and `Latest` baselines have distinct update behavior, and current battle predicate
  translation updates its `Latest` baseline before evaluation at the same hit;
- receipts, baselines, and guest-derived handles reject stale `StateEpoch` use;
- canonical lowering has stable source maps and exact action/type/capability/emission imports; and
- no `ObservationRuntime`, query VM, observation opcode, direct Dolphin access, filesystem access, or
  database access is introduced.

### Interaction composition tests

The reusable interaction library receives focused pure-contract, fake-session, and migration-parity
coverage:

- static sequences and adaptive initialization/advancement reducers lower to ordinary subprogram CFG,
  verifier-known segment calls, actions, branches, observations, and emissions;
- a reducer may select only a declared segment ID and cannot construct an effect or access services;
- the requested input is published and its epoch obtained before exactly one source instruction is
  executed;
- completion matches logical point, PC, stop sequence, and epoch, while unrelated or stale stops do not
  complete the segment;
- both reached-instruction policies are preserved: leave paused or execute under the held request;
- after held-through-hit execution, the request receipt is read before neutral publication;
- neutral publication alone does not satisfy release: segments that require release use a separately
  named witness with a fresh neutral epoch and guest-poll acknowledgement;
- baseline capture precedes advancement and translated memory-change polling retains one neutral frame
  between polls;
- one input lease spans the full interaction while segment subscriptions and observations use nested
  scopes;
- timeout, VI stall/movie end, unexpected point, unacknowledged request/release, unsatisfied check,
  infrastructure failure, cancellation, and cleanup failure remain distinct;
- normal return and every injected failure/cancellation point unwind once, neutralize input, prove
  release when required, release subscriptions/lease, and taint on mandatory cleanup failure; and
- no `InteractionRuntime`, segment scheduler, macro opcode family, direct Dolphin access, or whole-macro
  action is introduced.

### Predicate composition tests

The reusable predicate library receives focused pure-contract and fake-session coverage:

- one definition is reused at multiple check sites, including a synthetic non-battle module, without a
  runtime change;
- each check consumes typed semantic-observation results and lowers only into canonical IR, declared
  action/type/capability imports, ordinary branches, and declared emissions;
- branch, clean domain rejection, explicit fail, record-only progress, and result accumulation remain
  independent use-site policies;
- `Unsatisfied`, `NotApplicable`, and `Unavailable` remain distinct;
- required observation failure cannot silently become false or be skipped;
- baselines and guest-derived witness state use semantic-observation rules and obey `StateEpoch`;
- type, import, capability, subscription, and emission mismatches reject the module before effects begin;
- normal return, rejection, failure, cancellation, and timeout unwind predicate-related resources through
  the ordinary resource stack;
- deterministic traces expose the observation, evaluation, branch or rejection, and emission; and
- no predicate-specific opcode, executor, runtime service, direct Dolphin access, or physical-breakpoint
  manipulation is introduced.

### ProgramExecutor tests

Cover:

- values, records, lists, arithmetic, comparisons, branch/switch, calls, return, emit, fail, and defer;
- nested subprograms and typed locals;
- one and many sequential action awaits;
- cancellation before execution, during an await, after completion delivery, and during unwind;
- deadline and instruction/effect/emission/artifact budget exhaustion;
- late, duplicated, mismatched, and stale-epoch action completions;
- deterministic trace reproduction;
- zero, one, and many emitted records/artifacts;
- independent infrastructure, domain, and cleanup status;
- pure native reducer transitions and requested effects;
- rejection of a reducer that attempts service/Dolphin ownership; and
- result construction when domain success is followed by cleanup failure.

### StopPointRouter and PhysicalStopPointManager tests

Cover:

- physical union and reference counting across multiple logical consumers;
- observe plus wake plus intercept on one PC;
- priority, consume/pass/replace behavior, and stable ordering;
- source-scoped removal without affecting another source;
- subscription lifetime on normal return, failure, timeout, and cancellation;
- state restore and epoch reconciliation;
- interpreter and JIT matching parity;
- bounded CPU-thread ingress with no blocking/allocation violation;
- visual observer coexistence with a running program; and
- no ordinary execution path that globally clears another consumer's sites.

### ExecutionEngine tests

Cover:

- continue-to-condition, step instruction, step frame, input sequence, pause, and interactive resume;
- every operation routed through interceptors;
- caller wait interrupted by a modal child operation and then resumed;
- remaining deadline/budget accounting across suspension;
- requested completion versus unrelated stop;
- timeout, VI stall, movie end, backend fault, and cancellation;
- child-operation failure propagation;
- visual commands serialized with program execution; and
- exactly one transition into running state at a time.

### InputArbiter tests

Cover:

- lease acquisition, priority, rejection, suspension, resumption, and release;
- suspendable versus unsuspendable owners;
- neutral restoration acknowledged by the guest;
- cancellation and owner destruction;
- input-poll receipt propagation;
- modal dialog borrowing;
- deterministic movie incompatibility as an explicit policy failure; and
- no input publication without an active lease.

The behavior currently checked by `test_input_macro_runtime.cpp` must migrate to these common service,
semantic-observation, interaction, action, reducer, and program tests. It must not remain proof of a peer
macro runtime.

### CaptureService compatibility tests

Characterize every retained `savor.capture.profile/1` behavior before moving it behind
`CaptureService`, then run the same corpus through the extracted service:

- valid and invalid profile parsing, subscription identity, filters/predicate bytecode, address programs,
  activation, and dynamic watchpoints;
- PC-hit and post-write memory sampling with unchanged sample ordering, type conversion, changed-only,
  one-shot, maximum-hit, and other existing sampling policies;
- window open/close/trigger behavior, flight recorders, trace buffers, retention, and artifact
  finalization;
- recorder queues, drops/coalescing, progress formatting/publication, and observable event ordering;
- control flags/metrics, control-triggered windows/recorders, and synthetic control events;
- active-wake-only control publication: an observed routed hit without an active wake/control match does
  not acquire control semantics merely because a capture profile names `control`;
- one routed match carries the same sequence/snapshot/epoch identity through control, capture, progress,
  and emitted artifacts;
- cancellation, deadline, restore, profile detach, normal finalization, backend failure, and cleanup
  fault behavior; and
- profile attach remains passive and cannot manipulate physical stop points, create a foreground wait,
  advance emulation, or grant control authority.

The compatibility oracle is the existing profile behavior and artifacts, not a new `CapturePlan` or
profile-to-IR compiler.

### StateService and StateEpoch tests

Cover:

- boot, load artifact, restore baseline, continue-session guards, and save artifact;
- epoch change on boot/reboot/restore and no change on ordinary reads;
- stale memory pointer, selected-object, worksheet, ground, router, and action handle rejection;
- exact runtime/disc/state compatibility;
- multiple named state handles rather than one hidden VM snapshot;
- immutable state artifact hash, parent, edge, and producer lineage;
- save/load failure and incomplete artifact rejection; and
- no implicit latest or ambient state selection.

### GuestMutationService tests

Cover:

- checked `u8`, `u16`, `u32`, and masked writes;
- expected-original mismatch before mutation;
- written-value readback mismatch;
- masked write proves that no undeclared bit changed;
- nested scopes and reverse-order restoration;
- executable patch pause requirement, alignment, expected word, cache/JIT invalidation, and readback;
- cancellation during an executable enable window;
- restoration failure and session taint;
- state-epoch change while a receipt is live; and
- audit receipt completeness.

For future Survey work, the concrete fixture should cover:

- `u8` value `0` at `0x8030b7ad`;
- enabled word `0x480F86C5` and suppressed word `0x48000018` at `0x80117e8c`;
- a wrong expected word and wrong readback;
- nested door-window unwind back to suppressed state, then outer Survey-scope unwind to the worker's
  recorded original state; and
- cancellation/fault injection at each pause, write, cache/JIT, readback, resume, and restore boundary.

### Action and resource-scope contract tests

Every registered action receives a generated conformance suite from `ActionDescriptor`:

- schema and capability validation;
- bounded completion or deadline;
- cancellation acknowledgement;
- declared determinism/replay behavior;
- permitted `StateEpoch` transition and handle rules;
- exact resources acquired;
- success, domain-negative, infrastructure-failure, and cleanup-failure outcomes;
- no undeclared emissions or side effects; and
- complete trace/receipt production.

Property and fault-injection tests acquire every resource type in varied nested orders, then terminate at
every suspension point by return, fail, cancel, deadline, backend failure, and restore. Expected result:

- every scope receives exactly one unwind attempt;
- unwind occurs in reverse acquisition order;
- independent cleanup failures are all reported;
- mandatory failure yields `Tainted`; and
- a tainted session rejects the next invocation.

### Invocation, protocol, catalog, and replay tests

Cover:

- exact module/revision/hash/entrypoint resolution;
- exact imported action/type/capability closure;
- worker capability negotiation and cached-module hash verification;
- `Boot`, `LoadArtifact`, `RestoreBaseline`, and guarded `ContinueSession`;
- malformed, oversized, unknown-version, missing-schema, and mismatched-runtime requests;
- cancellation and progress correlation by invocation/attempt;
- zero-to-many records and artifacts in one result;
- infrastructure/domain/cleanup status encoding without collapse;
- retry as a new attempt under one semantic invocation;
- immutable artifact publication before reference commit;
- same complete invocation identity plus trace reproduces the same action/branch behavior; and
- any module, action, input, route, state, or policy version change produces distinct provenance.

### Current-phase differential matrix

Every row must pass frozen unit/fake traces and focused live parity before the legacy source is removed:

| Target module/entrypoint | Required parity evidence |
|---|---|
| `soa.seed_probe/probe` | Starting state, input behavior, seed/result records, savestate/artifact output, failure mapping |
| `soa.tas_movie/play_and_checkpoint` | Movie identity, playback stops, checkpoint state/artifact, movie-end and failure behavior |
| `soa.tas_frame_detector/detect` | Detector observations, branch/terminal classification, result fields |
| `soa.battle.legacy_path/run` | Retained legacy behavior and explicit terminal outcomes |
| `soa.battle.context/capture` | Qualification, captured context, state lineage, failure ordering |
| `soa.battle.single_turn/execute` | RNG mutation receipt, semantic point/observation ordering, adaptive interaction trace, request/release acknowledgements, predicate trigger/baseline/comparison, abort/result mapping, passed/total accounting, progress emissions, unchanged capture-profile behavior, outcome, context, output savestate |
| `soa.battle.macro_probe/probe` | Input-before-step synchronization, alternative semantic gates, held-through-hit variants, exact receipt matching, baseline/change waits, release witness, unchanged capture profile, error mapping |
| `soa.battle.completion/complete` | Adaptive interaction timing, completion observations, request/release receipts, emitted state/artifacts, terminal classification |
| `soa.battle.results_screen/advance` | Adaptive interaction timing, input/dialog observations, release witnesses, results capture, terminal state |
| `soa.navigation.context/capture` | Qualification before `.nctx` and savestate publication, exact codec, neutral input, failure atomicity |

Parity compares domain semantics and durable evidence, not old internal breakpoint-set mutation or
`PSContext` layout. Any intentional behavior change requires an explicit versioned decision and new
golden evidence; it cannot be hidden as refactor drift.

### SavorDb integration-boundary tests

Use the current workflow persistence and restart fixtures to prove:

- no schema migration or stored-representation change is required;
- existing persisted jobs feed the correct `ProgramInvocation` through program-kind handlers;
- `ProgramResult` projects through existing result, artifact, and transition operations;
- current payload/result codecs remain usable where stored records require them;
- existing predicate records load and translate through current interfaces/storage, and their result
  projection and survivor-selection behavior remain compatible without migration or conversion;
- existing macro, address-program, and capture-profile representations are translated or consumed in
  memory without migration, conversion, or new storage contracts;
- current queue, claim, affinity, lease, retry, cancellation, outbox, and restart behavior is unchanged;
- current dynamic fan-out, output routing, BattleSingleTurn survivor selection, and next-wave behavior
  remains unchanged;
- current duplicate-completion and stale-attempt behavior remains unchanged; and
- no new workflow/frontier record or transaction model is introduced.

Generalized DFS/BFS/best-first frontier persistence and new typed workflow-binding tests belong to a
separate future SavorDb project.

### Phase-switch isolation scenario

A fake and live-capable `A -> B -> A` scenario shall:

1. have A acquire input, router observation, capture, data-mutation, and epoch-bound handle scopes;
2. end A cleanly and bind its typed output/state explicitly to B;
3. have B use a different capability pack and state policy;
4. end B cleanly and invoke the exact original A module revision again; and
5. snapshot the session resource ledger before and after each invocation.

The second A must see:

- no subscriptions or physical sites attributable to the first A or B;
- no previous input lease and confirmed neutral input;
- no capture/movie handles;
- no remaining guest data/code patch;
- no pending continuation;
- no stale epoch-bound handle; and
- no session taint.

Repeat with cancellation at each A/B await and with one injected cleanup failure. The failure case must
force fresh-session recovery before the next phase.

### Future Navmesh Survey `a101b` scenario

This is a non-gating example for future Survey work. When that work begins, add a custom scenario with
logical ID `execution_runtime.navmesh_survey.a101b.first_slice`. It invokes the production
`soa.navigation.survey` bounded entrypoints through program-kind adapters and existing SavorDb operations
where available, using:

- `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`
- `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

The scenario should prove:

1. every worker reloads the same untouched bootstrap;
2. encounter suppression writes and verifies `0` at `0x8030b7ad`;
3. trigger suppression installs/verifies `0x48000018` at `0x80117e8c`;
4. the door window restores `0x480F86C5`, performs only the bounded interaction, and immediately
   reinstalls `0x48000018`;
5. the selected object through `0x8034744c` is readable and `[object + 8]` identifies TBLID `4101`;
6. the initial lock is observed and only BitVar `2556` at `0x80310c78`, mask `0x10000000`, is cleared;
7. the portal records `initially_locked`, polarity, original value, and override;
8. BitVar `1555` at `0x80310bfc`, mask `0x00080000`, witnesses open/collision-motion completion;
9. the player crosses and settles on the far side;
10. the proposed far-side anchor is replayed from the untouched common bootstrap by teleport and settle;
11. bounded Wave 1 invocations produce the complete accepted/rejected anchor evidence needed before
    Wave 2;
12. bounded Wave 2 invocations run from both initial and far-side anchors and emit immutable spatial
    observations without owning durable fan-out;
13. the versioned deterministic reducer produces one canonical per-area refinement from those exact
    ordered inputs; and
14. cleanup receipts prove restoration or explicitly taint/quarantine the worker.

The scenario may use current workflow/dynamic-step facilities when they already represent the two waves.
It may otherwise drive the bounded invocations and reducer directly for runtime acceptance. A new
persisted barrier, fan-out/fan-in model, binding representation, or workflow transaction is separate
SavorDb/workflow work and is not required by this scenario.

Schema and artifact assertions must prove that:

- no anchor owns a savestate;
- no ground-selector record, worksheet pointer, ground pointer, or other live handle is persisted;
- no inferred probe clock, VI-frame cost, or movement schedule appears in Survey evidence;
- runtime deadlines remain diagnostics only;
- no permanent hook, code cave, breakpoint-based suppression, generalized trigger allowlist, or
  `eventhook` dependency exists;
- the first-slice trigger path is limited to `motscpt`, `wallmot`, and `goscript`; and
- an area load is a separate area artifact/boundary, not an anchor continuation.

Negative variants cover wrong patch precondition, wrong selected TBLID, locked/no-open result, undeclared
BitVar change, missing BitVar `1555`, failure to cross, fall, wrong-side correction, settle failure,
cancellation during the enable window, and cleanup failure. None may publish a positive successor anchor.

### Future-design example matrix

| Design | Runtime proof | Runtime fault proof | Published evidence |
|---|---|---|---|
| `soa.navigation.survey` (`establish_anchors`, `probe_geometry`) | Two bounded entrypoints, exact scoped patches, and handler integration through current SavorDb operations | Cancel/restore/door/settle failures; worker restart | Anchors, portals, spatial observations, refinement; no timing or anchor states |
| `soa.navigation.replay/replay_route` | Exact route/control and module identity yields expected checkpoints | Interruption and divergence are typed; retry from exact source | Replay witnesses, deviations, terminal state when requested |
| `soa.navigation.collision_search/probe_candidates` | One bounded candidate batch | Cancellation/failure does not retain worker-owned topology | Expected/oddity observations and coverage |
| `A -> B -> A` | Same executor and handler-projected state handoff through existing workflow records | Cleanup failure forces fresh session | Resource ledgers and exact phase lineage |
| `soa.cutscene.fast_forward/run_slice` | One bounded strategy slice under router/arbiter | Unsupported/budget/cancel results allow workflow fallback | Slice result, handled events, trace, optional state |
| `soa.overworld.expand/expand_node` | One node expansion emits zero-to-many state children | Cancellation/failure leaves no worker-owned DFS state | Proposed state/edge artifacts and terminal/goal result |

Before domain rules exist, collision, cutscene, and overworld tests may use synthetic capability fixtures.
Their live game acceptance becomes additional validation when those implementations begin.
Restart-safe collision/frontier/DFS orchestration is acceptance for a separate SavorDb/workflow project,
not for the Execution Runtime refactor.

### Build and validation policy

During implementation:

- build the affected SAVOR solution targets with the repository's required MSVC v145 toolchain;
- run focused unit/integration tests for changed ownership, cleanup, concurrency, protocol, verifier, or
  persistence-adapter seams;
- run architecture/invariant checks when a dependency or ownership boundary changes; and
- run parity tests for each affected current phase as it migrates.

Final functional acceptance is the Release solution build and production-worker SavorE2E outcome
summarized below, plus confirmation that no production path selects the retired legacy executor.

Focused SavorTests, differential fixtures, SavorDb regressions, fault injection, and protocol checks are
used when they help implement or diagnose the changed boundary. They are development guards, not an
additional final-acceptance ceremony. Survey joins the E2E corpus only after it is implemented.

### Useful durable artifacts

Repeatable unit-test and build output does not require a separate evidence packet. Preserve durable logs,
normalized traces, reduced comparisons, or fault seeds when they are needed to reproduce:

- a live result that is expensive or environment-dependent;
- a differential mismatch;
- a retry, restart, cancellation, or cleanup fault; or
- a release-cutover failure.

Record only the semantic identities and environment details needed to reproduce that result. Ordinary
development does not require a stage review or metadata inventory.

## Interfaces and ownership affected

Verification requires explicit seams for:

- fake `DolphinBackend`;
- inspectable router physical/logical state;
- `ExecutionEngine` event injection and trace capture;
- `InputArbiter` lease/resource ledger;
- semantic-observation and interaction lowering/source-map inspection;
- `CaptureService` profile-event injection and recorder/artifact inspection;
- `StateService` epoch and state-artifact inspection;
- mutation fault injection and restoration receipts;
- deterministic action registry completions;
- program trace/source-map events; and
- existing SavorDb test controls for program-kind materialization, result projection, workflow restart,
  and artifact operations.

These are observability/test seams around production boundaries, not alternate execution paths. This
refactor requires no new SavorDb database-service, queue, claim, workflow, or artifact-store test
interface.

## Failure and cleanup behavior

- Tests fail closed on missing required typed state, schema mismatch, incomplete cleanup state, or
  unrecognized nondeterminism.
- Flaky timing-based assertions are prohibited where a typed event or state transition can be observed.
- A live timeout may fail the scenario as infrastructure protection; it cannot become Survey timing
  evidence.
- Use targeted fault injection around mutating, suspending, and unwind boundaries where cleanup or
  ownership behavior is otherwise difficult to establish.
- A domain-negative outcome is accepted only when infrastructure completion and cleanup are clean.
- Optional observation unavailability is not silently rewritten as false or zero; required missing
  evidence and stale-epoch evidence fail with their structured classifications.
- Interaction tests keep unexpected point, unacknowledged request/release, unsatisfied check,
  infrastructure failure, cancellation, and cleanup failure distinguishable.
- A tainted result is never reused to make a later test pass; the next invocation must prove fresh-session
  recovery.
- Differential mismatches block legacy deletion until explained and versioned.

## Dependencies and migration implications

- Characterization, contract tests, and fake trace events are added just in time for the affected seam
  before its current direct path is removed.
- Add router, engine, input, state, mutation, capture-compatibility, composition, verifier, and executor
  tests just in time for the direct paths being replaced.
- Each migrated phase adds permanent new-runtime regression coverage; legacy comparison disappears only
  after equivalent behavior is established.
- Use existing SavorDb boundary regressions when changing handler adapters. Any new workflow/frontier
  tests belong to their separate future work.
- The live Survey scenario depends on production Survey implementation and exact bootstrap artifacts, but
  the fake patch/door/state tests can be built earlier.

## Functional acceptance

- The final Release `SAVOR.sln` build passes.
- The production-worker SavorE2E matrix passes with shared-DB quiescence at every participating boundary.
- Current supported behavior remains available through the new runtime.
- No production path links or selects the legacy interpreter or a second controller.
- Semantic observations and interactions lower completely into verified ordinary IR, actions, router
  subscriptions, pure reducers, and emissions; no peer runtime, scheduler, query VM, or opcode family
  remains.
- Existing `savor.capture.profile/1` behavior remains compatible behind passive `CaptureService`, with
  wake/control authority retained by `StopPointRouter` and `ExecutionEngine`.
- Existing SavorDb jobs, results, artifacts, workflows, queues, retries, and restart behavior remain
  compatible without data conversion.
- The refactor adds no SavorDb migration and changes no database-service, queue, claim,
  workflow-persistence, transaction, or artifact-storage interface.

The detailed test sections above are implementation aids for reaching these outcomes. They are not a
requirement to produce a formal evidence packet or run every possible matrix after each slice.

## Deferred work

- Numeric runtime performance, throughput, and memory targets.
- Long-duration soak duration and production telemetry alert thresholds.
- Real collision-oddity objectives and live golden corpus.
- Real cutscene tactic corpus and fastest-safe comparison metric.
- Overworld bounded-expansion rules, state fingerprint, goal corpus, and live node-expansion fixtures.
- Final CI job partitioning and hardware matrix.
- Authoring UI/compiler conformance tests, to be defined with the authored frontend.
- A generalized capture-plan language or replacement for `savor.capture.profile/1`.

Artifact-retention policy and end-to-end durable DFS orchestration tests belong to separate projects;
they are not deferred gates for this refactor.

## Source references

- `planning/ExecutionRuntime/README.md`
- `planning/ExecutionRuntime/02-target-execution-architecture.md`
- `planning/ExecutionRuntime/03-program-modules-ir-and-types.md`
- `planning/ExecutionRuntime/04-actions-effects-and-session-services.md`
- `planning/ExecutionRuntime/05-invocation-result-versioning-and-artifacts.md`
- `planning/ExecutionRuntime/06-workflows-frontiers-and-phase-composition.md`
- `planning/ExecutionRuntime/07-current-phase-migration-matrix.md`
- `planning/ExecutionRuntime/08-future-phase-reference-designs.md`
- `planning/ExecutionRuntime/09-breaking-change-cutover-plan.md`
- `SavorTests/test_phase_script_opcodes.cpp`
- `SavorTests/test_input_macro_runtime.cpp`
- `SavorTests/test_navigation_context_framework.cpp`
- `SavorTests/test_navigation_context_codec.cpp`
- `SavorTests/test_worker_runtime_materialization.cpp`
- `SavorTests/test_savordb_fixture_sqlite.cpp`
- `SavorTests/test_savordb_phase3_nonfixture.cpp`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
