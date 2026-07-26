# Invocation, Result, Versioning, and Artifacts

## Scope

This document defines the logical invocation, result, version, and artifact contracts used by the target
Execution Runtime. Implementation slices may choose concrete C++ types and worker transport encoding,
but must preserve these concepts and their separate meanings. SavorDb storage and normalization are
fixed inputs, not implementation choices in this refactor.

## Purpose and non-goals

The contract must support built-in programs, future authored programs, exact replay, phase switching,
multi-artifact results, bounded expansions that a future state-based frontier could orchestrate, and
worker caching without using `ProgramKind` as execution identity.

This document does not define:

- a packed wire struct;
- changes to SavorDb SQL/schema, migrations, stored representations, database-service interfaces,
  queues, claims, workflow persistence, transaction boundaries, or artifact-storage interfaces;
- the authored program source format;
- an object-store implementation; or
- domain-specific output schemas.

## Current code evidence

The current transport and worker path are narrower than the target:

- `SavorCore/Runner/IPC/Wire.h:63-83` defines wire-stable `PK_*` values.
- `WireSetProgram` at `Wire.h:142-149` sends `init_kind`, `main_kind`, buffer kind, timeout, and one
  savestate path. It does not identify a program revision, hash, entrypoint, or dependency closure.
- `WireResult` at `Wire.h:100-112` separates a transport `ok` bit and error byte but returns exactly one
  trailing context blob.
