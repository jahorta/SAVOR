# 06 - Workflows, Frontiers, and Phase Composition

## Status and authority

**Status:** Target architecture guidance; not implemented.

This document is authoritative for the boundary between a bounded worker program and durable SAVOR
orchestration. It defines how workflows compose programs, bind typed results, create parallel waves, and
own arbitrary-depth search frontiers. It does not claim that the logical contracts below already have
database tables or wire encodings.

Repository code is authoritative for current behavior. In particular, the existing workflow structures
and services described below are implementation evidence, not proof that the target frontier model
already exists.

## Purpose and non-goals

The purpose of this boundary is to let every phase use one `ProgramRuntime` while allowing workflows to
express topology that cannot safely live in a worker process:

- sequential phase composition;
- parallel fan-out and deterministic fan-in;
- finite two-wave jobs such as Navmesh Survey;
- user or policy selection gates;
- retry and crash recovery;
- breadth-first, depth-first, or best-first expansion through an arbitrary number of waves; and
- exact artifact and savestate lineage across phase changes.

A program invocation is a bounded interaction with one emulation session. It may call subprograms, await
registered actions, emit records and artifacts, and return one result. It may not:

- create, claim, prioritize, retry, or cancel workflow jobs;
- write the durable workflow graph or frontier;
- select an implicit "latest" upstream result;
- recursively run an unbounded search;
- keep the only copy of a search stack, visited set, or wave barrier in worker memory; or
- decide that its own output should become an accepted durable successor.

This document does not define SQL DDL, a final transport encoding, workflow-authoring UI, or the internal
search rules of a game-specific capability pack.

## Current code evidence

The current repository already demonstrates several useful orchestration behaviors:

- `SavorDb/Execution/Workflow/WorkflowOrchestration.h` defines persisted workflow instances, unit
  activations, steps, edges, input bindings, arguments, attempts, output records, and terminal snapshots.
  It also exposes retry, cancel, resume, terminal marking, output recording, unit scheduling, and dynamic
  step insertion commands.
- `WorkflowAppendDynamicStepsCommand` accepts a parent step plus zero-to-many children.
  `SqliteWorkflowOrchestrationCommandService::AppendDynamicSteps` inserts those children transactionally,
  creates parent edges, makes them ready, and treats an existing `(workflow instance, step key)` as
  idempotent only when its shape matches.
- `WorkflowTerminalAdvancementService` evaluates a terminal step, marks it terminal, appends dynamic
  successors, routes authored-graph outputs, or completes/fails the workflow.
- `WorkflowGraphRoutingService::RouteTerminalStep` records job outputs as step outputs, checks authored
  `data_kind` compatibility, records upstream input bindings, and makes a target step ready only after
  all required bindings exist.
- `ProgramKindDescriptor.h` currently attaches job persistence, runtime initialization, result mapping,
  and `IWorkflowTransitionHandler` to a numeric program kind. Its
  `WorkflowTransitionDecision::spawn_steps` is the current extension point for dynamic waves.
- `BattleSingleTurnTransitionHandler` in
  `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp` selects successful survivor
  savestates, records the advancement decision, creates next-turn wave records, and emits one dynamic
  workflow step per selected successor.
- Existing tests cover idempotent dynamic insertion, terminal advancement, workflow graph routing,
  restart during a workflow, and BattleSingleTurn next-turn fan-out.

These are strong foundations. The pressure points are:

- transition policy is coupled to `ProgramKindDescriptor`, even though search topology belongs to the
  workflow;
- step and binding contracts identify kinds primarily with strings and database references, rather than
  the complete schema and immutable program identity required by the target runtime;
- a dynamic step is a generic child, not a first-class frontier node with lineage, deduplication,
  expansion state, strategy, or goal status; and
- current program kinds and materializers combine workflow adaptation with worker execution selection.

The target retains transactional lifecycle and dynamic scheduling while removing those couplings.

## Locked target decisions

### One durable owner for topology

The workflow subsystem is the sole durable owner of:

- workflow and frontier state;
- ready/claimed/terminal transitions;
- fan-out and fan-in barriers;
- retry policy and attempt history;
- selection and pruning policy;
- parent/edge lineage;
- deduplication and accepted-result choice;
- cross-phase input binding;
- search limits and terminal/goal determination; and
- scheduling priority.

`ProgramRuntime` never calls workflow persistence. It returns a `ProgramResult`; an orchestration adapter
validates that result against the invocation contract and applies it to workflow state in one
idempotent transaction.

### Bounded invocation contract

Every workflow execution step resolves to one exact `ProgramInvocation`:

