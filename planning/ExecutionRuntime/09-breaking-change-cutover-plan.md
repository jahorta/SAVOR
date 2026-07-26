# 09 - Breaking-Change Cutover Plan

## Scope

This document describes dependency slices, the compatibility window, production cutover, and legacy
deletion required by the Execution Runtime refactor. Breaking changes are expected. The slices are an
adaptable implementation order, not approval gates, release numbers, or calendar commitments.

Repository code defines the current system. Documents 02 through 08 provide the target constraints and
design direction; implementation may adapt their concrete shape while preserving the core boundaries.

## Purpose and non-goals

The plan replaces the current worker execution path with one universal typed runtime without leaving a
permanent second controller behind. It is ordered to establish ownership and test seams before migrating
domain behavior.

This plan does not:

- require backward-compatible worker-internal C++ APIs or worker messages;
- change SavorDb SQL/schema, migrations, stored representations, database-service interfaces, queues,
  claims, workflow persistence, transaction boundaries, or artifact-storage interfaces;
- reinterpret or rewrite existing persisted payload/result bytes; program-kind handlers continue to
  support them through the current storage contracts;
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
2. The temporary legacy translator accepts existing in-memory `PhaseScript` builders and produces the
   canonical target IR. It does not execute old opcodes and is not a worker controller.
3. A test-only reference harness may run the old binary/interpreter for differential evidence during a
   bounded migration window. Production dispatch never selects between old and new by job, flag, module,
   or `ProgramKind`.
4. New features, including Navmesh Survey, are prohibited on the legacy path.
5. An active `ProgramInstance`, resource scope, `StateEpoch`, Dolphin session, or in-flight worker command
   is never migrated across runtime versions.
6. Existing persisted payloads and results remain in their current representation. Program-kind handlers
   translate them at the runtime boundary; they are not rewritten as stored `ProgramModule` or
   `ProgramInvocation` records.
7. Coordinator, worker protocol, module catalog, action/type registries, and runtime-facing program-kind
   adapters deploy as one compatibility-checked application release set.
8. Rollback means stopping the release and restoring the matched application build. No database rollback
   or schema state is part of this refactor, and rollback never enables an old interpreter inside the new
   worker.
9. SavorDb storage, database-service interfaces, queue/claim contracts, workflow persistence, and
   transaction boundaries remain unchanged throughout the cutover.

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
| TAS playback/checkpoint | `soa.tas_movie/play_and_checkpoint` |
| TAS frame detector | `soa.tas_frame_detector/detect` |
| Legacy battle path | `soa.battle.legacy_path/run` |
| Battle Context | `soa.battle.context/capture` |
| BattleSingleTurn | `soa.battle.single_turn/execute` |
| BattleMacroProbe | `soa.battle.macro_probe/probe` |
| Battle completion | `soa.battle.completion/complete` |
| Battle results screen | `soa.battle.results_screen/advance` |
| Navigation Context | `soa.navigation.context/capture` |

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
and focused behavior tests may remain as translation evidence for later slices, but no production worker
command reaches it. The old worker tags remain disconnected and fail locally rather than emitting legacy
frames.

The Slice 1 worker advertises session lifecycle, screenshot, host-event, cancellation-protocol, and
shutdown capabilities, but not `ProgramInvocation` or interactive visual debugging. Pause, resume, and
step requests fail as unsupported until `ExecutionEngine` owns them in slice 3. Ordinary visual rendering
and host-event publication may continue through the standard session and protocol paths.

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

### Dependency slice 2: single physical stop-point ownership and routing

**Implement:**

- introduce `PhysicalStopPointManager` as the sole Dolphin PC breakpoint/memcheck owner;
- introduce `StopPointRouter` with logical subscription groups, source identity, delivery mode, priority,
  lifetime, guards, routing history, and epoch behavior;
- translate current VM waits, capture observers, native probe hooks, and visual/debug observers into
  subscriptions; and
- reconcile physical sites after boot, restore, JIT changes, or subscription changes.

**Remove during this slice:**

- `armed_singleton` and flat globally enabled-PC ownership;
- routine `clearAllPcBreakpoints`/`clearMemoryWatchpoints` scope cleanup; and
- any program's ability to replace another consumer's physical set.

**Completion checks:**

- the physical set equals the union of logical subscriptions;
- observe, wake, intercept, and guard consumers coexist at one PC;
- interpreter/JIT paths report equivalent routed events;
- restore advances the epoch and reconciles sites without stale delivery.

### Dependency slice 3: ExecutionEngine as sole emulator-advancement owner

**Implement:**

- introduce typed continue, instruction-step, frame-step, input-sequence, pause, and interactive-resume
  operations;
