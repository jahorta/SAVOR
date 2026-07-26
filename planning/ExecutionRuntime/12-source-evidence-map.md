# 12 - Source Evidence Map

## Status and authority

**Status:** Authoritative traceability index for the Execution Runtime guidance set.

**Repository snapshot:** SAVOR commit
`b584920ffad8dbe770f343e532d7f7386c82fadf`.

**Commit timestamp and subject:** `2026-07-25T14:22:17-05:00`,
`Model combatant auxiliary visual publication`.

**Inspection date:** 2026-07-25.

Current code is the highest authority for current implementation behavior. This package is the authority
for the agreed target architecture. The Navigation Context workflow package is the authority for
Navmesh Survey domain behavior. Historical planning and the external breakpoint-router analysis remain
evidence and rationale only where this map does not explicitly supersede them.

Relevant current source paths were clean at the inspected `HEAD`; the worktree also contained unrelated
user edits and working Navigation Phase planning edits. Consequently:

- implemented-code citations below refer to the recorded commit unless explicitly stated otherwise; and
- Navigation Phase planning citations refer to the inspected working-tree text on 2026-07-25.

The production-composition prelude is an explicit post-snapshot implementation recorded from the
working tree on 2026-07-25. Evidence IDs IC-061A through IC-061C and the updated IC-090, IC-105, and
IC-106 rows below refer to that implementation rather than to the recorded snapshot commit.

## Purpose and non-goals

This map:

- gives stable evidence IDs to important current-state claims;
- distinguishes code, observed artifacts, target decisions, and unresolved research;
- records exact files, symbols, and inspected line ranges;
- records negative evidence for the unimplemented Survey;
- identifies conflicts among prior documents and declares which decision governs; and
- supplies a discrepancy protocol for later implementation work.

It is not a substitute for reading source before editing it. Line numbers are snapshot anchors and may
move. The symbol and behavior must be rechecked against then-current code.

## Evidence classes and precedence

| Class | Meaning | What it can prove |
|---|---|---|
| **Implemented code** | Compiled source, tests, registration, schema, or adapter present in the repository snapshot | What the current system can attempt to execute and how its code is structured |
| **Observed runtime evidence** | Concrete run artifact or durable output inspected on disk | That the exact artifact exists with the recorded properties; not that every producing path is correct |
| **Target decision** | Normative decision in this Execution Runtime package | What the refactor must implement; not that it exists now |
| **Unresolved research** | Explicitly deferred question or incomplete game/runtime characterization | A boundary the implementer must not silently invent |

Precedence for conflicts:

1. current code for current behavior;
2. concrete runtime artifact for the properties of that artifact;
3. this package for target worker/program architecture;
4. Navigation Context workflow planning for Survey domain behavior;
5. DB workflow planning for retained durable-orchestration behavior;
6. external analyses and superseded plans as research/history.

Tests support a source claim but do not override the implementation they exercise. A planning document
never proves that a feature is implemented.

## Current code evidence

### Implemented-code evidence

#### Worker, activation, and wire path

| ID | Current-state claim | Source anchor | Classification |
|---|---|---|---|
| IC-001 | The worker boots one Dolphin host, builds one runtime breakpoint map, and constructs one `PhaseScriptVM`. | `SavorWorker/SavorWorker.cpp:227-240`, `main` worker setup | Implemented code |
| IC-002 | The main worker loop handles `SET_PROGRAM`, `RUN_INIT_ONCE`, `ACTIVATE_MAIN`, and `JOB` serially. | `SavorWorker/SavorWorker.cpp:345-505` | Implemented code |
| IC-003 | The worker's `init_prog` is default-constructed and is not populated from `WireSetProgram::init_kind`; `RUN_INIT_ONCE` executes it only when nonempty. | `SavorWorker/SavorWorker.cpp:355-397` | Implemented code |
| IC-004 | A visual-control thread directly calls VM pause/step and host pause/resume/frame-step APIs, bypassing a single command actor. | `SavorWorker/SavorWorker.cpp:272-342` | Implemented code |
| IC-005 | Job payload decoding is selected by active numeric program kind; result context is encoded after `vm.run`. | `SavorWorker/SavorWorker.cpp:438-505` | Implemented code |
| IC-006 | The wire protocol has fixed lifecycle/job tags, 8-bit `ProgramKind`, one timeout/derived-buffer/savestate-path activation structure, and a one-context-blob result. | `SavorCore/Runner/IPC/Wire.h:8-18`, `:58-83`, `:93-149` | Implemented code |
| IC-007 | Current program kinds are `0` through `10`; kind `8` has a source-compatibility Battle End Results alias. | `SavorCore/Runner/IPC/Wire.h:63-83` | Implemented code |
| IC-008 | `ProcessWorker::ctl_set_program` is the parent-side sender used by coordinator and direct E2E activation. | `SavorWorkflow/Worker/ProcessWorker.cpp`, `ProcessWorker::ctl_set_program`; `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp:1438-1451`; `SavorE2E/BattleMacroProbeScenario.cpp` | Implemented code |

