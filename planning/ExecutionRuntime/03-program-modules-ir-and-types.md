# Program Modules, IR, and Types

## Scope

This document describes the target meanings of `ProgramModule`, entrypoint, function,
`ProgramInstance`, canonical IR, type/schema dependency, verification, and compilation frontend.
Concrete C++ declarations may adapt during later migration, while the implemented
`SPRM`/`SPRI`/`SPRR` version-1 canonical boundary remains an exact compatibility contract. One verified
typed model and one executor remain the required architecture.

## Purpose and non-goals

The program model must represent every bounded worker phase with one verifier and one executor. It must
support compiled built-ins, future user-authored sources, reusable subprograms, exact dependency
identity, typed inputs/outputs, suspension on bounded actions, structured cleanup, replay diagnostics,
and migration from current builders.

The model must make domain growth occur in modules and registered capabilities rather than in an
ever-growing central opcode switch.

This document does not:

- define the authoring syntax or editor;
- change SavorDb SQL/schema, migrations, stored representations, database-service interfaces, queues,
  claims, workflow persistence, transaction boundaries, or artifact-storage interfaces;
- create separate observation, interaction, predicate, or capture-language projects during this
  refactor; the composition libraries use the existing typed module-builder surface;
- make canonical program envelopes a worker protocol or database persistence format;
- permit a program to schedule workers or own a durable search frontier;
- define action service implementations, which are governed by document 04; or
- preserve the current `PSContext`, opcode numbering, labels, or `ProgramKind` switches as public ABI.

## Current code evidence

The current system has several useful seeds but does not provide a general module contract:

- `SavorCore/Runner/Script/PhaseScriptProgram.h:13-17` defines a program as canonical breakpoint keys,
  gated breakpoint keys, and one flat vector of `PSOp`.
- `PhaseScriptProgram.h:25-40` defines initialization, job, and result contracts separately as
  `PSInit`, `PSJob`, and `PSResult`; the result is a boolean, worker-error byte, and `PSContext`.
- `SavorCore/Runner/Script/PSContext.h:10-55` is an unscoped key/value map whose closed variant mixes
  primitive values with `GCInputFrame` and battle-specific `BattlePath`.
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc:6-57` currently contains 52 entries. Generic flow,
  memory, input, snapshots, movies, breakpoint control, capture, battle macro materialization, seed
  capture, and navigation-context extraction are peers in one opcode catalogue.
- `SavorCore/Runner/Script/PhaseScriptVM.cpp:170-325` implements that catalogue in one exhaustive switch.
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109` uses `ProgramKind` switches both to construct a
  compiled program and to choose its payload decoder.