- route every operation through the same control-thread event loop and router;
- represent timeout, VI stall, movie end, cancellation, requested completion, and intercepted stops as
  structured results;
- support suspended child/interceptor operations with explicit remaining budgets; and
- move current run-until, frame, opcode, tape, and macro advancement beneath the engine; and
- restore interactive visual pause, resume, and step by routing them through the same serialized engine
  control path.

**Completion checks:**

- repository search finds no Dolphin run/step call outside `ExecutionEngine`/backend implementation;
- every advancement mode remains interceptor-aware;
- cancellation and modal interruption tests pass at every suspension boundary.

### Dependency slice 4: input, state, mutation, capture, and scoped-resource services

**Implement:**

- implement `InputArbiter` leases, priority, suspension, neutral restoration, and guest-observed
  acknowledgement;
- implement `StateService` with explicit baseline/state handles, immutable state artifacts, and monotonic
  epoch invalidation;
- implement checked `GuestMutationService` data writes, masked writes, reversible executable patches,
  preconditions, readback, cache/JIT handling, restoration receipts, and taint;
- extract movie, screenshot, and telemetry services;
- place the existing `savor.capture.profile/1` parser and all current profile execution semantics behind
  passive `CaptureService`, without translating profiles into program IR or inventing a replacement
  capture language;
- introduce the shared structured scope/defer stack used by all effects; and
- introduce modular game capability packs rather than one growing game facade.

`CaptureService` preserves current subscriptions, filters/predicate bytecode, address programs,
activation/dynamic watchpoints, PC and post-write sampling, sampling policies and ordering,
one-shot/max-hit behavior, windows, flight recorders, trace buffers, queue/drop/coalescing behavior,
progress, event ordering, and artifact finalization. `StopPointRouter` and `ExecutionEngine` own all
wake/control authority. Capture observes their matched routed event so legacy profile `control`
subscriptions, control flags/metrics, control-triggered windows/recorders, and synthetic control events
keep their existing meaning under one sequence/snapshot/epoch identity.

**Completion checks:**

- input, router subscriptions, captures, movies, state handles, data writes, and code patches all unwind
  on return, failure, timeout, and cancellation;
- injected restoration failures taint and retire the session;
- stale epoch-bound handles fail closed;
- BattleSingleTurn's RNG override is expressed through the generic mutation service; and
- compatibility tests cover every retained capture-profile policy, while no profile grants control or
  creates a foreground wait.

### Dependency slice 5: canonical typed ProgramRuntime

**Implement:**

- implement `ProgramDefinitionStore`, `ProgramVerifier`, `ProgramExecutor`, `ActionRegistry`, and
  `TypeSchemaRegistry` under `ProgramRuntime`;
- implement immutable `ProgramModule`, typed CFG/basic-block IR, functions/subprograms, core instructions,
  structured unwind, and one action-await boundary;
- implement `ProgramInstance` as data containing control stack, locals, pending continuation, scopes,
  epoch, emissions, and result construction;
- validate exact imported action/type/capability closure before activation;
- implement the three-axis `ProgramResult`;
- support C++ builders as one frontend that emits canonical modules; and
- add reusable semantic-observation, interaction, and predicate composition libraries to that
  builder/frontend surface.

Semantic observation defines capability-pack-owned `SemanticPointDefinition`, `SemanticAwaitDefinition`,
`SemanticPointReceipt`, `AddressExpression<T>`, `ObservationDefinition<T>`, and `ObservationUse<T>`.
It lowers exact point alternatives, bounded hit-time samples, paused reads/registered coherent queries,
explicit post-step behavior, required/optional evidence, baselines, and epoch checks into ordinary IR,
actions, scoped router subscriptions, and emissions.

Interaction composition defines versioned `InteractionDefinition<State, Output>`, a finite
verifier-known segment set, pure initialization/advancement reducers, and typed
`InteractionSegmentResult`. It lowers static and adaptive input sequences into ordinary subprogram CFG,
semantic awaits/observations, input/execution actions, branches, and emissions. Predicate composition
consumes typed observation results and lowers pure conditions plus explicit `Check` policies through the
same surface. All three composers finish lowering before `ProgramVerifier` validates the resulting
module and exact dependency closure.

**Completion checks:**

- verifier rejection cases and deterministic executor traces pass;
- no domain action is represented by a new core opcode;
- `ProgramExecutor` is the only program-flow scheduler;
- actions and reducers cannot access Dolphin or create private loops; and
- all three composers expose effects, subscriptions, branches, and emissions through the ordinary
  verified dependency closure and contain no observation, interaction, or predicate opcode, executor,
  runtime service, query VM, scheduler, or hidden controller.

