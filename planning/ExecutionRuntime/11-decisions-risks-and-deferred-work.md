# Decisions, Risks, and Deferred Work

## Status and authority

**Status:** Authoritative decision register for the target Execution Runtime.

Decisions marked **Locked** are not open implementation questions. Reopening one requires new code or
runtime evidence, an identified contradiction, and coordinated updates to every affected document.
Items marked **Deferred** must not be silently decided inside an implementation pull request.

## Purpose and non-goals

This document prevents architectural ambiguity from moving downstream to implementers. It records the
chosen boundary, rejected alternatives, known risks, mitigations, and intentionally deferred product or
encoding decisions.

It is not a backlog and does not assign implementation owners or dates.

## Current code evidence

The locked decisions respond to specific current pressures:

- `ProgramRegistry.cpp:44-109` uses central `ProgramKind` switches for construction and decoding.
- `PhaseScriptOpcodeTable.inc` mixes language operations with battle, TAS, state, capture, memory, and
  navigation operations.
- `PhaseScriptVM.h:35-150` owns Dolphin access, stop-point state, one snapshot, input-macro state, visual
  debugging, and domain-specific sinks.
- `PSContext.h` uses one closed variant/key map across invocation inputs, locals, and results.
- `IInputMacroPlanDriver.h:49-59` demonstrates a useful `Start`/`Advance` continuation shape, but it is
  hosted inside the current VM as a separate mini-runtime.
- `ProgramKindDescriptor.h:142-185` combines queue persistence, runtime initialization, result mapping,
  and workflow transitions.
- `WorkflowTransitionDecision::spawn_steps` already proves that durable orchestration can create later
  waves outside the worker.

Navmesh Survey remains unimplemented. Its requirements are a target-design test, not evidence of a
current runtime capability.

## Locked target decisions

| ID | Decision | Status | Consequence |
|---|---|---|---|
| D01 | One universal typed program executor/VM serves every bounded worker program. | Locked | No `NavmeshSurveyRunner`, cutscene controller, overworld controller, or controller-per-phase factory. |
| D02 | `ProgramRuntime` is a subsystem; `ProgramExecutor` is its sole interpreter; `ProgramInstance` is data/state. | Locked | These terms cannot be used interchangeably with native phase controllers. |
| D03 | `WorkerRuntime` owns external commands and session lifecycle. | Locked | Programs and actions cannot read the worker pipe or change worker assignment. |
| D04 | `ExecutionEngine` alone advances Dolphin. | Locked | All run, step, wait, input-synchronized advance, and stop behavior goes through one serialized owner. |
| D05 | The canonical IR is a small typed CFG with a single action-await boundary. | Locked | Domain behavior never adds an interpreter opcode or switch case. |
| D06 | Native extension uses bounded actions or pure reducers. | Locked | A native extension may integrate with services or compute a transition but may not own an entire phase. |
| D07 | All effectful resources are scoped and unwound on every terminal path. | Locked | Cleanup failure taints the session and blocks reuse. |
| D08 | Savestate restore creates a new `StateEpoch`. | Locked | Guest-derived opaque handles are invalid after restore and must be reacquired. |
| D09 | Program identity is immutable module revision/hash plus entrypoint and exact dependency closure. | Locked | Today's compiled definition selected only by `ProgramKind` is insufficient. |
| D10 | Built-in C++ builders and future authored sources compile to the same `ProgramModule`. | Locked | There is no special `PK_UserScript` runtime or dual activation ABI. |
| D11 | `ProgramKind` may remain semantic metadata but not executor dispatch, payload decoding, controller selection, or primary affinity. | Locked | Existing switches are migration targets. |
| D12 | Programs are bounded to one session; workflows own durable waves, phase selection, frontier policy, deduplication, and recovery. | Locked | An invocation may emit child state artifacts but cannot enqueue them. |
| D13 | Results separate infrastructure, domain, and cleanup/session status and support zero-to-many emissions/artifacts. | Locked | A global `ok` or outcome key cannot represent the target contract. |
| D14 | Runtime compatibility is dependency-scoped, not one global registry hash. | Locked | Unrelated action additions do not invalidate modules. |
| D15 | Guest data writes and executable patches use one generic checked `GuestMutationService`. | Locked | Survey trigger suppression and battle mutations share audit, restore, and epoch behavior. |
| D16 | Game capabilities are modular packs: `soa.battle`, `soa.field`, `soa.navigation`, `soa.cutscene`, and `soa.overworld`. | Locked | The refactor does not create another monolithic game-runtime facade. |
| D17 | The existing session/router analysis is retained as evidence but its “shrink VM” and arbitrary program-factory target is superseded. | Locked | Session services are absorbed beneath the universal executor. |
| D18 | The user-script payload plan's authoring/revision separation is retained, while serialized current `PhaseScript`, flat `PSContext`, `PK_UserScript`, and dual paths are superseded. | Locked | Program IR is defined before persistence and authoring frontends. |
| D19 | Breaking cutover deletes the legacy interpreter after a bounded differential window. | Locked | Production does not carry two controllers indefinitely. |
| D20 | Navmesh Survey is a general architecture acceptance client with `establish_anchors` and `probe_geometry` entrypoints. | Locked | Two worker waves remain workflow topology; Survey does not own a worker scheduler. |
| D21 | Survey evidence is spatial. | Locked | Safety deadlines are infrastructure metadata, not inferred probe timing or navigation evidence. |

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
Input leases and segment execution become actions beneath `ProgramExecutor`.