#### Program registry and legacy program model

| ID | Current-state claim | Source anchor | Classification |
|---|---|---|---|
| IC-020 | `ProgramRegistry` reconstructs one fixed `PhaseScript` through a switch on `ProgramKind`. | `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-70`, `build_main_program` | Implemented code |
| IC-021 | A second switch validates the payload tag and calls one program-specific decoder into `PSContext`. | `SavorCore/Phases/Programs/ProgramRegistry.cpp:76-108`, `decode_payload_for` | Implemented code |
| IC-022 | Retry-tuning metadata is another program-kind selection and is absent for several executable kinds. | `SavorCore/Phases/Programs/ProgramRegistry.cpp:110-151`, `get_retry_tuning_info` | Implemented code |
| IC-023 | `PhaseScript` is two breakpoint-key vectors plus a flat operation vector; `PSInit`, `PSJob`, and `PSResult` are small unversioned runtime structures. | `SavorCore/Runner/Script/PhaseScriptProgram.h:13-40` | Implemented code |
| IC-024 | The opcode catalog contains 52 numbered entries, `0` through `51`, including both generic control operations and domain operations. | `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc:6-57` | Implemented code |
| IC-025 | One dispatch switch handles every operation, including battle context, macro materialization, TAS sampling, seed capture, and Navigation Context. | `SavorCore/Runner/Script/PhaseScriptVM.cpp:170-332`, `PhaseScriptVM::dispatch_op` | Implemented code |
| IC-026 | `PSResult::ok` is derived at return from `DW_RUN_OUTCOME_CODE`, while the domain return value is inserted into the context. | `SavorCore/Runner/Script/PhaseScriptVMControl.cpp:158-159`, `op_emit_result`, `op_return_result` | Implemented code |

#### VM ownership, state, context, and macros

| ID | Current-state claim | Source anchor | Classification |
|---|---|---|---|
| IC-040 | VM init cancels a macro, clears watchpoints, may load a disk savestate, mutates breakpoint state, and captures one in-memory baseline snapshot. | `SavorCore/Runner/Script/PhaseScriptVM.cpp:125-167`, `PhaseScriptVM::init` | Implemented code |
| IC-041 | Every VM job restores that baseline before interpretation; most state-based scripts also begin with `LOAD_SNAPSHOT`. | `SavorCore/Runner/Script/PhaseScriptVM.cpp:335-396`; current `*Script.h` builders | Implemented code |
| IC-042 | Job exit has local RAII cleanup for memory watchpoints and probe capture. | `SavorCore/Runner/Script/PhaseScriptVM.cpp:345-365` | Implemented code |
| IC-043 | `PhaseScriptVM` directly calls host APIs for state, physical breakpoints/watchpoints, running/stepping, input, movies, capture, guest memory, and game context. | `SavorCore/Runner/Script/PhaseScriptVM*.cpp`; representative anchors `PhaseScriptVMControl.cpp:58-143`, `:162-429`, `PhaseScriptVMInput.cpp:15-194`, `PhaseScriptVMMacro.cpp:136-424`, `PhaseScriptVMMemory.cpp:13-105`, `PhaseScriptVMNavigation.cpp:107-122` | Implemented code |
| IC-044 | `PhaseScriptVM` privately implements the macro host interfaces and owns one `InputMacroRuntime` plus an adaptive plan driver. | `SavorCore/Runner/Script/PhaseScriptVM.h:35-38`, `:106-118`, `:205-246` | Implemented code |
| IC-045 | Adaptive macro drivers expose `Start`, `Advance`, and `Cancel`, with the latest stop and guest-memory reads supplied through their host. | `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:12-59` | Implemented code |
| IC-046 | `InputMacroRuntime` validates plans, executes one action at a time, and has cleanup-once semantics. | `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp:45-213`, `:277-440` | Implemented code |
| IC-047 | Macro cleanup neutralizes input, clears macro watchpoints, releases its VM-local exclusive session, and restores breakpoint state. | `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp:416-427` | Implemented code |
| IC-048 | The VM implements macro exclusivity by replacing the complete enabled-PC set and directly driving input and execution. | `SavorCore/Runner/Script/PhaseScriptVMMacro.cpp:136-225` | Implemented code |
| IC-049 | `PSContext` is a global 16-bit-key map over a closed variant that includes generic scalars plus `GCInputFrame` and battle `BattlePath`. | `SavorCore/Runner/Script/PSContext.h:10-44`; `ContextKeys/KeyIds.h:8-18` | Implemented code |
| IC-050 | `PSContextCodec` serializes that closed set; an unsupported in-memory variant is skipped during encoding and unknown wire types are skipped during decoding. | `SavorCore/Runner/Script/PSContextCodec.cpp:18-78`, `:90-158` | Implemented code |

