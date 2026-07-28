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
  durable queue/claim contracts, workflow persistence, transaction boundaries, or artifact-storage
  interfaces; workset-specific coordinator behavior may change only as defined below;
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

### WorkerWorkset and item templates

All production submissions use one logical `SubmitWorkset` operation. Its payload contains a finite,
static, ordered set of one to the negotiated maximum number of `WorksetItemTemplate`s. A one-item
workset is the ordinary single-job path; there is no second `SubmitInvocation` execution path.

The normative conceptual type catalog is:

| Type | Required meaning |
|---|---|
| `WorkerWorksetId` | Worker-protocol identity for one accepted transient workset |
| `WorkerWorksetItemId` plus stable item ordinal | Exact item identity and immutable order within that workset |
| `WorkerWorksetExecutionKey` | Exact module/revision/hash/entrypoint and dependency closure; runtime profile, game/disc/backend compatibility, and capability packs; exact source-state identity/hash/lineage and movie-continuation policy; common execution/input/capture/movie/mutation and other relevant service policies |
| `WorkerWorksetDefinition` | Immutable workset ID, execution key, limits, common preparation policy, and complete ordered item set |
| `WorksetItemTemplate` | Immutable item ID/ordinal, invocation/attempt/cancellation correlation, one typed input record, per-item budget, and provenance awaiting actor-owned session/epoch binding |
| `WorkerWorksetLimits` | Maximum item count, encoded bytes, aggregate declared child budgets, resident-item capacity, and unacknowledged terminal-result count and bytes |
| Typed state and receipts | Distinct workset/item state, exact item/workset cancellation, item-terminal acknowledgement, item terminal, workset terminal, and final drain receipts |

`WorkerWorksetDefinition` therefore fixes request correlation, one `WorkerWorksetExecutionKey`, one
`WorkerWorksetLimits`, the common state preparation, and every `WorksetItemTemplate` before acceptance.

Every item must match the workset's exact `WorkerWorksetExecutionKey`. Only typed input,
invocation/attempt/cancellation correlation, per-item budget, and child identity/provenance may differ;
no item may select another module, entrypoint, dependency closure, runtime, baseline, or
session-shaping policy. The complete workset is rejected before state
mutation if it is empty, unbounded, oversized, malformed, or mixed-key.

Membership and stable priority/claim order are fixed by the coordinator and immutable after acceptance.
The worker cannot reorder them. An item cannot append another item, choose the next item, consume a
previous item result, or make its execution conditional on a previous domain outcome.
Such relationships remain ordinary module control flow or durable workflow composition. The workset is
transient worker/transport state, not a `ProgramModule`, `ProgramInvocation`, workflow, persisted batch,
or durable retry record.

Before common state preparation, `ProgramRuntime` preflights the shared module/dependency/capability
closure and every item's input schema and static policy/budgets without constructing a
`ProgramInstance`. A multi-item workset captures one immutable reusable baseline after common
preparation; a one-item workset does not perform that unnecessary capture. Immediately before each
item starts, `WorkerRuntime` binds its template to the exact current `SessionId` and `StateEpoch`. The
first item uses the already-prepared state; every later item follows a successful restore of the
multi-item baseline and therefore binds a fresh epoch. That binding fills only actor-owned
session/epoch identity and cannot rewrite any immutable template field.

### ProgramInvocation

Every admitted workset item becomes one immutable logical invocation:

| Group | Required contents |
|---|---|
| Invocation identity | `invocation_id`, `attempt_id`, workset ID/item ordinal, optional workflow/step/job identities |
| Exact program | `module_id`, revision, `module_hash`, named `entrypoint` |
| Dependency lock | IR version and exact action/type dependency identities expected by the caller |
| Runtime profile | Game/runtime/disc compatibility, backend requirements, and required capability packs |
| State policy | Declared `Boot`, `LoadArtifact`, `RestoreBaseline`, or `ContinueSession` preparation provenance plus the exact prepared session/epoch guard |
| Execution policy | Live, replay, or visual-debug intent; movie/input/capture policy; cancellation and observation policy |
| Inputs | One typed record conforming exactly to the entrypoint input schema |
| Limits | Deadline and instruction/effect/emission/artifact budgets |
| Provenance | Requesting workflow or tool, source artifacts, route/model revisions, and caller correlation IDs |

The program-kind adapter derives every non-actor-owned field from existing SavorDb records and fixed
runtime/module configuration. It submits immutable workset item templates rather than guessing future
session/epoch values. `WorkerRuntime` supplies those two exact guards just in time; they and the workset
correlation are worker-facing envelope data, not new persisted SavorDb fields.

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