- `SavorWorker/SavorWorker.cpp:366-405` reconstructs today's compiled program from `main_kind`.
- `SavorWorker.cpp:438-472` selects a program-specific payload decoder, builds `PSJob`, and executes the
  active VM.
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109` contains the construction and payload-decoding
  switches.

These are current facts, not contracts to preserve.

## Core runtime contracts

### ProgramModule identity

An executable module is immutable. Its identity contains:

| Field | Required meaning |
|---|---|
| `module_id` | Stable semantic family identifier, namespaced independently of UI labels |
| `revision` | Immutable authoring/build revision meaningful within the module family |
| `module_hash` | Canonical content hash of executable IR plus declared metadata |
| `ir_version` | Version of the canonical typed IR |
| `entrypoints` | Named entrypoints with input, output, emission, artifact, and outcome schemas |
| `action_imports` | Exact action IDs, versions, and signature hashes |
| `type_imports` | Exact type/schema IDs, versions, and hashes |
| `required_capabilities` | Capability/effect requirements checked before activation |
| `accepted_policies` | Runtime, state, movie, debug, and attempt policies the module accepts |
| `budgets` | Maximum instruction, emission, artifact, recursion/call, and effect budgets |
| `source_map` | Optional but versioned mapping from IR locations to builder/authored source |

The canonical hash excludes deployment location and cache metadata. Two modules with the same hash must
have the same executable definition and declared dependency set.

When predicate composition generates module content, the lowered IR, imports, schemas, and emissions are
ordinary hashed module content. Source maps should retain the predicate definition and `Check` use site
for diagnostics, but those source identities do not create a second runtime dependency mechanism.

### ProgramInvocation

Every attempt receives one immutable logical invocation:

| Group | Required contents |
|---|---|
| Invocation identity | `invocation_id`, `attempt_id`, optional workflow/step/job identities |
| Exact program | `module_id`, revision, `module_hash`, named `entrypoint` |
| Dependency lock | IR version and exact action/type dependency identities expected by the caller |
| Runtime profile | Game/runtime/disc compatibility, backend requirements, and required capability packs |
| State policy | One of `Boot`, `LoadArtifact`, `RestoreBaseline`, or `ContinueSession`, plus required state reference and guards |
| Execution policy | Live, replay, or visual-debug intent; movie/input/capture policy; cancellation and observation policy |
| Inputs | One typed record conforming exactly to the entrypoint input schema |
| Limits | Deadline and instruction/effect/emission/artifact budgets |
| Provenance | Requesting workflow or tool, source artifacts, route/model revisions, and caller correlation IDs |

These fields belong to the worker-facing runtime envelope. A program-kind adapter derives them from
existing SavorDb records and fixed runtime/module configuration; they are not new persisted SavorDb
fields.

State policies have fixed semantics:

- **Boot** creates or rebuilds the session from the named runtime profile. It accepts no implicit prior
  emulation state.
- **LoadArtifact** restores one explicitly named immutable `StateArtifact` before the entrypoint begins.
- **RestoreBaseline** names one immutable baseline artifact and guarantees that the entrypoint starts from
  it. Deliberate intra-invocation retry restores must request the same service explicitly and advance the
  epoch; they are not hidden VM behavior.
- **ContinueSession** uses the current session only when the invocation supplies the expected session
  lineage and `StateEpoch`. A mismatch rejects the invocation before user program logic runs.

An entrypoint may reject a state policy it does not declare. There is no implicit “latest savestate” or
ambient workflow result.

### ProgramInstance

`ProgramInstance` is worker-local mutable execution state. It contains:

- verified module and entrypoint references;
- instruction pointer, call stack, and typed local scopes;
- immutable invocation inputs and mutable declared locals;
- pending action/effect continuation;
- structured resource/defer stack;
- current `StateEpoch` and epoch-bound opaque handles;
- emitted-record and artifact builders;
- accumulated diagnostics and trace correlation; and
- terminal result state.

It does not contain a phase-specific controller virtual table, worker queue, thread, nested event loop, or
direct Dolphin handle.

### ProgramResult

One result envelope separates three independent status dimensions:

| Dimension | Required states and rule |
|---|---|
| Infrastructure status | At least `Completed`, `Rejected`, `Cancelled`, `TimedOut`, and `BackendFailed` |
| Domain outcome | Entry-point-defined typed outcome; never inferred from a global context key |
| Cleanup/session status | At least `Clean`, `CleanWithDiagnostics`, and `Tainted` |

The envelope also contains:

- the exact invocation and resolved dependency identities;
- one declared typed output record when the entrypoint contract permits it;
- zero-to-many typed emitted-record batches;
- zero-to-many immutable artifact references;
- structured diagnostics with severity, source location, and causal chain;
- execution/action/branch trace references;
- cleanup receipts and session disposition; and
- provenance linking all outputs to source inputs and state lineage.

`Completed` does not imply a successful domain outcome. A locked door, no collision anomaly, or a
search-node dead end can be a successfully executed program with a non-success domain classification.
Likewise, a desirable domain outcome cannot make a tainted session reusable.

Partial records and artifacts are marked incomplete. A downstream binding may consume them only if its
declared input schema explicitly accepts incomplete material.

### Condition observations

Reusable predicates are authored through the composition library in document 03 and lower before
activation into canonical IR, exact action imports, scoped router operations, and declared emissions.
They do not add another result-status dimension or a predicate-specific runtime channel.

`ConditionObservation` is an ordinary typed emitted record. Its declared schema identifies the predicate
and evaluation sequence, records the current `StateEpoch`, carries the typed witness values needed by
that condition, and reports `Satisfied`, `Unsatisfied`, `NotApplicable`, or `Unavailable`.
`Unavailable` is not equivalent to `Unsatisfied`: failure to acquire required evidence follows the
action/infrastructure-failure contract unless the check explicitly defines absence as a domain
condition.

Emission and reaction are use-site policies. The same pure predicate may be used to branch, return a
clean domain rejection, explicitly fail, record progress, or accumulate a domain result. The predicate
definition itself performs no effects and does not decide the program outcome.

### ArtifactRef and immutable artifacts

Every artifact reference contains:

- artifact identity and content hash;
- schema ID, version, and hash;
- declared role;
- immutable storage locator;
- producer invocation, module, entrypoint, and attempt;
- source artifact identities;
- completeness and validation state; and
- runtime/disc/model compatibility where applicable.

Artifacts are immutable after publication. Correction creates another artifact with explicit derivation
lineage; it never replaces an earlier object in place.

This is a runtime artifact contract. It neither prescribes nor changes SavorDb artifact tables,
references, storage locators, or domain representations; program-kind adapters project it through the
existing artifact and result operations.

### StateArtifact and StateEpoch

A `StateArtifact` is an immutable artifact with additional state semantics:

- savestate content hash and storage reference;
- emulator/runtime/disc compatibility;
- parent state and transition-edge lineage;
- producer invocation and state-save observation;
- optional game-state fingerprint used for deduplication; and
- validation/completeness state.

`StateEpoch` is a worker-local monotonic identity for the currently loaded emulation state. Boot, reboot,
or savestate restore creates a new epoch. Memory-backed pointers, worksheet handles, selected-object
handles, ground-selector handles, router state tied to guest execution, and similar opaque handles carry
the epoch in which they were acquired. The executor rejects use after an epoch change.

An artifact ID is not an epoch, and an epoch is not a durable artifact ID.

### Version and dependency verification

Activation verifies the module hash, IR version, every imported action signature, every imported type
schema, required capability packs, runtime compatibility, and declared budgets before constructing a
`ProgramInstance`.

Compatibility is dependency-scoped. Adding an unrelated action or schema to a worker must not invalidate
an existing module. A single global registry hash is insufficient as the permanent compatibility model.

Workers cache verified modules by canonical hash and cache resolved dependency closures by their combined
identity. Cache hits never weaken invocation verification.

### Exact replay identity

An exact replay request fixes:

- program module hash and entrypoint;
- action/type dependency closure;
- typed invocation inputs;
- source state and other artifact hashes;
- runtime/disc/backend compatibility;
- execution and observation policies; and
- relevant model or route versions.

Matching these inputs permits comparison of action and branch traces. It does not promise identical host
wall-clock durations. Any tolerated backend nondeterminism must be declared by the affected actions and
reported in provenance.

The canonical lowered module is the replay authority for predicate composition. Stable predicate/check
identities remain in source maps, traces, and declared condition observations so a replay can explain
which evidence and decision produced a branch or emission.

## Interfaces and ownership affected

### Worker protocol

The target protocol needs logical operations for:

1. capability and runtime-profile negotiation;
2. module transfer or cache lookup by hash;
3. module verification and activation acknowledgement;
4. invocation submission;
5. progress/diagnostic and optional streamed record events;
6. cancellation;
7. terminal result delivery; and
8. explicit session disposition.

The concrete message framing is deferred. It must not reintroduce separate built-in and authored
execution modes.

### Worker affinity

Runtime compatibility, session reuse, and module-cache locality are resolved without changing existing
SavorDb affinity, claim, or queue contracts. `ProgramKind` may remain current SavorDb routing or affinity
metadata; it cannot by itself authorize worker-session reuse or select a worker interpreter/controller.

### Workflow adapters

The existing SavorDb program-kind handler remains the integration boundary. Its implementation or an
adjacent adapter may construct `ProgramInvocation` from existing job/domain data and project
`ProgramResult` through existing result writers and transition operations. This refactor does not
require splitting or changing SavorDb descriptor, workflow, database-service, queue, claim, or
persistence interfaces. Workflow code does not decode worker-private program locals.

Existing persisted battle predicate definitions are decoded through current SavorDb interfaces and
supplied to in-memory predicate composition before module verification. Completion adapters project
condition summaries, passed/total compatibility fields, and predicate-rejection outcomes through
existing result operations. Neither `ConditionObservation` nor the composition source requires a new
stored representation.

## Failure and cleanup behavior

- Verification failure produces `Rejected`; the entrypoint never runs.
- Deadline or cancellation requests suspend new effects, cancel the pending cancellable action, and
  unwind all scopes.
- An action failure is typed and may be handled by program control flow only when its descriptor permits
  recovery.
- Failure to restore any mandatory resource produces cleanup status `Tainted`.
- A tainted session is retired or rebuilt before accepting another invocation.
- Artifacts emitted before terminal failure remain incomplete unless their schema declares an independently
  atomic publication transaction.
- Transport loss does not create a second “unknown” program success. Existing SavorDb durable-attempt
  and idempotent-publication mechanisms continue to provide workflow recovery; runtime correlation stays
  in the worker envelope.

Artifact publication in the rules above is runtime-side. Existing SavorDb attempt, idempotency,
result-publication, and recovery mechanisms remain unchanged.

## Dependencies and migration implications

- Only worker-facing activation and result protocols change. Existing persisted job payloads and
  domain/result representations remain unchanged. Program-kind adapters decode existing records into
  typed runtime inputs and project typed runtime results back through existing operations.
- Current builders may compile through a temporary legacy translator, but the resulting module receives
  the same identity and verification treatment as every other module.
- Predicate definitions supplied by existing adapters lower through the same module builder and are
  covered by the resulting canonical module identity; their persisted representation remains unchanged.
- Worker-side result mapping migrates from a single `PSContext` blob to declared outputs, emissions,
  artifacts, and provenance; the program-kind adapter then writes the existing SavorDb representation.
- State paths or references are adapted in memory into explicit runtime state policy without changing
  their persisted representation.
- Persisting DB-authored modules is outside this refactor. Any later frontend must use the identical
  activation/invocation path.

## Runtime contract checks

- A worker rejects a module with one mismatched imported action signature without being affected by an
  unrelated registry addition.
- Exact module hash, entrypoint, inputs, state artifact, dependency closure, and runtime profile are
  available in the runtime result envelope; this check does not require new SavorDb fields.
- A result can represent infrastructure completion, a locked-domain outcome, and clean cleanup
  simultaneously.
- A cleanup failure produces a tainted session even when the domain objective was achieved.
- One invocation can emit many observations and artifacts without placing them in one opaque context blob.
- Condition observations are deterministic typed emissions, and unavailable required evidence cannot
  silently become an unsatisfied predicate.
- Restoring state invalidates an epoch-bound handle and the executor prevents its later use.
- Built-in and authored programs have one module verification and invocation path.
- `ProgramKind` is absent from executor dispatch and exact replay identity.

## Deferred work

- Concrete C++ structs and ownership types.
- Transport framing, compression, and streaming thresholds.
- Any SavorDb normalization or artifact-store-interface change is outside this refactor.
- Canonical binary encoding and hash algorithm.
- User-facing module/version selection UI.

## Source references

- `SavorCore/Runner/IPC/Wire.h:8-17,63-83,100-112,142-163`
- `SavorWorker/SavorWorker.cpp:355-472`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:103-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