### Dependency slice 6: legacy translation and current-phase migration

**Implement:**

- implement a temporary compatibility compiler from the supported current `PhaseScript` builder subset
  into canonical typed IR;
- reject any legacy operation that cannot be translated with exact semantics;
- translate current stop keys/alternatives, direct reads, address programs, registered queries, and
  baselines through semantic-observation composition without changing their persisted representations;
- express input-macro `Start`/`Advance` behavior through interaction definitions, pure reducers, and
  verifier-known segments under the universal executor;
- preserve input-before-source-step, exact point/PC/stop-sequence/epoch matching, held-through-hit
  policy, request-receipt-before-neutral ordering, separately witnessed neutral release,
  baseline-before-advance, and one-neutral-frame memory polling;
- translate existing Battle Single Turn predicate records at the runtime-facing adapter/module-builder
  boundary into the shared predicate composition library without changing their stored representation;
- express baseline capture, comparison, abort-on-fail behavior, passed/total accumulation, and progress
  reporting through ordinary observations and explicit check policy rather than VM-global predicate
  state;
- migrate each current phase to the stable module identities above; and
- run old-reference/new-runtime differential tests plus focused live parity.

Existing capture-profile artifacts continue to pass unchanged through `runtime.capture.attach` to
`CaptureService`; current profile parsing and behavior are characterized and preserved rather than
lowered or redesigned during phase migration.

**Migration order:**

1. SeedProbe, to establish basic branching, memory, input, state, and result parity.
2. Navigation Context, to establish qualification, capture, artifact, and matching-savestate parity.
3. TAS frame detection and TAS playback/checkpoint, to establish movie and checkpoint behavior.
4. Battle Context, to establish richer capture and emitted data.
5. BattleMacroProbe, to establish shared semantic-observation and interaction composition, adaptive
   reducer transitions, exact macro timing, and request/release acknowledgement.
6. BattleSingleTurn, to absorb generic mutations, capture, local restore/epoch behavior, shared predicate
   composition, complex results, and durable turn waves.
7. Battle completion and results-screen advancement.
8. The legacy battle path, after its retained behavior and retirement boundary are explicit.

The compatibility compiler is a migration tool, not the public authored format. Once a program receives
a native typed builder, that builder becomes its source and legacy translation is retained only until
the differential window closes.

**Completion checks:**

- every current phase satisfies document 07's parity and deletion criteria;
- the same exact module/dependency identity produces the expected action/branch trace;
- current stop/address/query/baseline behavior executes through semantic-observation composition with
  equivalent acquisition, ordering, availability, and epoch semantics;
- current input macros execute through interaction composition with equivalent temporal ordering,
  acknowledgement, memory-wait, and cleanup semantics;
- Battle Single Turn preserves predicate triggering, baseline and comparison semantics, explicit
  unavailable-observation handling, abort/result mapping, passed/total accounting, progress reporting,
  and scoped cleanup through the shared observation/predicate libraries;
- current capture profiles retain parsing, sampling, control-observation, window/recorder, progress, and
  artifact behavior behind passive `CaptureService`; and
- no new production job requires legacy VM execution.

### Dependency slice 7: invocation/result and handler-adapter cutover

**Implement:**

- activate the Slice 1 encoded module/invocation protocol envelopes with exact `ProgramInvocation`;
- replace flat/global-context result mapping with typed `ProgramResult` and immutable artifact references;
- update program-kind handlers or adjacent integration adapters to construct `ProgramInvocation` from
  existing persisted job/domain data;
- update those handlers/adapters to project `ProgramResult` through existing result writers, artifact
  operations, and transition handlers;
- keep current persisted `ProgramKind`, payload/result codecs, job identity, affinity, queue, claim, and
  workflow representations while removing `ProgramKind` from worker execution selection; and
- negotiate protocol/runtime/module compatibility after existing claim/materialization and before worker
  activation or guest-state mutation.

**Cutover preparation:**

1. publish the final module/action/type catalog for the release;
2. verify adapter coverage for every currently stored and queued program-kind payload/result version;
3. verify every existing workflow definition continues to use its current persistence representation;
4. stop new worker activations and drain running or claimed executions for the application deployment;
5. prove that existing queued jobs rematerialize the correct target runtime invocation without rewriting
   their rows;
6. prove that typed runtime results write the same existing domain/result representation; and
7. deploy coordinator and workers as one matched application set.

Existing job payload/result bytes remain valid production data. Program-kind handlers may retain their
codecs solely to translate that representation into/out of runtime types; those codecs do not execute or
select the legacy interpreter.

**Completion checks:**