For workset admission, common preparation satisfies the declared state policy once. A multi-item
workset captures the reusable baseline; a one-item workset does not. The resolved child invocation
retains that preparation identity/provenance and an exact current-session/epoch guard;
`ProgramRuntime` does not repeat the common load for the first item. Later items start only after
`StateService` restores the same multi-item baseline and the actor binds the resulting epoch.

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
- exact workset/item correlation and the starting `SessionId`/`StateEpoch` bound at admission;
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

There is no aggregate domain-level batch result. Every item independently produces its ordinary
`ProgramResult`, and existing result adapters consume it independently. A final workset terminal only
summarizes transport completion, cancellation, and session disposition; it cannot replace, merge, or
reinterpret item outputs.

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

Within a workset, the first item starts at the epoch established by common preparation. Each successful
baseline restore before a later item advances the epoch exactly once. Future epochs are therefore never
precomputed in submitted item templates. No receipt, observation, acknowledgement, suppression state,
or guest-derived handle from one item can appear in another item's inputs; the resolved invocation and
result record the actual starting epoch.

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
4. unified `SubmitWorkset` submission for one to many items;
5. whole-workset acceptance or rejection before session mutation;
6. ordered item-start, correlated progress, and workset-state events;
7. exact pending/active item cancellation and exact workset cancellation;
8. immediate authoritative per-item terminal `ProgramResult` delivery;
9. exact durable acknowledgement of one item terminal;
10. final bookkeeping-only workset terminal/drain summary; and
11. explicit session disposition.

Per-item terminals are authoritative and non-lossy. The serialized publisher emits each result as soon
as invocation unwind finishes and `WorkerRuntime` retains it by count and bytes until the parent confirms
that exact workset ID, item ordinal, invocation ID, attempt ID, and terminal sequence was handled
durably. Duplicate delivery is permitted until acknowledgement and must resolve through the existing
idempotent result path; stale, mismatched, or duplicate acknowledgement cannot release another result.
The publisher enqueues an item's terminal before any start, progress, or other event for a later item.

The worker may continue while the retained window has capacity, but it keeps Dolphin paused and does not
restore/admit the next item when the window is full. Progress and optional diagnostics may coalesce or
drop under their declared policy; item terminals never may. The final workset summary is emitted only
after every item is terminal or classified unstarted, every required durable acknowledgement has been
correlated, and the workset scope has released cleanly.

`ProcessWorker` may expose a convenience API that accepts one invocation template, but it wraps that
template into a one-item workset and uses this same operation, correlation, result, and acknowledgement
path.

Concrete framing/version assignment remains an implementation-cutover concern. It must preserve this
single workset path and must not reintroduce separate built-in, authored, or single-item execution modes.
The former `SubmitInvocation` discriminator is reserved and receiving it rejects the frame before
session mutation; it is not renumbered or reused.

### Worker affinity

Runtime compatibility, session reuse, and module-cache locality are resolved without changing existing
SavorDb affinity, claim, or queue contracts. `ProgramKind` may remain current SavorDb routing or affinity
metadata; it cannot by itself authorize worker-session reuse or select a worker interpreter/controller.

`WorkerWorksetExecutionKey` is the runtime authorization for grouping items. Existing affinity may
identify candidates, but the worker rejects a workset unless every exact key component matches. Workset
reuse is therefore an optimization over one verified module/session/baseline, never evidence that
ambient guest state from the prior item is acceptable.

### Workflow adapters

The existing SavorDb program-kind handler remains the integration boundary. Its implementation or an
adjacent adapter may construct immutable workset item templates from existing job/domain data and
project each resolved `ProgramResult` through existing result writers and transition operations. This
refactor does not
require splitting or changing SavorDb descriptor, workflow, database-service, queue, claim, or
persistence interfaces. Workflow code does not decode worker-private program locals.

Workset identity, membership, ordering, baseline handle, terminal-retention state, and acknowledgements
are not persisted runtime records. Existing jobs remain individually claimed, leased, cancelled,
retried, completed, and transitioned. The parent sends a terminal acknowledgement only after its
existing result-projection/idempotency transaction has committed durable handling of that exact item.

Existing persisted battle predicate definitions are decoded through current SavorDb interfaces and
supplied to in-memory predicate composition before module verification. Completion adapters project
condition summaries, passed/total compatibility fields, and predicate-rejection outcomes through
existing result operations. Neither `ConditionObservation` nor the composition source requires a new
stored representation.

