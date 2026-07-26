# 09 - Breaking-Change Cutover Plan

## Status and authority

**Status:** Authoritative implementation order and cutover policy; not yet executed.

This document defines the dependency order, stage gates, compatibility window, production cutover, and
legacy deletion required by the Execution Runtime refactor. Breaking changes are expected. Stage names
describe implementation outcomes rather than release numbers or calendar commitments.

Repository code is authoritative for the current system. Documents 02 through 08 are authoritative for
the target that each stage must satisfy.

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

## Locked target decisions

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

Before Stage 0, the current DB-facing program catalog was centralized in
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

### Stage 0: evidence freeze and executable contract skeleton

**Build:**

- record the source commit and current phase/program corpus;
- freeze golden payload/result/artifact samples and focused live fixtures;
- define logical module, IR, action, invocation, result, resource, artifact, and epoch contracts plus the
  unchanged workflow-integration boundary from documents 03 through 06;
- define stable semantic module IDs and entrypoints;
- define protocol compatibility and dependency-verification error categories; and
- create the deterministic fake backend/event-trace vocabulary used throughout migration.

**Required current module identities:**

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

**Exit gate:**

- every contract field and ownership rule is represented in tests or an executable test fixture;
- every current phase has frozen parity inputs, observable outputs, and required side effects;
- no unresolved IR or ownership decision is delegated to Stage 5.

### Stage 1: WorkerRuntime and EmulationSession seam

**Build:**

- separate process/transport concerns from one serialized `WorkerRuntime` command actor;
- make `WorkerRuntime` own exactly one explicit `EmulationSession`;
- route visual control, cancellation, screenshots, and job commands through that actor;
- narrow `DolphinBackend` to backend capabilities rather than policy; and
- introduce typed command/result, cancellation-token, session-disposition, and `StateEpoch` primitives.

Program execution may temporarily call the old VM behind this seam, but the old VM may not receive
external commands directly.

**Exit gate:**

- no pipe-reader, visual-control, telemetry, or background thread directly mutates Dolphin or VM state;
- command serialization and cancellation races pass deterministic concurrency tests;
- boot, shutdown, error, and cancellation leave a known session disposition.

### Stage 2: single physical stop-point ownership and routing

**Build:**

- introduce `PhysicalStopPointManager` as the sole Dolphin PC breakpoint/memcheck owner;
- introduce `StopPointRouter` with logical subscription groups, source identity, delivery mode, priority,
  lifetime, guards, routing history, and epoch behavior;
- translate current VM waits, capture observers, native probe hooks, and visual/debug observers into
  subscriptions; and
- reconcile physical sites after boot, restore, JIT changes, or subscription changes.

**Remove during this stage:**

- `armed_singleton` and flat globally enabled-PC ownership;
- routine `clearAllPcBreakpoints`/`clearMemoryWatchpoints` scope cleanup; and
- any program's ability to replace another consumer's physical set.

**Exit gate:**

- the physical set equals the union of logical subscriptions;
- observe, wake, intercept, and guard consumers coexist at one PC;
- interpreter/JIT paths report equivalent routed events;
- restore advances the epoch and reconciles sites without stale delivery.

### Stage 3: ExecutionEngine as sole emulator-advancement owner

**Build:**

- introduce typed continue, instruction-step, frame-step, input-sequence, pause, and interactive-resume
  operations;
- route every operation through the same control-thread event loop and router;
- represent timeout, VI stall, movie end, cancellation, requested completion, and intercepted stops as
  structured results;
- support suspended child/interceptor operations with explicit remaining budgets; and
- move current run-until, frame, opcode, tape, and macro advancement beneath the engine.

**Exit gate:**

- repository search finds no Dolphin run/step call outside `ExecutionEngine`/backend implementation;
- every advancement mode remains interceptor-aware;
- cancellation and modal interruption tests pass at every suspension boundary.

### Stage 4: input, state, mutation, capture, and scoped-resource services

**Build:**

- implement `InputArbiter` leases, priority, suspension, neutral restoration, and guest-observed
  acknowledgement;
- implement `StateService` with explicit baseline/state handles, immutable state artifacts, and monotonic
  epoch invalidation;
- implement checked `GuestMutationService` data writes, masked writes, reversible executable patches,
  preconditions, readback, cache/JIT handling, restoration receipts, and taint;
- extract movie, capture, screenshot, and telemetry services;
- introduce the shared structured scope/defer stack used by all effects; and
- introduce modular game capability packs rather than one growing game facade.

**Exit gate:**

- input, router subscriptions, captures, movies, state handles, data writes, and code patches all unwind
  on return, failure, timeout, and cancellation;