#### DB descriptors, affinity, and durable workflow behavior

| ID | Current-state claim | Source anchor | Classification |
|---|---|---|---|
| IC-060 | `ProgramKindDescriptor` combines worker kind with job persistence, graph persistence, runtime init/materialization, result mapping/writing, and workflow transition. | `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:14-184` | Implemented code |
| IC-061 | `ProgramKindRegistry` indexes descriptors both by numeric program kind and workflow step kind. | `SavorDb/Execution/ProgramDB/ProgramKindRegistry.h:13-58` | Implemented code |
| IC-061A | One atomic production builder owns the complete six-family registration order, canonical numeric winners, sixteen step-kind mappings, required capabilities, and production default roots. | `SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.h`; `ProductionProgramKindRegistry.cpp` | Implemented working-tree prelude |
| IC-061B | SavorQt and each DB-backed SavorE2E registry construction use the complete production builder while retaining explicit scenario config overrides. | `SavorQt/SavorDbRuntime.cpp`, `SavorDbRuntime::buildProgramRegistry`; DB-backed scenario files under `SavorE2E` | Implemented working-tree prelude |
| IC-061C | SavorE2E shares one `DBService` and checks existing workflow query state before and after each DB-backed scenario and repeat; Battle Macro Probe bypasses this gate. | `SavorE2E/DbSetup.cpp`, `CheckWorkflowQuiescence`; `SavorE2E/Main.cpp` | Implemented working-tree prelude |
| IC-062 | Materialization resolves the descriptor by step kind, builds `RuntimeInitRequest`, materializes `PSJob`, and derives savestate/bootstrap affinity. | `SavorWorkflow/Execution/JobMaterializationService.cpp:180-224`, `:330-363` | Implemented code |
| IC-063 | A warm worker is reused if program kind, runtime affinity, and savestate affinity match; otherwise it is reconfigured and activated. | `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp:1397-1476`, `EnsureWorkerProgramForJob` | Implemented code |
| IC-064 | Affinity ranking prefers matching savestate, then program kind, then runtime profile. | `SavorWorkflow/Execution/JobMaterializationService.cpp:513-550`, `BetterMaterializedDispatchCandidate` | Implemented code |
| IC-065 | `WorkflowTransitionDecision` can carry zero-to-many dynamic child steps. | `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:120-140` | Implemented code |
| IC-066 | Terminal advancement persists dynamic child steps transactionally through `AppendDynamicSteps`. | `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp:244-303` | Implemented code |
| IC-067 | Battle Context transitions append initial `battle.single_turn` work. | `SavorDb/Execution/ProgramDB/BattleContext/BattleContextProbeAdapters.cpp:691-761` | Implemented code |
| IC-068 | Battle Single Turn reduction selects candidates, creates next waves, and appends subsequent `battle.single_turn` steps. | `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp:1594-1652` | Implemented code |
| IC-069 | SeedProbe transitions expand chain work into grid and unique phases. | `SavorDb/Execution/ProgramDB/SeedProbe/SeedProbePhaseRegistration.cpp:172-218`; `SeedProbeGridAdapters.cpp:50-82` | Implemented code |

#### Implemented program families