- `SavorCore/Runner/IPC/Wire.h:63-83` exposes stable numeric program families, but no module revision,
  content hash, entrypoint, action dependency, or type dependency.
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:42-59` provides a useful typed
  `Start`/`Advance` continuation shape. Today it is a separate mini-runtime hosted by
  `PhaseScriptVM`, not a general program facility.

The existing program builders prove that declarative control flow is useful. The current opcode and
context shapes are migration inputs, not the target ABI.

### Implemented Slice 5 model

Slice 5 implements the canonical typed boundary under
`SavorCore/Runner/Runtime/ProgramRuntime`:

- `SPRM` version 1 encodes the complete normalized `ProgramModule`;
- `SPRI` version 1 encodes one typed `ProgramInvocation`;
- `SPRR` version 1 encodes the three-axis `ProgramResult`;
- every envelope uses a four-byte magic, little-endian version and payload length, canonical ordering,
  bounded decoding, and rejection of malformed, noncanonical, oversized, or trailing input; and
- module identity is SHA-256 over canonical `SPRM` bytes with the declared module hash omitted.

The same implementation provides the bounded value graph/arena, definition store, exact type/action/
capability registries, verifier, `ProgramExecutor`, and one `ProgramRuntime`. Program action requests
leave the runtime only through the WorkerRuntime actor mailbox and return as correlated queued
completions; the runtime obtains neither `EmulationSession` nor a backend/service escape hatch.
`SessionResourceBindingTable` maps program resource identities onto the existing Slice 4 ledger.
The concrete internal `SessionProgramActionHost` translates canonical requests into those
session-owned services and returns completions through the same actor queue.

This implementation checkpoint is not a production activation claim. No current phase has been
translated, the production worker constructs neither this runtime nor its action host and does not
advertise `ProgramInvocation`, and a live game-program smoke and production-worker SavorE2E remain
deferred.

## Core runtime model

### One executable definition

Every built-in, generated, or user-authored program executes as a verified immutable
`ProgramModule`. There is no alternate native-program controller and no `PK_UserScript` executor.

The logical module contains:

| Field | Required meaning |
|---|---|
| `module_id` | Stable namespaced semantic identity, such as `soa.battle.single_turn` |
| `revision` | Immutable revision within that module family |
| `module_hash` | Canonical content identity for the complete normalized module |
| `ir_version` | Exact canonical IR contract consumed by the verifier/executor |
| `entrypoints` | Named typed public functions that may be invoked by a workflow/job |
| `functions` | Private and exported IR functions organized as typed CFGs |
| `local_types` | Module-owned enum, record, and other schema definitions |
| `module_imports` | Exact imported module IDs, revisions, and hashes |
| `action_imports` | Exact action ID, version, and signature hash for every awaited action |
| `reducer_imports` | Exact identity and signature of every imported pure native reducer |
| `type_imports` | Exact type/schema ID, version, and hash dependencies |
| `required_capabilities` | Capability/effect requirements checked before activation |
| `accepted_policies` | State, runtime, movie, capture, replay, and debug policies the module permits |
| `budgets` | Maximum instructions, calls, action requests, emissions, values/bytes, and elapsed deadline |
| `source_map` | Versioned mapping from IR locations to builder or authored source |

Exact identity and replay fields are further governed by document 05. Module imports and registered
dependencies are pinned before publication; runtime resolution never silently selects "latest."

The canonical module is immutable. Editing an entrypoint, function, schema, dependency, budget, or
source map publishes another revision/hash. Deployment path, cache location, and worker-local
verification status are not part of its semantic identity.

### Entrypoints and functions

An entrypoint declares:

- a stable name;
- exactly one input record schema;
- exactly one output record schema or explicit `unit`;
- one domain-outcome enum/schema;
- the record types it may emit zero-to-many times;
- the artifact roles/schema families it may publish zero-to-many times;
- accepted invocation state/execution policies;
- required capability packs; and
- entrypoint limits that may narrow, but never exceed, module budgets.

An entrypoint is a normal IR function with public invocation metadata. Private functions and imported
subprograms use the same call semantics and verifier.

Examples aligned with the migration matrix are:

- module `soa.seed_probe`, entrypoint `probe`;
- module `soa.battle.single_turn`, entrypoint `execute`;
- module `soa.navigation.context`, entrypoint `capture`; and
- future module `soa.navigation.survey`, entrypoints `establish_anchors` and `probe_geometry`.

The name chooses behavior inside an exact module. It does not select a controller type.

### Type system

The canonical value system contains:

- `unit` and `bool`;
- fixed-width `u8`, `u16`, `u32`, `u64`, `i32`, and `i64`;
- IEEE `f32` and `f64`;
- bounded UTF-8 `string`;
- bounded opaque `bytes`;
- closed `enum`;
- `optional<T>`;
- named `record` with fixed typed fields;
- bounded homogeneous `list<T>`;
- typed immutable `artifact_ref<TSchema>`;
- typed `resource_handle<TResource>`; and
- typed opaque guest/session handles declared by an action schema.

Rules:

1. Every IR value has one statically known type. There is no untyped key dictionary.
2. Record and enum identity includes exact schema ID/version/hash, not just a matching display name.
3. Numeric widening, narrowing, signedness changes, and integer/float conversion are explicit checked
   operations. Overflow, invalid narrowing, NaN policy violations, and out-of-range indexing are typed
   program failures.
4. Strings and byte arrays carry verified maximum sizes. Lists carry verified maximum element counts.
5. `optional<T>` is inspected before extraction; an unchecked missing value cannot reach an action.
6. Artifact references may leave the runtime in `ProgramResult`. A program-kind result adapter maps them
   to the existing SavorDb artifact/domain representation; this refactor does not persist
   `artifact_ref<TSchema>` directly. Resource and opaque handles may not cross the runtime boundary.
7. Guest-derived opaque handles are tagged with the `StateEpoch` in which they were produced. Using one
   after state replacement is an infrastructure contract failure.
8. Host pointers, object addresses, service references, callbacks, threads, and arbitrary C++ objects
   are not values.
9. Program inputs are immutable. Computed values use function-frame values and block arguments rather
   than mutating a global ambient context.
10. Action/domain errors intended for program branching are explicit typed records/enums. Backend,
    capability, verifier, or contract failures are not disguised as ordinary values.

`GCInputFrame`, battle paths, navigation positions, stop observations, and similar current values become
named schemas owned by their generic or game capability pack. They do not expand a global closed
variant.

### Canonical control-flow representation

Every function is a typed control-flow graph:

- a finite set of uniquely identified basic blocks;
- one entry block;
- typed function arguments;
- typed block arguments for values carried across edges and loops;
- instructions whose results are immutable typed values; and
- exactly one terminator per block.

The executor may store frame values in an efficient register/local representation, but the logical IR
has no dynamically named `PSContext` slots. Loops carry state through block arguments. Calls create
ordinary typed call frames.

The stable core instruction families are:

| Family | Permitted semantics |
|---|---|
| Constants and values | Create constants, copy/select values, and perform explicit checked conversions |
| Records/options/lists | Construct, project, update-by-copy, inspect, index, append, and measure bounded typed values |
| Arithmetic and logic | Checked integer arithmetic, declared float arithmetic, boolean operations, and comparisons |
| Control flow | Unconditional branch, conditional branch, enum switch, and block arguments |
| Calls | Call an IR function/imported subprogram or an exact imported pure reducer |
| Effects | Await one exact imported action with typed inputs and receive its typed completion |
| Emissions | Emit a declared typed record or publish a declared artifact reference |
| Resource structure | Enter/exit a lexical scope and register a verified deferred compensation |
| Termination | Return a declared output/domain outcome or fail with a structured diagnostic |

The following are not core instructions:

- run to a Skies breakpoint;
- materialize a battle command;
- read the navigation context;
- suppress an encounter or trigger;
- load a specific phase savestate;
- fast-forward a cutscene;
- open a door;
- expand an overworld node; or
- execute an input macro.

Those behaviors are compositions of generic IR, imported actions, pure reducers, and reusable
subprograms. Adding one cannot require a new `ProgramExecutor` switch case.

### Reusable semantic-observation composition

The semantic-observation composition library is a typed module-builder/frontend library for saying
"wait for this logical game point and acquire this evidence there." It lowers completely before module
verification. It is not an `ObservationRuntime`, query VM, action family, persistence system, or second
execution mechanism.

The reusable authoring model contains:

- `SemanticPointDefinition`: an exact capability-pack-owned logical point mapped by the pack to a PC,
  memory, or synthetic source. Programs import the logical identity and never manipulate its physical
  breakpoint/watchpoint representation.
- `SemanticAwaitDefinition`: one or more exact point alternatives, optional bounded hit-time
  qualification/sample requirements, explicit current-point acceptance, deadline/stall/movie/cancel
  policies, and rearm/current-instruction-suppression policy. It lowers to a scoped logical router
  subscription and `runtime.execution.continue_until`.
- `SemanticPointReceipt`: the matched logical point, physical hit evidence, stop sequence,
  `StateEpoch`, and any declared hit-time samples. A receipt identifies one routed event; it does not
  grant control authority.
- `AddressExpression<T>`: a pure, bounded, typed address derivation from registered symbols,
  compatibility-pinned addresses, checked dereferences/offsets, or receipt fields. Current address
  expressions used by program/predicate logic may be translated through this boundary without making
  their present syntax or encoding the new canonical IR. Address programs embedded in
  `savor.capture.profile/1` remain opaque to `CaptureService` and are not translated here.
- `ObservationDefinition<T>`: an exact typed query/read recipe, result schema, required capabilities,
  consistency requirement, and permitted acquisition modes. A coherent multi-field domain observation
  is one registered query rather than a collection of scalar reads falsely treated as atomic.
- `ObservationUse<T>`: a use-site binding to a receipt or current paused point, required/optional
  policy, one selected permitted acquisition mode, optional named-baseline behavior, and authoritative
  emission versus telemetry policy.

There are exactly two observation acquisition modes:

1. `HitTimeSample` is a bounded, allocation-free sample executed synchronously by the router before
   ordinary program handling. Only the router's restricted registered sample subset may be used.
2. `PausedAtPoint` uses ordinary registered guest-read or game-query actions after the router/execution
   result proves the requested pause and before the program explicitly advances again.

Observing after a reached instruction is not a hidden third mode. The composing program explicitly
awaits `runtime.execution.step_instructions` or `runtime.execution.step_frames` and then performs a new
paused observation. Ordered observation uses lower in their declared order. Required missing evidence
is a structured action/runtime failure; optional missing evidence is `Unavailable`, distinct from
`false`, zero, or an ordinary domain-negative result.

Named baselines are ordinary typed IR values. Each use declares `First` or `Latest` update policy and
the exact comparison point. Translation of current battle predicates uses `Latest`: acquire and update
the baseline before evaluating the predicate at that same hit. Receipts, observations, baselines,
derived addresses, and guest-derived handles carry their originating `StateEpoch` and cannot be reused
after state replacement.

Lowering generates only exact imports, ordinary scoped subscriptions, execution and read/query action
awaits, typed values, branches, and declared emissions. Its normalized IR, source mapping, schemas, and
dependency closure participate in the module hash. The composition library cannot access Dolphin,
session services, the filesystem, SavorDb, or mutable global state.

### Reusable interaction composition

The interaction composition library is a typed module-builder/frontend library for bounded static or
adaptive input sequences. It uses semantic awaits and observations and lowers to ordinary IR
subprograms. It does not introduce an `InteractionRuntime`, `InputMacroEngine`, input controller, whole
macro action, scheduler, or new opcode family.

An `InteractionDefinition<State, Output>` declares:

- a stable source/diagnostic identity and immutable revision;
- typed reducer state and completion output;
- pure initialization and advancement reducers;
- a finite verifier-known set of segment definitions;
- hard instruction/action/emission/elapsed budgets; and
- the typed records/artifacts it may emit.

Each `InteractionSegmentDefinition` declares one bounded unit:

- exact semantic-gate alternatives;
- the requested held, pulse, neutral, or sequence input;
- request and release guest-poll acknowledgement requirements;
- source-stop step-off and reached-instruction execution policies;
- ordered observations and checks at the permitted acquisition moments;
- deadline, stall, movie-ended, cancellation, and current-instruction-suppression policies; and
- the typed completion mapping returned to the reducer/statechart.

`InteractionSegmentResult` contains the exact semantic-point receipt, requested-input and neutral-release
receipts when applicable, ordered observations/check results, elapsed evidence, originating
`StateEpoch`, and a distinct terminal status. Timeout, unexpected point, unacknowledged input,
unsatisfied check, infrastructure failure, cancellation, and cleanup failure cannot collapse into one
boolean.

Static interactions lower to ordinary subprogram CFG. Adaptive interactions lower to a reusable IR
statechart whose pure reducer consumes only typed state and the prior segment result. The reducer may
select only a segment ID in the definition's verifier-known finite set, return completion, and request
declared emissions; it cannot construct effects or subscriptions dynamically. The lowered program owns
one lexical input lease for the complete interaction, nests segment subscriptions/observations beneath
it, and uses the universal runtime cancellation and unwind path.

The lowering must preserve the temporal contract in document 04, including input publication before
stepping off a current stop, exact stop/sequence/epoch matching, the reached-instruction policy,
request-receipt capture before neutralization, independently witnessed neutral release where required,
and baseline-before-advance behavior. Existing `Start`/`Advance` drivers translate to pure
initialize/transition reducers; their `Cancel` and private cleanup paths are replaced by ordinary
runtime cancellation and structured unwind.

### Reusable predicate composition

The shared predicate composition library is a typed module-builder/frontend library. It is not a runtime
service, executor, action family, durable catalog, or persistence system.

A reusable `PredicateDefinition` contains:

- a stable diagnostic/source identity;
- typed witness inputs; and
- a pure condition over those values.

The definition contains no abort, progress, scoring, retry, waiting, resource, or persistence policy. It
cannot access session services, Dolphin, the filesystem, SavorDb, the clock, or mutable global state. Its
evaluation lowers to ordinary arithmetic, logic, comparisons, and control flow, an exact reusable IR
subprogram, or an imported pure reducer.

A `Check` is one use of a predicate definition. Each check separately declares:

- its semantic evaluation point;
- the exact `ObservationDefinition`/`ObservationUse` values and optional named baseline needed to
  supply the witnesses;
- whether each witness is required or optional;
- a use policy such as branch, return a typed domain rejection, explicitly fail, emit an evaluation
  record, or accumulate a result for later scoring; and
- whether a declared typed `ConditionObservation` is emitted.

Predicate lowering may produce only ordinary canonical constructs:

- the canonical constructs produced by semantic-await/observation lowering;
- pure IR or reducer calls for evaluation;
- ordinary conditional branches, returns, or structured failures; and
- declared typed emissions, with optional telemetry for live presentation.

Each evaluation produces one of `Satisfied`, `Unsatisfied`, `NotApplicable`, or `Unavailable`.
`Unsatisfied` is an ordinary typed result and becomes terminal only because the check's use policy says
so. Failure to obtain required evidence is an action/runtime failure and must not silently become
`Unsatisfied` or be ignored. Epoch-bound baselines and witness values cannot be reused after
`StateEpoch` changes.

The library lowers predicates before module verification. The verifier receives only canonical IR,
normal imports, schemas, scopes, and emissions; it accepts no separate module-level predicate bytecode.
This restriction does not reinterpret the opaque capture-profile filter/predicate bytecode owned by
`CaptureService`. The executor has no predicate-specific instruction, registry, or dispatch path.
Equivalent predicate-composed and hand-composed behavior should normalize to equivalent executable IR
and dependencies.

Predicates do not independently define timing, address resolution, baseline lifetime, or guest reads.
They consume the typed results of the semantic-observation composition library. This keeps one
authoritative acquisition contract for predicate checks, interaction progress, and ordinary module
branching.

### Calls, loops, reducers, and boundedness

IR functions and imported module subprograms use one call mechanism. Imports are linked to an exact
dependency closure before invocation.

Pure native reducers use that same call boundary:

- the reducer receives only typed state and a typed completed observation/effect;
- it returns a typed transition containing new state, requested next work, emitted domain events, and
  optional completion;
- it performs no I/O and cannot suspend; and
- a reusable IR statechart loop interprets the transition and performs any requested action through
  ordinary `await action`.

This retains useful adaptive `Start`/`Advance` behavior without creating a peer `InputMacroEngine`.
Native reducer constraints are detailed in document 04.

Loops and recursion are permitted only under finite budgets:

- every invocation has a hard instruction budget;
- every module has a hard call-depth budget;
- every action wait consumes the action-request budget;
- every backward CFG edge is visible to tracing and budget accounting; and
- a recursive call-graph component is accepted only when a finite call-depth limit is declared.

Arbitrary-depth durable search is not implemented as a long-running program loop. One invocation emits
bounded successors; existing SavorDb transition mechanisms schedule later waves through unchanged
workflow persistence. Generalized frontier storage is outside this refactor.

### Action-await semantics

`await action` is the only effect boundary in the IR.

At that boundary:

1. `ProgramExecutor` constructs a typed request from the exact imported action signature.
2. `ProgramRuntime` verifies the action identity, signature, capability set, effect allowance, budgets,
   current scope, and epoch requirements.
3. The instance records one pending continuation and stops advancing.
4. `ActionRegistry` dispatches the request to the bounded handler.
5. A completion returns with invocation ID, request ID, origin `StateEpoch`, typed output, resource
   receipts, and diagnostics.
6. `ProgramRuntime` rejects stale, duplicate, mismatched, or schema-invalid completions.
7. `ProgramExecutor` binds the output and resumes the saved continuation.

An instance has at most one program-level awaited action at a time. Structured child operations inside
an action remain owned by session services and complete before the one action completion is delivered.
This restriction makes cancellation, trace order, replay, and resource ownership unambiguous.

### Structured scopes and defer

Every entrypoint starts with one root resource scope. IR may nest lexical scopes.

- Resources returned or attached by an action are registered in the current scope automatically.
- `defer` may register only an imported compensation action marked cleanup-safe and idempotent for that
  receipt type.
- Normal scope exit performs its verified releases/compensations in reverse acquisition order.
- Return, fail, cancellation, timeout, budget exhaustion, guard abort, and backend failure unwind every
  open scope through the same runtime path.
- A resource may be promoted only to an enclosing scope by an explicit typed operation allowed by its
  descriptor. It can never be emitted to a workflow as a live handle.
- An artifact created by finalizing a resource is immutable durable output; it is not the resource
  handle itself.

Any workflow-facing artifact representation is projected through the existing program-kind adapter and
persistence contract.

Program code cannot catch or ignore a mandatory cleanup failure. The runtime records it and applies
session-taint policy.

The implemented `SessionResourceLedger` is a standalone actor-owned facility beneath
`ProgramRuntime`, not a data structure hidden inside `ProgramExecutor`. Slice 4 initializes one session
root and permits synthetic nested scopes so every generic service can use the cleanup boundary before
program execution exists. Slice 5 adds actor-owned binding between invocation resource identities and
that same ledger without changing the ledger's contracts.

The ledger assigns monotonic receipt/acquisition identities and records owner, service, scope,
acquisition epoch, release descriptor, promotion policy, cleanup requirement, and one of
`EpochAgnostic`, `EndOnEpochChange`, `RebindAfterRestore`, or `ReplacesState`. Batch acquisition is
atomic. Scope unwind is reverse acquisition order, continues after independent failures, and may suspend
only for a typed cleanup-execution continuation. Optional release failure yields clean-with-diagnostics;
unproven mandatory cleanup yields taint. A successful state transaction closes or requests rebind for
old-epoch resources before accepting the new epoch.

### ProgramInstance

`ProgramInstance` is mutable execution data for exactly one invocation. It contains:

- exact verified module/dependency and entrypoint references;
- immutable invocation inputs;
- current function, block, instruction position, and block arguments;
- typed call frames and their computed values;
- pending action request and continuation, if suspended;
- lexical resource/defer stack;
- current `StateEpoch` and all epoch-bound handles;
- remaining instruction, call, action, emission, memory, artifact, and deadline budgets;
- emitted-record and artifact-reference builders;
- branch/action trace correlation;
- structured diagnostics; and
- terminal result construction state.

It does not contain:

- a phase-controller subtype or virtual behavior;
- worker queues, workflow IDs used for scheduling, or frontier mutation methods;
- an OS thread, event loop, timer thread, or callback into the protocol;
- a `DolphinBackend`, `EmulationSession`, or broad service reference; or
- live state that can be serialized and resumed on another worker.

After a worker crash, the existing coordinator and program-kind handler rematerialize the immutable
runtime invocation from existing persisted job/domain records and state artifacts. Neither
`ProgramInstance` nor a new invocation record is stored.

### ProgramRuntime subsystem

The worker has one `ProgramRuntime` composed of:

- `ProgramDefinitionStore`: retrieves/caches immutable modules by exact identity;
- `ProgramVerifier`: validates IR, schemas, imports, policies, effects, and budgets before state mutation;
- `ProgramExecutor`: advances the canonical IR and only the canonical IR;
- `ActionRegistry`: resolves exact action and pure-reducer registrations; and
- `TypeSchemaRegistry`: resolves exact named type/schema identities.

These are services inside one subsystem, not alternate executors. `ProgramDefinitionStore` is not the
DB workflow phase-adapter registry. `ActionRegistry` stores capability implementations, not arbitrary
phase factories. `ProgramRuntime` drives the session-owned resource ledger for invocation scopes and
receipts; it does not implement a second cleanup ledger.

For this refactor, `ProgramDefinitionStore` is a worker/runtime catalog and cache backed by compiled or
packaged definitions. It is not SavorDb persistence.

The Slice 5 implementation supplies these exact components and the canonical runtime port. Production
composition intentionally does not construct that port yet, so the existence of the subsystem does not
advertise worker invocation capability.

### Verification

Verification completes before state preparation or any action:

1. Resolve the exact module hash and full module/action/reducer/type dependency closure.
2. Check `ir_version`, source-map integrity, and canonical hash.
3. Check unique function/block/value identities and well-formed terminators.
4. Check dominance, definition-before-use, block-argument arity, and exact type agreement on every edge.
5. Check all record fields, enum cases, optional extraction, list bounds metadata, and explicit numeric
   conversion.
6. Check function/reducer/action call signatures and return types.
7. Check entrypoint input, output, domain-outcome, emission, and artifact declarations.
8. Check action capability/effect imports against the module and entrypoint allowance.
9. Check lexical scope structure, deferred compensation descriptors, and resource promotion rules.
10. Check recursion, call-depth, instruction, action, value/byte, emission, artifact, and deadline
    budgets.
11. Check accepted invocation policies and runtime compatibility.
12. Produce a verified-module object that references only the imported dependency closure.

A verifier failure is deterministic, locatable through the source map, and rejects the invocation
before Dolphin state is changed.

### Compilation frontends and migration

All frontends compile to the same canonical module:

```text
Current C++ phase builders --\
Legacy PhaseScript adapter ---+--> typed module builder --> verifier --> ProgramModule
Future authored DSL/JSON -----/
Generated research program --/
```

The typed module builder is an authoring convenience, not another executor. During this refactor,
modules, revisions, and runtime type schemas come from compiled or packaged runtime definitions.
Persisting module source or canonical modules in SavorDb is separate future work and is not part of this
refactor.

The semantic-observation, interaction, and predicate composition libraries are shared by these
frontends. Static definitions may lower during the ordinary build. Existing macro, non-capture
address-expression, and battle-predicate records may be translated in memory by the runtime-facing
program-kind adapter/module builder and lowered before the resulting module is verified. Normalized IR,
exact dependencies, schemas, and source mapping participate in the module hash; the current SavorDb
representations remain unchanged. Persisting composition-library source, normalized definitions, or
reusable catalogs is outside this refactor.

Existing `savor.capture.profile/1` inputs are not another compilation frontend. They remain opaque
versioned capture configuration passed through `runtime.capture.attach` to `CaptureService`. Neither
semantic-observation nor interaction composition interprets or lowers profile internals. A generalized
capture-plan language is deferred.

The legacy adapter may translate current `PhaseScript`, payload decoding, and result mapping into
canonical modules for differential testing. It shall:

- preserve current behavior only where explicitly mapped;
- map current context keys to declared schemas;
- translate generic control flow to core IR;
- translate current machine/domain opcodes to imported actions or subprograms;
- emit diagnostics for unsupported or ambiguous operations; and
- be deleted after the migration gates in document 09.

New capabilities and phases cannot be added only to the legacy representation.

## Interfaces and ownership affected

The target replaces these current public assumptions:

| Current assumption | Target boundary |
|---|---|
| `PhaseScript` is a flat op vector plus breakpoint sets | `ProgramModule` is an immutable typed CFG plus exact imports |
| `PSInit` implicitly loads one savestate and captures one baseline | `ProgramInvocation` declares state policy; `StateService` performs it as the sole epoch authority using explicit handles or caller-declared immutable artifacts with hash, compatibility, lineage, and any exact DTM continuation |
| `PSJob` combines raw payload and ambient context | Entrypoint receives one verified typed input record |
| `PSResult` is `ok`, error byte, and context | `ProgramResult` separates infrastructure, domain, and cleanup status with typed outputs/emissions |
| `PSContext` is inputs, locals, observations, and outputs | Each boundary has an explicit schema; frame values are typed |
| `ProgramKind` selects builder and decoder | Exact module identity plus entrypoint selects behavior |
| Domain operation adds an opcode | Domain operation adds/reuses a registered action or subprogram |
| Physical breakpoint/address operations define module intent | Capability-pack semantic points and typed observations lower to scoped router, execution, and query actions |
| Macro provider owns a second adaptive runtime | Shared interaction composition lowers to an IR statechart, pure reducer, semantic observations, and ordinary actions |
| VM predicate tables combine observation, evaluation, progress, scoring, and `AbortOnFail` | Shared predicate composition consumes typed observations and lowers pure conditions plus separate use policies into ordinary IR, branches, and emissions |
| Capture-profile internals become candidate program syntax | `savor.capture.profile/1` remains opaque configuration owned by `CaptureService` |

The exact logical invocation/result/artifact fields are defined in document 05. Existing SavorDb
program-kind handlers perform workflow integration through existing contracts; document 06 describes
that ownership boundary without changing workflow/frontier persistence.

## Failure and cleanup behavior

Program failures are classified before result assembly:

- **Verification rejection:** malformed IR, missing dependency, schema mismatch, undeclared effect, or
  incompatible policy. No session mutation occurs.
- **Program failure:** explicit `fail`, checked arithmetic/index/conversion failure, exhausted program
  budget, or invalid program state. Open scopes unwind.
- **Action failure:** infrastructure or action-contract failure reaches the pending continuation only as
  allowed by its descriptor; otherwise the invocation fails and unwinds.
- **Domain terminal:** an entrypoint returns a valid domain outcome such as locked, no successor, or no
  anomaly. This can be a clean completed invocation.
- **Predicate evaluation:** `Unsatisfied` follows the check's explicit branch/domain/fail/record policy;
  `Unavailable` remains distinct and follows the required-observation or action-failure contract.
- **Cancellation/timeout:** ordinary flow does not resume; the pending action is cancelled and all
  scopes unwind.
- **Cleanup failure:** diagnostics accumulate, remaining cleanup continues, and the result marks the
  session tainted when mandatory restoration is unproven.

An emitted record is committed to the result only after schema validation. An artifact reference is
published only after its owning service finalizes and verifies the immutable artifact. Partial outputs
are explicitly marked by the result contract; no half-constructed live resource escapes the instance.

## Dependencies and migration implications

This contract depends on:

- the `WorkerRuntime`/`EmulationSession` ownership model in document 02;
- the action, reducer, service, scope, and epoch contracts in document 04;
- invocation, version, result, and artifact identity in document 05; and
- the unchanged SavorDb ownership boundary for durable composition in document 06.

Implementation must define the canonical IR and verifier before serializing user scripts as a permanent
runtime format. Adopting today's opcode/context serialization as the permanent module format would
fossilize the coupling this refactor is intended to remove.

Migration should first compile representative existing modules such as `soa.seed_probe/probe`,
`soa.battle.single_turn/execute`, and `soa.navigation.context/capture`, then migrate all remaining
current phases. Navmesh Survey is the first net-new module after current behavior has a stable universal
execution path.

## Runtime model checks

- One verifier accepts modules from current C++ builders, the temporary legacy adapter, and a synthetic
  authored frontend, producing the same normalized module for equivalent input.
- An exact module hash and dependency closure resolves identically on two workers.
- Invalid CFG edges, types, schemas, action signatures, effects, policies, scopes, epochs, and budgets are
  rejected before state preparation.
- A module can branch, loop under budget, call a subprogram, call a pure reducer, await actions, emit
  zero-to-many records/artifacts, and return a typed result without a domain opcode.
- A suspended `ProgramInstance` contains only typed continuation/state data and no Dolphin or service
  pointer.
- Duplicate or stale action completions cannot resume an instance.
- Cancellation at every instruction/action suspension point takes the same verified unwind path.
- Invocation scopes project onto the standalone session ledger, and no executor-local receipt or cleanup
  stack can disagree with the session's reverse-order unwind, epoch transition, or taint disposition.
- Current macro `Start`/`Advance` behavior can be represented as a typed reducer/statechart without a
  peer executor.
- Equivalent semantic-await/observation composition and hand-authored canonical IR normalize to the
  same imports, scopes, source-map meaning, and executable behavior.
- Hit-time and paused observations remain distinct; post-instruction observation requires an explicit
  step; required-unavailable evidence and an ordinary false/domain-negative value remain distinct.
- Named `First` and `Latest` baselines update at their declared point and every receipt, observation,
  address, and baseline is rejected after its originating `StateEpoch`.
- Static and adaptive interactions lower to verifier-known subprogram CFG and reducer transitions
  without a new opcode, action family, runtime, or dynamically constructed effect.
- Interaction tests distinguish input request from guest-observed release and preserve input-before-step,
  reached-instruction, exact point/sequence/epoch, baseline-before-advance, and common-unwind behavior.
- One predicate definition can be used as record-only and fail-fast at different check sites without
  changing its pure condition.
- Required predicate evidence failure is distinguishable from an unsatisfied condition, and predicate
  lowering introduces no opcode, executor, service, or runtime registry.
- The current phase module IDs and entrypoints in document 07 compile without changes to the executor.
- Future `soa.navigation.survey` exposes `establish_anchors` and `probe_geometry` through the same module
  model.
- Adding an action import or subprogram changes only the importing module's exact dependency closure,
  not unrelated module compatibility.
- No `ProgramKind`, `PK_UserScript`, phase factory, or domain-specific switch participates in execution.

## Deferred work

- Any future authored textual syntax and generated type wrappers. The canonical binary version-1
  envelopes and SHA-256 module identity are implemented and no longer deferred.
- A future version-2 encoding, only if an incompatible model change requires one; version 1 remains
  exact and must not be silently reinterpreted.
- Authored source language, parser, editor, debugger UI, publication workflow, and access control.
- A generalized capture-plan authoring language or translation of `savor.capture.profile/1` into
  program IR. Existing profile behavior remains behind `CaptureService` during this refactor.
- Optimizer, constant folding, dead-code elimination, or JIT compilation; the initial executor may
  interpret verified IR.
- Source-level hot reload. Active invocations always retain exact immutable module identity.
- Any durable catalog or persistence for module source, normalized IR, verification cache, or source maps
  is a separate project. This refactor adds no SavorDb storage for those objects.
- Optional future generic sum/variant types beyond enum-plus-record/optional schemas.
- Performance-driven batching instructions; any addition must remain generic, typed, and non-domain
  specific.
- Translation of current phases, production runtime/action-host construction and capability
  advertisement, a live game-program smoke, and production-worker SavorE2E.

## Source references

- `SavorCore/Runner/Script/PhaseScriptProgram.h:13-40`
- `SavorCore/Runner/Script/PSContext.h:10-55`
- `SavorCore/Runner/Script/PhaseScriptOpcodeTable.inc:6-57`
- `SavorCore/Runner/Script/PhaseScriptOpcodes.h:37-49`
- `SavorCore/Runner/Script/PhaseScriptOpcodes.h:102-183`
- `SavorCore/Runner/Script/PhaseScriptVM.cpp:170-325`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorCore/Runner/IPC/Wire.h:63-83`
- `SavorCore/Runner/InputMacro/IInputMacroPlanDriver.h:42-59`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.h:85-145`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp:45-105`
- `SavorCore/Runner/InputMacro/InputMacroRuntime.cpp:277-427`
- `SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/Model`
- `SavorCore/Runner/Runtime/ProgramRuntime/Store/ProgramDefinitionStore.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/Verify/ProgramVerifier.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/Execution/ProgramExecutor.*`
- `SavorCore/Runner/Runtime/ProgramRuntime/ProgramRuntime.*`
- `planning/ExecutionRuntime/05-invocation-result-versioning-and-artifacts.md`
- `planning/ExecutionRuntime/07-current-phase-migration-matrix.md`
- `D:\SoAInvestigate\Analyses\20260723_2107_savor_worker_breakpoint_router\20260723_2107_savor_worker_breakpoint_router_architecture_summary.txt:499-555`