### Put arbitrary-depth search inside a worker program

Rejected because worker loss would lose search topology and because one worker would become a scheduler.
Durable frontier state, deduplication, retries, and arbitrary wave generation belong to workflows.

### Freeze current `PSContext` and `ProgramKind` as public ABI

Rejected because current inputs, locals, results, and domain-specific values are conflated. Exact replay,
many artifacts, typed schemas, and independent capability evolution require explicit contracts.

### Permanent compatibility runtime

Rejected. A temporary translator is acceptable for differential evidence, but maintaining old and new
production paths would make ownership and cleanup guarantees unenforceable.

## Risks and required mitigations

| Risk | Failure mode | Required mitigation |
|---|---|---|
| Actions become hidden controllers | Long-running action owns loops, branching, or private state machines | Enforce descriptor review, bounded effects, cancellation points, trace visibility, and maximum budgets |
| Action granularity becomes too small | Programs become noisy and encode emulator mechanics | Provide reusable composite IR subprograms and transaction-sized actions |
| Action granularity becomes too large | Programs cannot observe, cancel, replay, or compose behavior | Split at observable capability transactions; keep orchestration in IR or workflow |
| Cleanup is best-effort only | Inputs, patches, subscriptions, movies, or captures leak into the next job | Structured scopes, restoration receipts, fault injection, and mandatory session taint |
| State restore leaves stale handles | Guest pointers or router assumptions are reused after restore | Epoch-tag every opaque handle and verify at action boundaries |
| Version checks are globally fragile | Adding an unrelated registry entry rejects all cached modules | Verify exact imported dependency closure and signature hashes |
| A legacy translator becomes permanent | Two semantic runtimes diverge | Publish removal gates before introducing the translator; block new features on the legacy path |
| Workflow frontier grows without bound | Overworld or collision search exhausts DB/storage | Persist budgets, dedupe fingerprints, terminal reasons, pruning policy, and artifact retention |
| Replay is overstated | Matching host timing is mistaken for deterministic game behavior | Compare exact inputs plus action/branch/observation traces; declare nondeterministic action fields |
| `ContinueSession` is used implicitly | A phase consumes unknown patches, input, or guest state | Require expected session lineage, epoch, and clean resource ledger |
| Domain packs become another monolith | Unrelated phase changes force one broad capability version | Namespace and version packs and action imports independently |
| Planning drifts from code | Current-state statements become obsolete | Maintain document 12 with commit/date evidence and update the discrepancy ledger before each slice |

## Deferred work

### Contract encodings

- Exact C++ class and ownership declarations.
- Canonical module binary format and hash algorithm.
- Worker message framing, streaming, compression, and negotiation encoding.
- SQL DDL, indexes, migration scripts, and artifact-store tables.

These are deferred encodings, not permission to alter the logical fields or ownership model.

### Authoring experience

- JSON, DSL, graphical, or other authored source syntax.
- Source editor, validation UI, debugger UI, publication, ownership, and permission model.
- Whether normalized source operations are queryable in the DB.

Every frontend must compile to the same verified `ProgramModule`.

### Game-specific research and algorithms

- Generalized `eventhook` trigger behavior and trigger allowlisting.
- Overworld movement, encounter, branching, and game-state rules.
- Collision-oddity objective generation and refinement heuristics.
- Cutscene fast-forward optimization strategy and choice schedule.
- Navigation replay verification tolerances.
- Survey spatial probe spacing, settle tolerances, and reduction algorithms beyond the locked first-slice
  semantics.

These items may add capability-pack actions, schemas, and programs. They may not create a new executor.

### Product and operations

- User-facing workflow composer changes.
- Module revision promotion and rollback UI.
- Artifact retention policy and storage backend.
- Operational dashboards and performance targets.

## Interfaces and ownership affected

The decisions require replacement or decomposition of:

- worker protocol activation and result envelopes;
- `ProgramRegistry` construction/decoding switches;
- `PhaseScriptVM` and its central opcode dispatch;
- `PSContext` as a public invocation/result contract;
- VM-owned macro, stop-point, savestate, input, and Dolphin services; and
- `ProgramKindDescriptor` as a combined worker/workflow adapter.

They retain the durable workflow concepts, current phase behavior, and useful `Start`/`Advance` adaptive
pattern while changing their boundaries.

## Failure and cleanup behavior

No deferred implementation detail may weaken these rules:

- cancellation and timeout unwind the same resource stack as normal return;
- mandatory restoration failure taints the session;
- a tainted session cannot run another invocation;
- state restore invalidates epoch-bound handles;
- a domain failure can be a clean infrastructure completion; and
- workflow retries use immutable attempt and artifact lineage rather than ambiguous in-place mutation.

## Dependencies and migration implications

Document 09 orders the cutover. Any implementation slice that discovers a need for a second executor,
phase controller, unscoped mutation, direct Dolphin access, or durable in-worker frontier must stop and
update this architecture through evidence-based review before proceeding.

## Acceptance criteria

- Every architectural question required to start the refactor is answered by a locked decision.
- Every remaining open item is listed as deferred with a boundary that prevents architectural drift.
- Every rejected alternative includes the reason it conflicts with ownership or extensibility goals.
- Risks have mandatory mitigations that can be tested.
- No deferred item is required to implement the universal executor or migrate current phases.

## Source references

- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc`
- `SavorCore/Runner/Script/PhaseScriptVM.h:35-150`
- `SavorCore/Runner/Script/PSContext.h`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:49-59`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:121-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
- `planning/NavigationPhase/NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router`