| ID | Family and evidence | Source anchor | Classification |
|---|---|---|---|
| IC-080 | SeedProbe fixed program and payload | `SavorCore/Phases/Programs/SeedProbe/SeedProbeScript.h:12-74`; `SeedProbePayload.h`; DB registration `SeedProbePhaseRegistration.cpp:223-256` | Implemented code |
| IC-081 | TAS playback fixed program and payload | `SavorCore/Phases/Programs/PlayTasMovie/TasMovieScript.h:13-44`; `TasMoviePayload.h`; `TasMoviePhaseRegistration.cpp:5-23` | Implemented code |
| IC-082 | TAS input-stream detector fixed program and payload | `SavorCore/Phases/Programs/TasFrameDetector/TasFrameDetectorScript.h:7-25`; `TasFrameDetectorPayload.h` | Implemented code |
| IC-083 | Legacy multi-turn battle-path fixed program and payload | `SavorCore/Phases/Programs/BattleRunner/BattleRunnerScript.h:33-111`; `BattleRunnerPayload.h` | Implemented code |
| IC-084 | Battle Context fixed program, payload, and workflow descriptor | `SavorCore/Phases/Programs/BattleContext/BattleContextScript.h:9-28`; `BattleContextPayload.h`; `BattleContextProbePhaseRegistration.cpp:5-21` | Implemented code |
| IC-085 | Battle Single Turn fixed program, payload, and workflow descriptor | `SavorCore/Phases/Programs/BattleTurnRunner/BattleTurnRunnerScript.h:42-207`; `BattleTurnRunnerPayload.h`; `BattleSingleTurnPhaseRegistration.cpp:5-21` | Implemented code |
| IC-086 | Battle Macro Probe fixed program/payload and direct-worker E2E activation | `SavorCore/Phases/Programs/BattleMacroProbe/BattleMacroProbeScript.h:10-67`; `BattleMacroProbePayload.h`; `SavorE2E/BattleMacroProbeScenario.cpp` | Implemented code |
| IC-087 | Battle Completion fixed program, BCMB manifest, and workflow descriptor | `SavorCore/Phases/Programs/BattleCompletion/BattleCompletionScript.h:11-57`; `BattleCompletionManifest.h`; `BattleEndResultsPhaseRegistration.cpp:7-54` | Implemented code |
| IC-088 | Battle Results Screen fixed program, BERB report, and workflow descriptor | `SavorCore/Phases/Programs/BattleEndResults/BattleEndResultsScript.h:11-80`; `BattleEndResultsReport.h`; `BattleEndResultsPhaseRegistration.cpp:7-54` | Implemented code |
| IC-089 | Navigation Context fixed program, result taxonomy, payload, NCTX extraction/codec, and workflow descriptor | `SavorCore/Phases/Programs/NavigationContext/NavigationContextScript.h:11-129`; `NavigationContextResult.h`; `NavigationContextPayload.h`; `SavorCore/Core/Memory/Soa/Navigation/NavigationContextCodec.cpp`; `NavigationContextPhaseRegistration.cpp:7-23` | Implemented code |
| IC-090 | SavorQt obtains TAS, SeedProbe, Battle Context, Battle Single Turn, Battle End, and Navigation Context DB descriptors from the shared production composition source. | `SavorQt/SavorDbRuntime.cpp`, `SavorDbRuntime::buildProgramRegistry`; `SavorDb/Execution/ProgramDB/ProductionProgramKindRegistry.cpp` | Implemented working-tree prelude |

#### Tests that define the current parity corpus

| ID | Covered behavior | Source anchor | Classification |
|---|---|---|---|
| IC-100 | Opcode catalog/default program contracts | `SavorTests/test_phase_script_opcodes.cpp` | Implemented code |
| IC-101 | Macro sequencing, validation, receipts, failure, and cleanup-once | `SavorTests/test_input_macro_runtime.cpp` | Implemented code |
| IC-102 | Battle macro compiler, fake attacks, payloads, and Battle Single Turn program shape | `SavorTests/test_battle_macro_probe.cpp` | Implemented code |
| IC-103 | Completion/results artifacts, providers, scripts, and DB workflow | `SavorTests/test_battle_end_results.cpp`; `test_battle_end_results_db.cpp` | Implemented code |
| IC-104 | Navigation Context extraction, codec, program execution, registry, and DB workflow | `SavorTests/test_navigation_context_codec.cpp`; `test_navigation_context_framework.cpp`; `test_navigation_context_db.cpp` | Implemented code |
| IC-105 | Dynamic workflow-step persistence, battle multi-wave behavior, and side-effect-free full production-registry construction | `SavorTests/test_savordb_fixture_sqlite.cpp`, including `ProductionProgramKindRegistryBuildsCompleteCatalogAtomicallyWithoutSideEffects` plus the snapshot tests beginning at lines 695, 799, 865, 981, 1130, and 1348 | Implemented code plus working-tree prelude |
| IC-106 | Real-worker/E2E phase surfaces use full production DB composition and quiescent shared-service boundaries; Battle Macro Probe remains direct-worker-only | `SavorE2E/SeedProbeRealWorkerScenario.cpp`; `TasMovieRealWorkerScenario.cpp`; `BattleMacroProbeScenario.cpp`; `BattleSingleTurnScenario.cpp`; `BattleEndResultsScenario.cpp`; `NavigationContextScenario.cpp`; `DbSetup.cpp`; `Main.cpp` | Implemented working-tree prelude |