| Field | Rule |
|---|---|
| module | Exact `ProgramModule` ID, revision, canonical hash, and entrypoint |
| runtime profile | Exact game/runtime/disc compatibility requirements |
| state policy | Explicit `Boot`, `LoadArtifact`, `RestoreBaseline`, or `ContinueSession` |
| execution policy | Explicit movie, input, capture, deadline, and budget policy |
| inputs | Typed values and immutable artifact references bound by declared input name |
| dependencies | Exact action, type-schema, and capability-pack versions |
| attempt identity | Unique execution attempt under one stable semantic step or frontier node |
| idempotency key | Hash of workflow identity, semantic operation, exact inputs, and module closure |

The invocation ends with one `ProgramResult`. A program cannot leave an invocation suspended across a
worker lease, workflow restart, user-selection wait, or frontier scheduling decision.

### Typed phase-to-phase bindings

A workflow edge binds one declared producer output to one declared consumer input. The binding contains:

- producer workflow step and exact module identity;
- producer output name and schema identity;
- artifact/reference kind and immutable identity;
- consumer workflow step, exact module identity, input name, and schema identity;
- binding source (`upstream`, `external_override`, `selection`, or `workflow_generated`);
- compatibility/coercion rule identity when an explicit converter is used; and
- lineage and causation identifiers.

The workflow definition is rejected before execution when required inputs cannot be satisfied, schema
identities differ, an undeclared coercion is required, or a state artifact is incompatible with the
consumer runtime profile. Runtime routing never guesses by position, result order, or "latest" record.

### State policy at phase boundaries

Cross-phase state is always explicit:

- `LoadArtifact` loads the named immutable state artifact.
- `RestoreBaseline` reloads the invocation's named baseline, even when the same worker ran the previous
  phase.
- `ContinueSession` is allowed only when the workflow explicitly requests it, the upstream result has a
  clean cleanup/session status, the session lineage and `StateEpoch` match, and the target module accepts
  continuation.
- `Boot` starts a new runtime profile without inheriting prior emulation state.

Worker reuse is an optimization and cannot change semantics. A workflow must remain reproducible when
each step runs in a fresh worker.

### Fan-out, barriers, and reduction

Fan-out is expressed as durable child work, not as worker-owned threads:

1. A producer commits its immutable output artifacts.
2. The workflow validates and deduplicates the proposed child work.
3. The workflow appends or readies bounded child invocations.
4. A barrier evaluates durable child states, not worker-local counters.
5. A deterministic reducer consumes an ordered set of accepted artifact identities.
6. The reducer publishes a new immutable aggregate/refinement.

The ordering supplied to a reducer is canonical and independent of job completion order. A reducer must
declare its algorithm/version and input ordering policy. Re-running the same reduction over the same
input identities must produce the same semantic result and canonical hash.

### First-class frontier contract

An arbitrary-wave search uses a durable `FrontierSpec`, `FrontierNode`, `FrontierEdge`, and
`ExpansionResult`. These are logical contracts; storage layout is deferred.

`FrontierSpec` contains:

- frontier identity and workflow owner;
- strategy: `DepthFirst`, `BreadthFirst`, or `BestFirst`;
- exact expansion module/entrypoint and dependency closure;
- source-state schema and child-state schema;
- node fingerprint and dedupe-policy versions;
- priority/tie-break policy;
- goal and pruning policy identities;
- maximum depth, node, artifact-byte, attempt, and wall-clock safeguards; and
- terminal policy for exhausted, goal-found, budget-exhausted, canceled, or failed frontiers.

`FrontierNode` contains:

- stable node identity and frontier identity;
- optional parent node and incoming edge identity;
- immutable source `StateArtifact`;
- canonical fingerprint and dedupe key;
- depth, deterministic discovery ordinal, strategy priority, and tie-break key;
- semantic status: `Discovered`, `Ready`, `Claimed`, `Expanded`, `Goal`, `Terminal`, `Pruned`,
  `Duplicate`, or `Failed`;
- current lease/attempt identity and complete attempt history;
- exact expansion invocation identity; and
- accepted expansion/result artifact identities.

`FrontierEdge` contains:

- parent and child identities;
- exact transition/action artifact;
- edge label and typed metadata;
- producer invocation and attempt;
- child-state artifact identity; and
- acceptance, duplicate, or pruning decision with policy version.

An expansion program returns one `ExpansionResult` containing:

- expanded node identity and exact source-state identity;
- zero-to-many proposed child state and edge artifacts;
- zero-to-many typed facts or observations;
- local terminal/goal observations;
- domain outcome and diagnostics; and
- complete provenance for the program, actions, runtime profile, and attempt.