- workers reject protocol, module, dependency, and runtime-profile mismatches before state mutation;
- existing jobs, queues, workflows, results, artifacts, retries, and restart behavior remain compatible
  without a database migration or record conversion;
- program-kind handlers project through existing SavorDb interfaces and transaction boundaries; and
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

**Delete or retire from production:**

- `PhaseScriptVM` as an interpreter/controller;
- the central legacy opcode dispatch and domain opcodes;
- `ProgramRegistry` construction and payload-decoding switches;
- `PSContext` as public invocation/result ABI;
- the peer `InputMacroRuntime` execution path and VM macro-host inheritance;
- any temporary direct-read/stop/macro path superseded by semantic-observation and interaction
  composition;
- numeric `ProgramKind` worker dispatch and any planned `PK_UserScript` path;
- direct program ownership of Dolphin, breakpoint sets, input, capture, or one hidden snapshot; and
- the temporary compatibility compiler after all native typed builders and archival readers no longer
  need it.

Existing payload/result codecs may remain behind SavorDb program-kind handlers to translate current
persisted records during production materialization and result writing. They cannot select or invoke the
legacy worker interpreter.

**Completion checks:**

- repository architecture tests fail if a second executor/controller or forbidden dependency is added;
- no executable production path can instantiate the old interpreter;
- all supported program-kind handlers construct exact runtime invocations and consume typed runtime
  results through existing SavorDb contracts.

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
| Opcode table | Legacy translation input temporarily; domain opcodes deleted |
| `PSContext` | Removed as the worker runtime ABI; a codec may remain behind SavorDb handlers where existing stored records require it |
| Current stop/read/address-program/query/baseline helpers | Translate in memory through semantic-observation composition; generated behavior uses router subscriptions, execution actions, guest reads or registered coherent queries, ordinary values, and emissions |
| `InputMacroRuntime` peer engine | Existing plans/providers translate in memory through interaction definitions, pure reducers, verifier-known segments, and common subprograms/actions |
| `PhaseScriptVM` predicate table and evaluator | Existing records translate at the module-builder boundary; predicates consume semantic-observation results and generated execution uses ordinary IR, actions, branches, and emissions; the legacy evaluator is deleted with the VM |
| `savor.capture.profile/1` | Representation and semantics remain unchanged behind passive `CaptureService`; router/engine retain wake and control authority |
| SavorDb program-kind handler implementations | Adapt existing records to/from runtime contracts without changing their interfaces or storage |
| Workflow lifecycle/outbox/recovery | Preserved unchanged; no typed-binding or frontier persistence is added |
| Existing and historical jobs/artifacts | Stored representation remains unchanged; no conversion |

## Failure and cleanup behavior

- During development, a failed new-runtime differential test falls back to investigation, not per-job
  production selection of the old VM.
- A protocol/catalog mismatch rejects worker activation after the current claim/materialization flow and
  before boot/load/mutation.
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
  -> scoped services
  -> universal program runtime
  -> current-phase migration
  -> invocation/result handler-adapter cutover
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

- The final Release `SAVOR.sln` build and production-worker SavorE2E matrix pass.
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
- In-flight legacy executions are drained/canceled rather than migrated.
- Exact invocation/result/module compatibility is enforced after current claim/materialization and before
  worker activation or guest-state mutation.
- No SavorDb migration, stored-representation change, database-service/queue/claim/workflow-interface
  change, transaction-boundary change, or artifact-storage-interface change is introduced.
- The legacy interpreter, public `PSContext` ABI, peer macro engine, and program-kind execution switch are
  absent from production after dependency slice 8.
- Rollback changes only the matched application release, leaves existing data untouched, and never
  enables dual runtime dispatch.

Focused unit, concurrency, fault-injection, architecture, and differential tests are development tools
for risky seams and migration mismatches. They inform implementation but are not a separate release
approval process.

Intermediate hard-cutover slices are accepted by solution compilation plus focused guards for the seam
being changed. Production-worker SavorE2E is intentionally unavailable after slice 1 and is run for final
functional acceptance only after `ProgramRuntime`, current-program migration, and handler adapters restore
the complete production path.

## Deferred work

- Exact commit/PR grouping and deployment calendar.
- Final C++ namespaces, file layout, and encoded module/invocation payload schemas beyond the fixed Slice
  1 `WRMS` frame header.
- Duration of the differential window, subject to phase parity rather than a calendar alone.
- Performance tuning and worker-pool sizing after correctness cutover.
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
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorDb/Execution/Workflow/WorkflowRecoveryService.cpp`
- `SavorDb/Execution/Workflow/SqliteWorkflowOrchestration.cpp`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router`