## Negative implementation evidence: Navmesh Survey

| ID | Claim | Evidence and method | Classification |
|---|---|---|---|
| IC-120 | Navmesh Survey is not implemented in the inspected snapshot. | Repository search outside planning found no `NavmeshSurvey`, `establish_anchors`, `probe_geometry`, `NavigationSurveyAnchor`, or `NavigationGeometryObservation` implementation. `Wire.h` ends at kind `10`; `ProgramRegistry` ends at Navigation Context; no Survey program directory or DB descriptor exists. | Implemented code |
| IC-121 | The current hidden overworld/dungeon workflow entries are placeholders, not traversal or Survey implementations. | `SavorDb/Execution/Workflow/WorkflowComposition.cpp:337-375` gives each empty `internal_step_kinds` and a one-step placeholder template; inspected Navigation workflow README states the same. | Implemented code |
| IC-122 | Navigation Context is a bootstrap, not Survey. | `NavigationContextScript.h` only captures context and a matching state. No anchor, door, teleport, spatial-probe, or refinement operation exists in the current program. | Implemented code |

This negative evidence is time-bounded to the recorded snapshot. A later implementation must replace
these entries rather than treating them as permanent truth.

## Observed runtime evidence

The approved first Survey bootstrap pair was inspected directly:

| ID | Artifact | Observed properties | Classification |
|---|---|---|---|
| OR-001 | `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav` | Length `18,515,547`; last write `2026-07-24T06:46:02.8896327-05:00`; SHA-256 `EBACEACAA383D6C5B74C39F8B281D8C53F60F9C95D5E9FDB13FD2BDD0BBD4222` | Observed runtime evidence |
| OR-002 | Adjacent `navigation-context-41.nctx` | Length `86`; last write `2026-07-24T06:46:02.9621874-05:00`; SHA-256 `847672875958D030336E91DF6F538739D7BB3E260C38DE84A47AE24566CB47B1` | Observed runtime evidence |

These observations prove that the exact files existed with these bytes at inspection. They do not prove
Survey behavior, universal reproducibility, or correctness of a future consumer.

## Research and planning evidence

### Breakpoint-router analysis

| ID | Research claim retained | Source anchor | Classification |
|---|---|---|---|
| RP-001 | Physical breakpoints and every form of emulator advancement need one session owner; callers submit logical subscriptions/requests. | `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt:8-15` | Target decision, corroborated by code |
| RP-002 | The useful top-level session services are `WorkerRuntime`, `EmulationSession`, `ExecutionEngine`, `StopPointRouter`, `PhysicalStopPointManager`, memory, input, state, movie, capture, telemetry, and game runtime. | Same file `:214-236` | Target decision, adapted by this package |
| RP-003 | Current VM responsibilities and macro ownership are too broad. | Same file `:87-169`, especially `:134-168`; corroborated by IC-040 through IC-050 | Implemented code |
| RP-004 | Session, physical-stop, router, execution, and input-arbiter extraction stages are viable. | Same file `:731-769` | Target decision, retained |
| RP-005 | Fake backend/router/execution/input/capture tests are appropriate. | Same file `:812-840` | Target decision, retained and expanded |

### Navigation Context and Survey planning

| ID | Domain claim | Source anchor | Classification |
|---|---|---|---|
| RP-020 | Survey consumes the named `.sav` and adjacent `.nctx`; every worker reloads the common baseline; anchors are positions, not savestates. | `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md:21-32`, `:151-160` | Target decision |
| RP-021 | Encounter suppression is a checked byte-zero write at `0x8030b7ad`. | Same file `:36`, `:76-79` | Target decision based on reverse-engineering evidence |
| RP-022 | Trigger suppression toggles `0x80117e8c` between enabled `0x480F86C5` and suppressed `0x48000018`; `eventhook` is outside the first slice. | Same file `:37-41`, `:82-96` | Target decision based on reverse-engineering evidence |
| RP-023 | The first door path covers `motscpt`, `wallmot`, and `goscript`. | Same file `:40-41` | Target decision |
| RP-024 | Door `4101` uses selected-object evidence at `0x8034744c`, lock BitVar `2556` at `0x80310c78/0x10000000`, and completion witness BitVar `1555` at `0x80310bfc/0x00080000`. | Same file `:135-146` | Target decision based on reverse-engineering evidence |
| RP-025 | Wave 1 establishes and replay-validates positional anchors; Wave 2 reloads the baseline, teleports/settles, and probes spatial geometry. | Same file `:122-170`, `:278-287`; `06-phase-job-and-artifact-integration.md:73-90` | Target decision |
| RP-026 | Persistent Survey evidence is spatial and excludes inferred timing and serialized ground-selector state. | `04-suppressed-exploration-and-world-refinement.md:159-160`, `:225-236`, `:286-303` | Target decision |
| RP-027 | Only deterministic domain reduction publishes the per-area refinement. | Same file `:244-303`; `06-phase-job-and-artifact-integration.md:89-100` | Target decision |