The expansion program proposes children. The workflow alone validates compatibility, calculates the
canonical fingerprint, deduplicates, persists edges, accepts children, and chooses what becomes ready.

### Deterministic strategy semantics

All strategies use a stable final tie-break by discovery ordinal and node identity:

- `DepthFirst` chooses the greatest eligible depth first and preserves a deterministic child order.
  The durable ready set is the DFS stack; no worker process owns a private recursive stack.
- `BreadthFirst` chooses the least eligible depth first.
- `BestFirst` chooses the lowest canonical policy score first, followed by depth and the stable
  tie-break. A score change creates a new policy/versioned frontier rather than silently reordering an
  existing historical run.

Parallel claims may relax physical completion order but cannot change accepted child ordering,
deduplication, or final reduction. A strictly reproducible DFS profile may set claim concurrency to one;
a parallel DFS profile must record its claim-width and deterministic commit policy.

### Frontier transaction boundary

Accepting an expansion is one idempotent orchestration transaction:

1. verify the node is claimed by the completing attempt;
2. verify the exact invocation and source artifact;
3. commit or verify all referenced immutable artifacts;
4. evaluate goal/pruning rules;
5. calculate child fingerprints and dedupe decisions in canonical child order;
6. insert edges, accepted nodes, duplicates, and facts;
7. mark the parent expanded or terminal;
8. ready eligible nodes according to the strategy and concurrency policy; and
9. append lifecycle events/outbox records.

Replaying the same completion cannot create a second semantic node or edge. A late result from an expired
lease is retained as diagnostic evidence but cannot advance the frontier.

### Phase switching

Switching phase means selecting a different module/entrypoint and binding exact outputs/artifacts into
its inputs. It does not select a different worker controller. The required `A -> B -> A` behavior is:

1. invoke phase A with an explicit state policy;
2. fully unwind A's scoped resources;
3. bind A's typed output/state artifact to B;
4. invoke B and fully unwind B;
5. bind B's output/state artifact to a new invocation of the exact A module revision; and
6. prove that the second A invocation observes no breakpoint subscriptions, input leases, patches,
   capture handles, movies, or epoch-bound handles leaked by the first A or B invocation.

### Program family metadata

`ProgramKind` may remain as semantic/UI metadata or a legacy read-only label. It is not the key used to:

- select an interpreter or controller;
- select worker affinity;
- materialize phase-specific context;
- route workflow transitions; or
- identify a program revision.

Workflow policy resolves by workflow definition, module contract, entrypoint, and typed outputs.

## Interfaces and ownership affected

| Component | Owns | Must not own |
|---|---|---|
| Workflow definition/catalog | Authored graph, typed bindings, policies, module requirements | Worker session objects |
| Workflow coordinator | Durable transitions, fan-out, barriers, retries, selection, completion | Dolphin advancement or program interpretation |
| Frontier service | Nodes, edges, leases, dedupe, strategy, goals, budgets | Guest-memory logic or emulator state |
| Program invocation materializer | Exact module and typed input resolution | Phase-specific execution control |
| Result ingestor | Contract validation, artifact/reference verification, idempotent workflow commit | Retrying inside a worker |
| Artifact/state stores | Immutable bytes, hashes, schema/media identity, lineage | Workflow scheduling decisions |
| WorkerRuntime | One serialized live session and bounded invocations | Durable graph/frontier state |
| ProgramRuntime | Verify and execute one module invocation | Worker allocation or workflow mutation |
| Domain reducer | Deterministic aggregate from explicit immutable inputs | Mutable shared navmesh/frontier state |

The existing `IWorkflowTransitionHandler` responsibility is split:

- domain result interpretation becomes a typed, versioned workflow policy/reducer;
- dynamic child acceptance and scheduling become frontier/workflow commands; and
- worker activation resolves through `ProgramInvocation`, not `ProgramKindDescriptor`.

## Failure and cleanup behavior

- **Infrastructure retry:** a worker crash, transport failure, lease loss, or retryable emulator failure
  creates a new attempt for the same semantic step/node and exact invocation. It does not create a new
  semantic child.
- **Domain terminal result:** locked path, no collision anomaly, no successor, or search dead end can be
  a valid successful analysis result and is not retried unless its policy explicitly says otherwise.
- **Contract failure:** wrong module hash, missing output, schema mismatch, incompatible state artifact,
  or undeclared capability fails before workflow advancement.
