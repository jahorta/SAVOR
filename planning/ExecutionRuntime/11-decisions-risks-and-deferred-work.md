# Decisions, Risks, and Deferred Work

## How to use this record

These decisions guide implementation until current code or runtime evidence exposes a contradiction or a
genuinely general missing primitive. Revise the affected guidance when that happens. Deferred items remain
outside the implementation slice unless the slice reaches their documented boundary.

## Purpose and non-goals

This document prevents architectural ambiguity from moving downstream to implementers. It records the
chosen boundary, rejected alternatives, known risks, mitigations, and intentionally deferred product or
encoding decisions.

It is not a backlog and does not assign implementation owners or dates.

## Current code evidence

The current decisions respond to specific current pressures:

- `ProgramRegistry.cpp:44-109` uses central `ProgramKind` switches for construction and decoding.
- `PhaseScriptOpcodeTable.inc` mixes language operations with battle, TAS, state, capture, memory, and
  navigation operations.
- `PhaseScriptVM.h:35-150` owns Dolphin access, stop-point state, one snapshot, input-macro state, visual
  debugging, and domain-specific sinks.
- `PSContext.h` uses one closed variant/key map across invocation inputs, locals, and results.
- `IInputMacroPlanDriver.h:49-59` demonstrates a useful `Start`/`Advance` continuation shape, but it is
  hosted inside the current VM as a separate mini-runtime.
- `InputMacroPlan.h` and `InputMacroRuntime.cpp` encode guest-facing ordering around semantic gates,
  input epochs, held-through-hit execution, memory baselines/change waits, poll acknowledgements, and
  cleanup-once behavior that must survive removal of that mini-runtime.
- `ProbeProfile.*` and `ProbeRuntime.*` define the current `savor.capture.profile/1` parser, address and
  predicate programs, subscriptions including `control`, sampling policies, windows, flight recorders,
  progress, and artifact behavior.
- `ProgramKindDescriptor.h:142-185` combines queue persistence, runtime initialization, result mapping,
  and workflow transitions.
- `WorkflowTransitionDecision::spawn_steps` already proves that durable orchestration can create later
  waves outside the worker.

Navmesh Survey remains unimplemented. Its requirements are a target-design test, not evidence of a
current runtime capability.

## Current target decisions