### Prior user-script plan

| ID | Historical proposal | Source anchor | Classification |
|---|---|---|---|
| RP-040 | The plan correctly identifies separation among immutable definitions, payload schemas, workflow handlers, and worker execution packages; this package retains it in generalized form. | `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md:37-51` | Target decision |
| RP-041 | It proposes serializing current `PhaseScript` as the canonical execution representation; CF-004 supersedes that proposal. | Same file `:85-105`, `:152-171` | Target decision |
| RP-042 | It proposes generic request/result `PSContext` and program-specific compatibility paths; CF-006 supersedes that proposal. | Same file `:107-150`, `:238-263` | Target decision |
| RP-043 | It proposes `PK_UserScript` or a second script activation path; CF-005 supersedes that proposal. | Same file `:75-83`, `:126-150`, `:254-263` | Target decision |
| RP-044 | It correctly keeps DB lookup and payload-source resolution out of the worker executor; this package retains that boundary. | Same file `:152-171`, `:201-222` | Target decision |

## Locked target decisions

### Target-decision map

| ID | Locked decision | Authoritative document | Classification |
|---|---|---|---|
| TD-001 | One `WorkerRuntime`, one `ProgramExecutor`, and one `ExecutionEngine`; `ProgramInstance` is state, not a controller. | `README.md`; `02-target-execution-architecture.md` | Target decision |
| TD-002 | Canonical execution is a verified typed CFG/basic-block IR with a small stable core and named entrypoints. | `03-program-modules-ir-and-types.md` | Target decision |
| TD-003 | Native extension is limited to bounded registered actions or pure reducers; effects use scoped session services. | `04-actions-effects-and-session-services.md` | Target decision |
| TD-004 | Module/invocation identity includes immutable revision/hash, entrypoint, and exact dependency closure; results split infrastructure, domain, and cleanup status. | `05-invocation-result-versioning-and-artifacts.md` | Target decision |
| TD-005 | Programs are bounded; existing SavorDb workflows retain durable phase switching, waves, deduplication, and recovery through unchanged contracts. Generalized frontier persistence is separate work. | `06-workflows-frontiers-and-phase-composition.md` | Target decision |
| TD-006 | Current phases migrate to the canonical module IDs and deletion gates in the migration matrix; macros become common continuations, not a peer engine. | `07-current-phase-migration-matrix.md` | Target decision |
| TD-007 | Survey, replay, collision search, cutscene acceleration, phase switching, and bounded overworld expansion all use the same runtime boundary; new durable topology is separate workflow work. | `08-future-phase-reference-designs.md` | Target decision |
| TD-008 | A bounded translator may support differential testing, but production cutover deletes the legacy executor and dual activation paths. | `09-breaking-change-cutover-plan.md` | Target decision |
| TD-009 | Verification includes static verifier tests, deterministic fake-session traces, fault/unwind injection, differential parity, unchanged SavorDb boundary regressions, and focused E2E. | `10-verification-and-acceptance.md` | Target decision |
| TD-010 | `ProgramKind` may remain SavorDb job/handler/transition/queue/affinity and semantic metadata, but cannot select worker execution; built-in and authored programs compile to the same module; no `PK_UserScript`. | `11-decisions-risks-and-deferred-work.md` | Target decision |
| TD-011 | Capability packs are independently versioned as `soa.battle`, `soa.field`, `soa.navigation`, `soa.cutscene`, and `soa.overworld`. | `README.md`; `04-actions-effects-and-session-services.md`; `11-decisions-risks-and-deferred-work.md` | Target decision |
| TD-012 | The first net-new phase using existing capabilities changes only its module, runtime type schemas, and program-kind integration through existing SavorDb contracts. | `README.md`; `02-target-execution-architecture.md`; `10-verification-and-acceptance.md` | Target decision |
| TD-013 | This refactor changes no SavorDb schema/migration, stored representation, database-service/queue/claim/workflow interface, transaction boundary, or artifact-storage interface. | `README.md`; `06-workflows-frontiers-and-phase-composition.md`; `11-decisions-risks-and-deferred-work.md` | Target decision |