## Failure and cleanup behavior

- Verification failure produces `Rejected`; the entrypoint never runs.
- Deadline or cancellation requests suspend new effects, cancel the pending cancellable action, and
  unwind all scopes.
- Exact cancellation of a pending workset item produces a cancelled-before-start item terminal without
  state mutation. Exact cancellation of the active item follows ordinary invocation cancellation and
  may permit the next item only after clean unwind.
- Workset cancellation prevents all later admission, classifies pending items, and cancels the active
  item through the same ordinary unwind path.
- A clean domain failure or clean infrastructure failure may permit the next ordered item. Cleanup
  failure, uncertain state integrity, or session taint stops admission immediately.
- An action failure is typed and may be handled by program control flow only when its descriptor permits
  recovery.
- Failure to restore any mandatory resource produces cleanup status `Tainted`.
- A tainted session is retired or rebuilt before accepting another invocation.
- A tainted or uncertain item/workset cleanup stops the workset immediately; no pending item is admitted.
- Artifacts emitted before terminal failure remain incomplete unless their schema declares an independently
  atomic publication transaction.
- Transport loss does not create a second “unknown” program success. Existing SavorDb durable-attempt
  and idempotent-publication mechanisms continue to provide workflow recovery; runtime correlation stays
  in the worker envelope.

An acknowledged child result remains durable after worker loss. An unacknowledged terminal may also
have been persisted before transport loss, while a pending child may never have started. Recovery
therefore retries or replays each affected durable job/attempt under the existing lease and idempotency
rules; it never resumes the ephemeral workset as a durable batch and does not infer success from a
missing acknowledgement.

Artifact publication in the rules above is runtime-side. Existing SavorDb attempt, idempotency,
result-publication, and recovery mechanisms remain unchanged.

## Dependencies and migration implications

- Only worker-facing activation and result protocols change. Existing persisted job payloads and
  domain/result representations remain unchanged. Program-kind adapters decode existing records into
  typed runtime inputs and project typed runtime results back through existing operations.
- Each supported current phase uses a direct native typed-module builder. Existing persisted payload and
  result codecs remain at the adapter boundary and are decoded or projected in memory; no `PhaseScript`
  translator contributes module bytes, identity, or verification evidence.
- Predicate definitions supplied by existing adapters lower through the same module builder and are
  covered by the resulting canonical module identity; their persisted representation remains unchanged.
- Worker-side result mapping migrates from a single `PSContext` blob to declared outputs, emissions,
  artifacts, and provenance; the program-kind adapter then writes the existing SavorDb representation.
- State paths or references are adapted in memory into explicit runtime state policy without changing
  their persisted representation.
- Compatible independently claimed jobs may be submitted through one finite workset, but every existing
  result, lease, cancellation, retry, transition, and idempotency operation remains per job/attempt.
- Persisting DB-authored modules is outside this refactor. Any later frontend must use the identical
  activation/invocation path.

## Runtime contract checks

- A worker rejects a module with one mismatched imported action signature without being affected by an
  unrelated registry addition.
- Exact module hash, entrypoint, inputs, state artifact, dependency closure, and runtime profile are
  available in the runtime result envelope; this check does not require new SavorDb fields.
- A one-item and multi-item submission use the same `SubmitWorkset` path and produce the same per-item
  `ProgramInvocation`/`ProgramResult` contract.
- A mixed `WorkerWorksetExecutionKey`, dynamic item append/reorder, or dependency on a prior item result
  rejects before common state preparation.
- The first item uses prepared state without a second restore; every later admitted item records one
  fresh epoch created by restoring the workset baseline.
- A full terminal-retention window prevents another baseline restore/admission until an exact durable
  acknowledgement creates capacity; progress loss cannot discard a terminal result.
- Item cancellation, workset cancellation, duplicate terminal delivery, and stale acknowledgement each
  preserve exactly one authoritative item terminal.
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

- Concrete C++ struct and ownership spellings.
- Transport compression plus numeric workset/result-window thresholds; the non-lossy acknowledged
  per-item terminal contract is not deferred. Numeric values are negotiated configuration informed by
  measurement, not frozen architectural constants.
- Any SavorDb normalization or artifact-store-interface change is outside this refactor.
- Canonical binary encoding and hash algorithm.
- User-facing module/version selection UI.

## Source references

- `SavorCore/Runner/IPC/Wire.h:8-17,63-83,100-112,142-163`
- `SavorWorker/SavorWorker.cpp:355-472`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:103-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