- injected restoration failures taint and retire the session;
- stale epoch-bound handles fail closed;
- BattleSingleTurn's RNG override is expressed through the generic mutation service.

### Stage 5: canonical typed ProgramRuntime

**Build:**

- implement `ProgramDefinitionStore`, `ProgramVerifier`, `ProgramExecutor`, `ActionRegistry`, and
  `TypeSchemaRegistry` under `ProgramRuntime`;
- implement immutable `ProgramModule`, typed CFG/basic-block IR, functions/subprograms, core instructions,
  structured unwind, and one action-await boundary;
- implement `ProgramInstance` as data containing control stack, locals, pending continuation, scopes,
  epoch, emissions, and result construction;
- validate exact imported action/type/capability closure before activation;
- implement the three-axis `ProgramResult`; and
- support C++ builders as one frontend that emits canonical modules.

**Exit gate:**

- verifier rejection cases and deterministic executor traces pass;
- no domain action is represented by a new core opcode;
- `ProgramExecutor` is the only program-flow scheduler;
- actions and reducers cannot access Dolphin or create private loops.

### Stage 6: legacy translation and current-phase migration

**Build:**

- implement a temporary compatibility compiler from the supported current `PhaseScript` builder subset
  into canonical typed IR;
- reject any legacy operation that cannot be translated with exact semantics;
- express input-macro `Start`/`Advance` behavior as ordinary continuations, subprograms, actions, or pure
  reducers under the universal executor;
- migrate each current phase to the stable module IDs from Stage 0; and
- run old-reference/new-runtime differential tests plus focused live parity.

**Migration order:**

1. SeedProbe, to establish basic branching, memory, input, state, and result parity.
2. Navigation Context, to establish qualification, capture, artifact, and matching-savestate parity.
3. TAS frame detection and TAS playback/checkpoint, to establish movie and checkpoint behavior.
4. Battle Context, to establish richer capture and emitted data.
5. BattleMacroProbe, to establish the common adaptive reducer/action continuation and input
   acknowledgement.
6. BattleSingleTurn, to absorb generic mutations, capture, local restore/epoch behavior, predicates,
   complex results, and durable turn waves.
7. Battle completion and results-screen advancement.
8. The legacy battle path, after its retained behavior and retirement boundary are explicit.

The compatibility compiler is a migration tool, not the public authored format. Once a program receives
a native typed builder, that builder becomes its source and legacy translation is retained only until
the differential window closes.

**Exit gate:**

- every current phase satisfies document 07's parity and deletion criteria;
- the same exact module/dependency identity produces the expected action/branch trace;
- current input macros execute through the common action/continuation path;
- no new production job requires legacy VM execution.

### Stage 7: invocation/result protocol and handler-adapter cutover

**Build:**

- replace worker activation with exact `ProgramInvocation`;
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

**Exit gate:**

- workers reject protocol, module, dependency, and runtime-profile mismatches before state mutation;
- existing jobs, queues, workflows, results, artifacts, retries, and restart behavior remain compatible
  without a database migration or record conversion;
- program-kind handlers project through existing SavorDb interfaces and transaction boundaries; and
- no `ProgramKind` value selects worker execution behavior after materialization.

### Separate follow-on: workflow/frontier generalization

Generalized typed workflow policies, persisted frontier nodes/edges/leases, new barrier models,
DFS/BFS/best-first scheduling, persisted deduplication, and new workflow transaction shapes are not a
stage or prerequisite of this refactor. They require separate planning and approval under the SavorDb
migration/workflow planning surfaces.

This refactor establishes only the negative worker boundary: a bounded program cannot mutate durable
topology or keep an unbounded search frontier in worker memory. Existing SavorDb workflow and dynamic-step
behavior remains unchanged.

### Stage 8: remove the legacy execution path

**Delete or retire from production:**

- `PhaseScriptVM` as an interpreter/controller;
- the central legacy opcode dispatch and domain opcodes;
- `ProgramRegistry` construction and payload-decoding switches;
- `PSContext` as public invocation/result ABI;
- the peer `InputMacroRuntime` execution path and VM macro-host inheritance;
- numeric `ProgramKind` worker dispatch and any planned `PK_UserScript` path;
- direct program ownership of Dolphin, breakpoint sets, input, capture, or one hidden snapshot; and
- the temporary compatibility compiler after all native typed builders and archival readers no longer
  need it.

Existing payload/result codecs may remain behind SavorDb program-kind handlers to translate current
persisted records during production materialization and result writing. They cannot select or invoke the
legacy worker interpreter.

**Exit gate:**