| ID | Decision | Consequence |
|---|---|---|
| D01 | One universal typed program executor/VM serves every bounded worker program. | No `NavmeshSurveyRunner`, cutscene controller, overworld controller, or controller-per-phase factory. |
| D02 | `ProgramRuntime` is a subsystem; `ProgramExecutor` is its sole interpreter; `ProgramInstance` is data/state. | These terms cannot be used interchangeably with native phase controllers. |
| D03 | `WorkerRuntime` owns external commands and session lifecycle. | Programs and actions cannot read the worker pipe or change worker assignment. |
| D04 | `ExecutionEngine` alone advances Dolphin. | All run, step, wait, input-synchronized advance, and stop behavior goes through one serialized owner. |
| D05 | The canonical IR is a small typed CFG with a single action-await boundary. | Domain behavior never adds an interpreter opcode or switch case. |
| D06 | Native extension uses bounded actions or pure reducers. | A native extension may integrate with services or compute a transition but may not own an entire phase. |
| D07 | All effectful resources are scoped and unwound on every terminal path. | Cleanup failure taints the session and blocks reuse. |
| D08 | Savestate restore creates a new `StateEpoch`. | Guest-derived opaque handles are invalid after restore and must be reacquired. |
| D09 | Program identity is immutable module revision/hash plus entrypoint and exact dependency closure. | Today's compiled definition selected only by `ProgramKind` is insufficient. |
| D10 | Built-in C++ builders and future authored sources compile to the same `ProgramModule`. | There is no special `PK_UserScript` runtime or dual activation ABI. |
| D11 | `ProgramKind` may remain SavorDb job/handler/transition/queue/affinity and semantic metadata, but not worker executor dispatch, worker-side payload decoding, or controller selection. | Existing SavorDb routing/storage remains; worker execution switches are migration targets. |
| D12 | Programs are bounded to one session; existing SavorDb workflows retain durable waves, phase selection, deduplication, and recovery through unchanged contracts. | An invocation may emit candidate artifacts but cannot enqueue jobs or mutate durable workflow state. |
| D13 | Runtime results separate infrastructure, domain, and cleanup/session status and support zero-to-many emissions/artifacts. | Program-kind adapters project them only through existing SavorDb persistence operations. |
| D14 | Runtime compatibility is dependency-scoped, not one global registry hash. | Unrelated action additions do not invalidate modules. |
| D15 | Guest data writes and executable patches use one generic checked `GuestMutationService`. | Survey trigger suppression and battle mutations share audit, restore, and epoch behavior. |
| D16 | Game capabilities are modular packs: `soa.battle`, `soa.field`, `soa.navigation`, `soa.cutscene`, and `soa.overworld`. | The refactor does not create another monolithic game-runtime facade. |
| D17 | The existing session/router analysis is retained as evidence but its "shrink VM" and arbitrary program-factory target is superseded. | Session services are absorbed beneath the universal executor. |
| D18 | The user-script payload plan's authoring/revision separation is retained, while serialized current `PhaseScript`, flat `PSContext`, `PK_UserScript`, and dual worker paths are superseded. | Program IR is independent of any separately approved future persistence or authoring frontend; existing stored job/result formats remain. |
| D19 | Breaking cutover deletes the legacy interpreter after a bounded differential window. | Production does not carry two controllers indefinitely. |
| D20 | Navmesh Survey is a future architecture stress test with `establish_anchors` and `probe_geometry` entrypoints. | It does not gate this refactor. If implemented, its bounded programs use current workflow operations where sufficient; any new durable two-wave topology is separate workflow work. |
| D21 | Survey evidence is spatial. | Safety deadlines are infrastructure metadata, not inferred probe timing or navigation evidence. |
| D22 | SavorDb storage and orchestration contracts are fixed inputs to this refactor. Only runtime-facing program-kind handler implementations or adjacent adapters may change. | No SavorDb SQL/schema migration, stored-representation change, database-service/queue/claim/workflow-interface change, transaction-boundary change, or artifact-storage-interface change. |
| D23 | Predicates are a reusable composition library that consumes semantic-observation results and lowers pure conditions plus explicit use policies into canonical IR, branches, and declared emissions. | There is no predicate executor, runtime service, domain opcode family, direct emulator ownership, hidden effect channel, or new persistence model. |
| D24 | Semantic observations are a reusable composition library. Capability packs define logical points, typed address/query observations, acquisition modes, baselines, and use policy; the composer lowers them before verification into ordinary IR, actions, router subscriptions, values, branches, and emissions. | There is no observation runtime, query VM, observation opcode family, direct emulator ownership, filesystem/database access, or hidden post-step timing. |
| D25 | Static and adaptive guest interactions are a reusable composition library. Versioned typed definitions, pure initialization/advancement reducers, and a finite verifier-known segment set lower before verification into subprogram CFG, semantic observations, input/execution actions, branches, and emissions. | There is no interaction runtime, peer macro scheduler, controller-like segment action, dynamically constructed effect, or second cancellation/cleanup model. |
| D26 | Existing `savor.capture.profile/1` representation and semantics remain intact behind passive `CaptureService` during this refactor. `StopPointRouter` and `ExecutionEngine` retain wake/control authority, while capture observes the same routed event and identity. | Capture profiles are neither lowered into program IR nor replaced. Existing control flags/metrics and control-triggered windows/recorders remain observable without granting a profile execution control. |

## Rejected alternatives

### Native controller per phase

Rejected because it creates competing execution, cancellation, debugger, cleanup, and resource-ownership
models. Existing phases already exhibit multiple control-flow shapes inside one VM. Survey adds actions
and composition needs, not a fundamentally different controller category.

### Keep the present VM and add more opcodes

Rejected because the opcode table and dispatch already mix language semantics with domain integration.
Every domain addition would modify the central executor and permanently couple authored-program
compatibility to internal game operations.

### Make a whole phase one native action

Rejected because `RunNavmeshSurvey`, `ExploreOverworld`, or `FastForwardAllCutscenes` would recreate the
controller-per-phase architecture behind an action descriptor. An action must represent one bounded,
reusable capability transaction.

### Keep `InputMacroEngine` as a peer executor

Rejected. The useful adaptive `Start`/`Advance` shape becomes the general continuation/reducer contract.
Input leases and the effects within each verifier-known segment become ordinary actions beneath
`ProgramExecutor`; the reusable interaction composer preserves their temporal ordering without surviving
as a scheduler.

### Make an interaction segment or whole macro one native controller action

Rejected. A controller-like action would hide input-before-step ordering, routed stop identity,
held-through-hit behavior, request/release acknowledgement, baselines, branches, cancellation, and
cleanup from verification and tracing. Interaction composition lowers these steps into visible ordinary
IR and bounded actions. A pure reducer may select only a declared segment.

### Separate observation runtime or query VM

Rejected. Semantic waits, bounded hit-time samples, paused reads, registered coherent queries,
baselines, and evidence policies compose into the existing router, engine, actions, IR values, and
emissions. A separate runtime, query instruction language, or implicit post-step mode would recreate
hidden control flow and timing semantics.

### Separate predicate runtime