- **Cleanup failure:** a result whose session cleanup is not clean is durable diagnostic evidence, but
  cannot authorize `ContinueSession`; the worker is quarantined or destroyed before more work.
- **Partial artifact publication:** workflow references are committed only after immutable artifact
  storage and hash verification succeed. Orphaned unreferenced bytes may be garbage-collected later.
- **Lease expiry:** an expired claim becomes eligible for retry. A stale completion cannot overwrite the
  accepted attempt.
- **Duplicate child:** record the edge and duplicate decision when useful for provenance, but do not
  schedule another expansion of the canonical node.
- **Budget exhaustion:** marks the frontier with a distinct analyzed terminal reason and preserves
  unexpanded nodes; it is not reported as ordinary infrastructure failure.
- **Cancellation:** prevents new claims, requests cancellation of active attempts, records their cleanup
  results, and retains all already committed nodes, edges, and artifacts.
- **Barrier failure:** a barrier may continue with explicitly permitted partial inputs only when the
  workflow definition declares that policy. The default is fail closed.

## Dependencies and migration implications

This contract depends on the module, action, invocation, result, artifact, and `StateEpoch` contracts in
documents 03 through 05.

Migration must:

1. preserve the current transactional workflow lifecycle and outbox/recovery behavior;
2. replace program-kind-owned transition topology with workflow-owned typed policies;
3. extend current step outputs and input bindings with schema and exact module provenance;
4. introduce first-class frontier persistence rather than encoding arbitrary DFS as an unbounded chain
   of ad hoc `spawn_steps`;
5. retain deterministic dynamic-step keys as a compatibility lesson and use stable node/dedupe keys;
6. migrate BattleSingleTurn survivor fan-out as the first existing arbitrary-wave exemplar; and
7. drain or terminate in-flight legacy jobs at cutover rather than serializing live `ProgramInstance`
   state across runtime versions.

The Navmesh Survey uses ordinary workflow fan-out and a finite barrier. Overworld exploration uses the
same frontier service with a different game capability pack and an arbitrary number of expansion waves.

## Acceptance criteria

- A workflow definition with incompatible producer/consumer schemas is rejected before materialization.
- The same terminal completion replayed twice creates no duplicate steps, nodes, edges, or accepted
  artifacts.
- Worker failure after artifact upload but before workflow commit recovers by verifying and committing
  the same immutable artifacts.
- Worker failure before cleanup never allows session continuation.
- `DepthFirst`, `BreadthFirst`, and `BestFirst` fixtures produce their specified canonical acceptance
  order despite randomized worker completion order.
- A repeated child state is represented once as a canonical node and retains all incoming edge lineage.
- A frontier survives coordinator and worker restart without losing ready nodes, visited fingerprints,
  attempts, or strategy position.
- BattleSingleTurn can be expressed as bounded turn expansion plus workflow-owned survivor selection and
  next-wave creation.
- Navmesh Survey anchor establishment reaches a durable barrier before spatial-probe fan-out.
- An `A -> B -> A` workflow runs the same universal executor for every phase and proves zero leaked
  session resources.
- Adding a phase that uses existing actions changes only its program module, schemas, and workflow
  bindings; it changes no `WorkerRuntime`, `ProgramRuntime`, `ExecutionEngine`, router, wire switch, or
  central opcode table.

## Deferred work

- Concrete SQL tables, indexes, archive format, and wire representation.
- Workflow-authoring UI for frontier policies and typed conversions.
- Distributed frontier sharding beyond one authoritative database.
- Speculative duplicate execution and result racing.
- Exact priority scoring for collision, navigation, battle, and overworld domain policies.
- Whether some deterministic CPU reducers use a dedicated local executor or a general non-Dolphin worker
  profile.
- User-facing visualization of duplicate edges, pruned branches, and partially exhausted frontiers.

## Source references

- `SavorDb/Execution/Workflow/WorkflowOrchestration.h`
- `SavorDb/Execution/Workflow/SqliteWorkflowOrchestration.cpp`
- `SavorDb/Execution/Workflow/WorkflowTerminalAdvancementService.cpp`
- `SavorDb/Execution/Workflow/WorkflowTerminalOutboxSubscriber.cpp`
- `SavorDb/Execution/Workflow/WorkflowGraphRoutingService.cpp`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h`
- `SavorDb/Execution/ProgramDB/BattleSingleTurn/BattleSingleTurnAdapters.cpp`
- `SavorTests/test_savordb_fixture_sqlite.cpp`
- `SavorTests/test_savordb_phase3_nonfixture.cpp`
- `planning/NavigationPhase/NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`