- repository architecture tests fail if a second executor/controller or forbidden dependency is added;
- no executable production path can instantiate the old interpreter;
- all supported program-kind handlers construct exact runtime invocations and consume typed runtime
  results through existing SavorDb contracts.

### Stage 9: Navmesh Survey as the first net-new client

**Build:**

- implement `soa.navigation.survey/establish_anchors`;
- implement `soa.navigation.survey/probe_geometry`;
- add only genuinely reusable `soa.navigation` actions and generic mutation capabilities;
- integrate through existing SavorDb workflow/dynamic-step operations where they are sufficient, without
  changing storage or interfaces; and
- validate the bounded runtime portions of the exact `a101b` slice defined in documents 08 and 10, using
  a harness or unchanged existing SavorDb contracts.

Survey must not cause changes to `WorkerRuntime`, `ProgramRuntime`, `ProgramExecutor`,
`ExecutionEngine`, core IR, router ownership, or a program-kind switch. A need for such a change is an
architecture acceptance failure unless it exposes a demonstrably general missing primitive and this
guidance set is revised before implementation.

**Exit gate:**

- the bounded-runtime `a101b` acceptance scenario passes;
- Survey evidence remains spatial;
- no per-anchor savestates, serialized ground-selector state, permanent hook, or `eventhook` dependency
  exists; and
- end-to-end durable two-wave orchestration is not a Stage 9 gate unless current SavorDb contracts already
  support it.

### Separate follow-on: authored-program frontend

After the universal runtime, migrated phases, legacy deletion, and Survey acceptance, a separately
approved project may:

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
| `InputMacroRuntime` peer engine | Folded into common continuations/actions/subprograms |
| SavorDb program-kind handler implementations | Adapt existing records to/from runtime contracts without changing their interfaces or storage |
| Workflow lifecycle/outbox/recovery | Preserved unchanged; no typed-binding or frontier persistence is added |
| Existing and historical jobs/artifacts | Stored representation remains unchanged; no conversion |

## Failure and cleanup behavior

- A stage cannot advance with a failing exit gate; partial architecture is not declared complete.
- During development, a failed new-runtime differential test falls back to investigation, not per-job
  production selection of the old VM.
- A protocol/catalog mismatch rejects worker activation after the current claim/materialization flow and
  before boot/load/mutation.
- Running legacy worker executions are drained before deployment; persisted and queued jobs remain in
  place and are translated by the updated program-kind handlers.
- A worker that fails mandatory cleanup is tainted and retired; retry starts in a fresh/known session.
- No database or artifact migration occurs. Application rollback leaves existing records untouched.

## Dependencies and migration implications

The stages are intentionally ordered:

```text
contracts
  -> serialized worker/session ownership
  -> physical stop ownership/router
  -> execution ownership
  -> scoped services
  -> universal program runtime
  -> current-phase migration
  -> invocation/protocol handler-adapter cutover
  -> legacy deletion
  -> first new phase
```

Stages may be developed in parallel only where their interfaces are already frozen, but their exit gates
must pass in this order. In particular:

- ProgramRuntime cannot compensate for unresolved emulator ownership.
- Current phase migration cannot retain direct Dolphin escape hatches.
- A bounded expansion program cannot schedule its own durable children; any generalized frontier work is
  a separate project.
- Survey cannot become the justification for a phase-specific controller.

## Acceptance criteria

- Every stage has all prerequisite gates, produced capabilities, removals, and evidence recorded.
- A production release contains one program executor and one emulator-advancement owner.
- All current supported phases use the stable module IDs and entrypoints listed in Stage 0.
- Differential evidence covers every migrated phase before its legacy source is removed.
- Current workflows retain transactional transitions, retries, outbox delivery, restart recovery, and
  dynamic-wave behavior.
- In-flight legacy executions are drained/canceled rather than migrated.
- Exact invocation/result/module compatibility is enforced after current claim/materialization and before
  worker activation or guest-state mutation.
- No SavorDb migration, stored-representation change, database-service/queue/claim/workflow-interface
  change, transaction-boundary change, or artifact-storage-interface change is introduced.
- The legacy interpreter, public `PSContext` ABI, peer macro engine, and program-kind execution switch are
  absent from production after Stage 8.
- Navmesh Survey is delivered only through the universal runtime and passes document 08.
- Rollback changes only the matched application release, leaves existing data untouched, and never
  enables dual runtime dispatch.

## Deferred work

- Exact commit/PR grouping and deployment calendar.
- Final C++ namespaces, file layout, and worker frame encoding.
- Duration of the differential window, subject to all phase gates rather than a calendar alone.
- Performance tuning and worker-pool sizing after correctness cutover.

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