Rejected. A predicate interpreter, controller, or independently scheduled service would recreate a
mini-runtime with hidden control flow and resource ownership. Reusable predicate definitions are a
composition frontend only; their generated behavior executes through the universal program executor and
existing session services.

### Replace capture profiles during runtime extraction

Rejected for the initial refactor. Re-authoring `savor.capture.profile/1` as a generalized capture-plan
language would combine an architecture cutover with a behavior and artifact-format migration. Existing
profile parsing and semantics move intact behind passive `CaptureService`. Capture observes routed
control events but does not acquire wake/control authority.

### Put arbitrary-depth search inside a worker program

Rejected because worker loss would lose search topology and because one worker would become a scheduler.
Existing durable workflow behavior remains in SavorDb. Any generalized frontier state, deduplication,
retry policy, or arbitrary-wave generation requires a separate workflow project.

### Freeze current `PSContext` and `ProgramKind` as public ABI

Rejected because current inputs, locals, results, and domain-specific values are conflated. Exact replay,
many artifacts, typed schemas, and independent capability evolution require explicit contracts.

### Permanent compatibility runtime

Rejected. A temporary translator is acceptable for differential evidence, but maintaining old and new
production paths would make ownership and cleanup guarantees unenforceable.

## Risks and mitigations

| Risk | Failure mode | Required mitigation |
|---|---|---|
| Actions become hidden controllers | Long-running action owns loops, branching, or private state machines | Enforce descriptor review, bounded effects, cancellation points, trace visibility, and maximum budgets |
| Action granularity becomes too small | Programs become noisy and encode emulator mechanics | Provide reusable composite IR subprograms and transaction-sized actions |
| Action granularity becomes too large | Programs cannot observe, cancel, replay, or compose behavior | Split at observable capability transactions; keep orchestration in IR or workflow |
| Cleanup is best-effort only | Inputs, patches, subscriptions, movies, or captures leak into the next job | Structured scopes, restoration receipts, fault injection, and mandatory session taint |
| State restore leaves stale handles | Guest pointers or router assumptions are reused after restore | Epoch-tag every opaque handle and verify at action boundaries |
| Version checks are globally fragile | Adding an unrelated registry entry rejects all cached modules | Verify exact imported dependency closure and signature hashes |
| A legacy translator becomes permanent | Two semantic runtimes diverge | Publish removal gates before introducing the translator; block new features on the legacy path |
| Replay is overstated | Matching host timing is mistaken for deterministic game behavior | Compare exact inputs plus action/branch/observation traces; declare nondeterministic action fields |
| `ContinueSession` is used implicitly | A phase consumes unknown patches, input, or guest state | Require expected session lineage, epoch, and clean resource ledger |
| Domain packs become another monolith | Unrelated phase changes force one broad capability version | Namespace and version packs and action imports independently |
| Semantic observation hides timing or coherence | A read silently occurs after step, hit-time work blocks the CPU thread, or separate scalar reads are treated as one snapshot | Make acquisition mode explicit, require explicit post-step flow, restrict hit-time samples, and use registered coherent queries |
| Observation absence is conflated with a domain value | Missing evidence silently becomes false or zero | Keep optional `Unavailable` distinct, fail required evidence structurally, and trace availability |
| Adaptive interaction timing drifts | Input is published after step-off, reached instructions run under the wrong input, or neutral publication is mistaken for guest-observed release | Characterize and test exact input/step/receipt order, point/PC/sequence/epoch matching, reached-instruction policy, and fresh-neutral release witnesses |
| Interaction composition becomes a hidden macro engine | A segment action or reducer owns service calls, loops, dynamic effects, or private cleanup | Restrict reducers to pure selection of verifier-known segments and lower every effect/cancellation/unwind edge into ordinary visible IR |
| Predicate composition hides effects or conflates false with unavailable | A reusable check silently owns stop points, fails open on unreadable state, or changes failure classification | Require typed observations, explicit use-site policy, exact imports, scoped subscriptions, declared emissions, and trace-visible evaluation outcomes |
| Capture extraction changes profile semantics or steals control | Sampling/window/artifact behavior drifts, or a profile creates a foreground wake path | Run profile compatibility characterization, keep one routed-hit identity, and enforce passive observation with router/engine as control owners |
| Planning drifts from code | Current-state statements become obsolete | Re-read the affected current symbols and revise guidance when implementation evidence changes an architectural conclusion |

## Deferred work

### Contract encodings

- Exact C++ class and ownership declarations.
- Canonical module binary format and hash algorithm.
- Worker message framing, streaming, compression, and negotiation encoding.

These are deferred runtime encodings, not permission to alter the logical fields or ownership model.
SavorDb SQL/schema, migrations, stored representations, interfaces, queues, claims, workflows, and
artifact-store interfaces are out of scope and remain unchanged.