## Interfaces and ownership affected

This evidence map tracks the replacement of the worker activation/result protocol, `ProgramRegistry`
switches, `PhaseScriptVM` ownership, `PSContext` as a public runtime contract, the input-macro
mini-runtime, and runtime-facing program-kind handler behavior. Existing SavorDb descriptor, storage,
database-service, queue, claim, workflow, artifact, and transaction contracts are retained unchanged.

Evidence entries describe current ownership; target-decision entries describe future ownership. The two
must not be blended into statements that make a planned service appear implemented.

## Conflict and supersession ledger

| Conflict ID | Earlier statement | Governing resolution | Why |
|---|---|---|---|
| CF-001 | Router analysis top-level composition retains separate `PhaseScriptInterpreter` and `InputMacroEngine` under `ProgramService`. | One `ProgramRuntime` contains one `ProgramExecutor`; macro providers become reducer/subprogram continuations. | Two program-flow schedulers would reproduce ownership and composition conflicts. |
| CF-002 | Router Stage 6 says to shrink `PhaseScriptVM` while retaining `PSContext` and result behavior. | Replace the legacy interpreter with the typed module/IR/runtime; permit only a bounded translator for differential tests. | Current domain opcodes and flat context are themselves architectural constraints. |
| CF-003 | Router registry proposal centers stable wire kind, payload decoder, and program factory. | A SavorDb program-kind handler may retain current routing/affinity metadata and persisted-data codecs, but derives exact module revision/hash, entrypoint, schemas, and dependency closure before worker execution; no arbitrary controller/program factory. | Worker execution identity must be separated without redesigning SavorDb routing or storage. |
| CF-004 | User-script plan makes serialized current `PhaseScript` the canonical bytecode. | Define the new IR first; current builders compile/translate to it temporarily. | Serializer-first work would fossilize the 52-opcode domain-heavy VM. |
| CF-005 | User-script plan permits `PK_UserScript`/`MSG_SET_SCRIPT` alongside built-ins. | Built-in and authored frontends produce the same `ProgramModule` and use one activation path. | Parallel activation modes become permanent semantic divergence. |
| CF-006 | User-script plan uses generic `PSContext` for both request and result. | Entrypoints use typed runtime input/output/emission/artifact schemas; program-kind handlers translate to/from current persisted representations. | `PSContext` is global, flat, and directly includes current domain C++ types. |
| CF-007 | Current warm-worker affinity can avoid reactivation when kind/runtime/savestate strings match. | Current SavorDb affinity/claim contracts remain; after materialization, exact module identity, state policy, `StateEpoch`, and cleanup disposition are verified before activation/reuse. | Matching cache keys alone does not prove guest or host state cleanliness, but fixing that does not require queue/storage redesign. |
| CF-008 | Current navigation workflow names `nav.explore_geometry` as a durable workflow step. | That workflow step may invoke `establish_anchors` or `probe_geometry`; workflow step names and module entrypoints are separate namespaces. | No architectural conflict exists once the layers are explicit. |
| CF-009 | Historical or placeholder explorer names can look like implemented navigation phases. | Code and IC-120 through IC-122 govern current status: Survey and explorer execution are not implemented. | Names and UI contracts are not executable worker behavior. |
| CF-010 | A whole Survey or future phase could be placed behind one native controller/action. | Native actions are bounded transactions; phase branching is program IR and durable topology remains outside the worker under existing SavorDb contracts or a separate future workflow project. | A whole-phase action would hide a controller and bypass verifier, trace, cancellation, and composition guarantees. |

The external router analysis is not edited. Its validated session/router evidence remains cited; CF-001
through CF-003 record the target decisions that this package replaces.

## Unresolved research map

| ID | Deferred question | Governing boundary | Source | Classification |
|---|---|---|---|---|
| UR-001 | Exact authored source syntax and editor experience | Every frontend must compile to the same verified module | `11-decisions-risks-and-deferred-work.md` | Unresolved research |
| UR-002 | Concrete C++ signatures, module binary format, canonical hash algorithm, and worker wire bytes | Logical runtime contracts in documents 03-06 cannot change; SavorDb storage is fixed and out of scope | `03`-`06`; document 11 | Unresolved research |
| UR-003 | Full trigger behavior beyond the first door path, including `eventhook` | First slice uses only the approved scoped toggle and recorded door evidence | Navigation workflow `07-open-research-questions.md:65-83` | Unresolved research |
| UR-004 | Final teleport implementation and positional settle tolerance | Anchor publication must fail closed and store only spatial evidence | Same file `:65-77` | Unresolved research |
| UR-005 | Collision-oddity objective/scoring and refinement policy | Bounded program emits observations; existing workflow operations may consume them, while any new durable frontier/refinement model is a separate future project | Same file `:88-92`; document 08 | Unresolved research |
| UR-006 | Overworld-specific movement, branching, goal, pruning, and durable frontier rules | Bounded worker expansion is fixed; durable frontier persistence is a separate future workflow project | Document 08; document 11 | Unresolved research |
| UR-007 | Cutscene fast-forward game strategy | It must use reusable actions/programs and cannot add another executor | Document 08; document 11 | Unresolved research |
| UR-008 | External consumers of legacy `PK_BattleTurnRunner` not visible in this repository | Migrate to deprecated `soa.battle.legacy_path`, audit, then remove if unused | `07-current-phase-migration-matrix.md` | Unresolved research |

An implementation slice that reaches one of these boundaries must either stay within the documented
fallback or update the authoritative plan with new evidence. It must not make the deferred choice
silently.

## Failure and cleanup behavior

### Current cleanup evidence

Current cleanup evidence is deliberately separated from target requirements:

- IC-042 proves VM job guards clear watchpoints and stop probe capture.
- IC-047 proves macro cleanup-once neutralizes input and restores its local state.
- IC-043 and IC-048 prove that physical ownership is still distributed and often destructive.
- No current general scoped receipt model covers input, stop subscriptions, movies, captures, state
  handles, data writes, and executable patches together.

Therefore the target rule is not an assertion that current cleanup is absent. It is the decision that all
effectful resources must enter one structured scope stack, all exits must unwind it, cleanup status must
be returned separately, and an unverifiably dirty session must be tainted.

## Dependencies and migration implications

Before implementing any work package, its owner must:

1. find the relevant evidence IDs in this map;
2. re-open the cited current symbols and check for drift;
3. record changed evidence or contradictions;
4. update current-state prose before changing a target contract;
5. preserve the conflict rulings unless new evidence triggers an explicit planning revision; and
6. attach tests to the corresponding current parity and target-decision IDs.

The legacy translator must record the exact source builder and target module hash so differential
evidence can be traced. Artifact comparisons must record codec version and semantic comparison rules;
they must not assume byte equality when a runtime-only artifact or envelope intentionally adds
provenance. Existing SavorDb artifact codecs and stored representations remain unchanged.

## Acceptance criteria

This evidence map is complete for the guidance phase when:

- every major current-system assertion in documents 01 and 07 maps to an IC entry;
- every observed bootstrap assertion maps to OR-001 or OR-002;
- every locked architectural family maps to a TD entry;
- every external-analysis or superseded-plan conflict has an explicit ruling;
- Survey is consistently marked unimplemented in current code;
- source paths and symbols resolve at the recorded snapshot;
- the bootstrap hashes can be reproduced from the exact files; and
- deferred research has a fixed architectural boundary and an authoritative place to resolve it.

During implementation, acceptance additionally requires updating entries whose current-state claim has
changed rather than leaving this snapshot to masquerade as live truth.

## Deferred work

This map intentionally does not contain:

- final implementation file locations;
- final C++ signatures;
- SavorDb SQL migrations, because they are prohibited in this refactor rather than deferred;
- final byte-level worker wire schema;
- authored-language syntax;
- final game-specific algorithms for Survey, collision search, cutscenes, or overworld;
- performance measurements not yet captured; or
- evidence from future refactor commits.

The permitted runtime and domain items above become new evidence entries when they exist. Any SavorDb
SQL migration belongs to a separate project and never becomes Execution Runtime implementation evidence.

## Source references

Current repository:

- `SavorWorker/SavorWorker.cpp`
- `SavorWorkflow/Worker/ProcessWorker.cpp`
- `SavorWorkflow/Execution/JobMaterializationService.cpp`
- `SavorWorkflow/Execution/DBWorkflowWorkerCoordinator.cpp`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp`
- `SavorCore/Phases/Programs`
- `SavorCore/Runner/Script`
- `SavorCore/Runner/InputMacro`
- `SavorCore/Runner/IPC/Wire.h`
- `SavorDb/Execution/ProgramDB`
- `SavorDb/Execution/Workflow`
- `SavorDb/Execution/Workflow/WorkflowComposition.cpp`
- `SavorQt/SavorDbRuntime.cpp`
- `SavorE2E`
- `SavorTests`

Authoritative domain planning:

- `planning/NavigationPhase/NavigationContextWorkflow/README.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
- `planning/NavigationPhase/NavigationContextWorkflow/07-open-research-questions.md`

Historical/research sources:

- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt`