### Authoring experience

- JSON, DSL, graphical, or other authored source syntax.
- Source editor, validation UI, debugger UI, publication, ownership, and permission model.
- A generalized capture-plan authoring language or replacement for `savor.capture.profile/1`.

Every frontend must compile to the same verified `ProgramModule`.
The capture item is different: until separately designed, existing profiles remain opaque versioned
inputs consumed by `CaptureService` rather than a frontend that compiles into module IR.

### Game-specific research and algorithms

- Generalized `eventhook` trigger behavior and trigger allowlisting.
- Overworld movement, encounter, branching, and game-state rules.
- Collision-oddity objective generation and refinement heuristics.
- Cutscene fast-forward optimization strategy and choice schedule.
- Navigation replay verification tolerances.
- Survey spatial probe spacing, settle tolerances, and reduction algorithms beyond the defined first-slice
  semantics.

These items may add capability-pack actions, schemas, and programs. They may not create a new executor.

### Separate future projects

- User-facing workflow composer changes.
- Module revision promotion and rollback UI.
- Artifact retention policy and storage backend.
- Persisted authored source/module catalogs or queryable normalized source operations.
- Generalized workflow/frontier persistence, policies, and scheduling.
- Operational dashboards and performance targets.

These are not deferred implementation choices in the Execution Runtime refactor.

## Interfaces and ownership affected

The decisions require replacement or decomposition of:

- worker protocol activation and result envelopes;
- `ProgramRegistry` construction/decoding switches;
- `PhaseScriptVM` and its central opcode dispatch;
- `PSContext` as a public invocation/result contract;
- VM-owned macro, stop-point, savestate, input, and Dolphin services; and
- runtime-facing program-kind handler implementation and adjacent worker integration.

They preserve the existing SavorDb descriptor, persistence, database-service, queue, claim, workflow,
artifact, and transaction interfaces. They also retain current phase behavior and the useful
`Start`/`Advance` adaptive pattern. Existing stop/address/query/baseline and input-macro inputs are
translated into semantic-observation and interaction composition in memory. Existing
`savor.capture.profile/1` artifacts remain unchanged behind `CaptureService`.

## Failure and cleanup behavior

No deferred implementation detail may weaken these rules:

- cancellation and timeout unwind the same resource stack as normal return;
- mandatory restoration failure taints the session;
- a tainted session cannot run another invocation;
- state restore invalidates epoch-bound handles;
- semantic-point receipts, observations, and baselines cannot cross `StateEpoch`;
- optional observation unavailability remains distinct from false or zero, while required missing
  evidence fails structurally;
- interaction unwind neutralizes input, proves release when required, and releases nested subscriptions
  and the interaction-wide input lease exactly once;
- timeout, unexpected point, unacknowledged input/release, unsatisfied check, infrastructure failure,
  cancellation, and cleanup failure remain distinguishable;
- capture finalization preserves existing profile behavior, but a capture profile never owns wake or
  advancement;
- a domain failure can be a clean infrastructure completion; and
- an unsatisfied predicate follows its explicit check policy, while inability to obtain required evidence
  remains a distinct action or infrastructure failure;
- existing SavorDb retry, idempotency, and lineage behavior remains unchanged; runtime trace identity is
  not a new persistence requirement.

## Dependencies and migration implications

Document 09 gives the dependency direction. An implementation slice that appears to require a second
executor, phase controller, unscoped mutation, direct Dolphin access, or durable in-worker frontier must
stay within the current constraints or revise the relevant guidance from concrete implementation evidence
before broadening the design.

## Consistency checks

- Single-owner execution, scoped cleanup, typed runtime, and no-dual-runtime constraints remain intact.
- Deferred items have boundaries that keep them out of the universal executor and current-phase migration.
- Risk mitigations are implemented and tested with the seam that makes them relevant.
- No SavorDb migration, stored-representation change, database-service/queue/claim/workflow-interface
  change, transaction-boundary change, or artifact-storage-interface change is part of the refactor.
- Semantic-observation, interaction, and predicate composition lower completely before verification and
  leave no peer runtime, scheduler, query VM, opcode family, or hidden controller.
- Existing `savor.capture.profile/1` semantics remain behind passive `CaptureService`; a generalized
  replacement language remains deferred.

## Source references

- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
- `SavorCore/Runner/Script/PhaseScriptVM.h:35-150`
- `SavorCore/Runner/Script/PSContext.h`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:49-59`
- `SavorCore/Runner/InputMacro/InputMacroPlan.h`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp`
- `SavorProbe/ProbeProfile.h`
- `SavorProbe/ProbeProfile.cpp`
- `SavorProbe/ProbeRuntime.h`
- `SavorProbe/ProbeRuntime.cpp`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:121-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router`
