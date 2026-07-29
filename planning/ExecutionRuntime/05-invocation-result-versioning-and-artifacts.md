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
- changes to SavorDb SQL/schema, migrations, stored representations, durable job/claim/lease/result
  semantics, workflow persistence, result-projection transaction boundaries, or artifact-storage
  interfaces; document 06 may narrowly adapt execution-facing interfaces for ordered batch claim,
  exact-set lease renewal, claim/start validation, and targeted terminal reconciliation over the same
  records and semantics;
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
| `WorkerWorksetExecutionKey` | Exact module/revision/hash/entrypoint and dependency closure; runtime profile, game/disc/backend compatibility, and capability packs; exact `ProgramBaselineKey` covering source-state identity/hash/lineage, movie continuation, and declared derived state; common execution/input/capture/movie/mutation and other relevant service policies |
| `WorkerWorksetDefinition` | Immutable workset ID, execution key, limits, common preparation policy, and complete ordered item set |
| `WorksetItemTemplate` | Immutable item ID/ordinal, invocation/attempt/cancellation correlation, one typed input record, per-item budget, and provenance awaiting actor-owned session/epoch binding |
| `WorkerWorksetLimits` | Maximum item count, encoded bytes, aggregate declared child budgets, resident/staged item capacity, item-capacity credits, immutable-state/finalization bytes, and completion-ledger count and bytes |
| `ProgramBaselineDefinition` | Complete ordered reusable starting condition for a multi-item workset |
| `ProgramBaselineComponent` | One savestate, exact movie-continuation, or runtime-facing program-kind adapter-declared derived-state component |
| `ProgramBaselineKey` | Exact identity of the complete ordered component set and its compatibility |
| `PreparedProgramBaselineReceipt` | Atomic proof that every component was prepared for one exact session/epoch before admission |
| Staged successor correlation | Exact immutable package/request identity tying one accepted but non-mutating successor package to its jobs, claim/lease authority, cancellation, and eventual promotion or rejection |
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

One workset may own the session while at most one immutable successor package is staged. Staging may
decode framing, validate the complete definition and item schemas, pin exact verified module/dependency
objects, and acquire host-only immutable state-cache leases. It cannot boot, restore, capture a
baseline, acquire an invocation/session effect, publish `JobStarted`, or create a `ProgramInstance`.
Claim/start authority for every finite member is validated before `SubmitWorkset` and acceptance.
Promotion occurs after the active workset releases its session scope, the session and local credits are
clean, and no exact cancellation or lease-loss notice has arrived; it does not synchronously revalidate
with the coordinator. This is bounded double buffering, not dynamic refill.

Before common state preparation, `ProgramRuntime` preflights the shared module/dependency/capability
closure once and validates every item's input schema and static policy/budgets without constructing a
`ProgramInstance`. The resulting exact verified entrypoint/closure may be pinned by the active and
staged packages; it is not re-resolved or re-verified for each child. A multi-item workset acquires or
creates one composite `ProgramBaselineDefinition` after common preparation; a one-item workset does not
perform an unnecessary baseline capture. Its `ProgramBaselineKey` covers the ordered savestate, exact
movie continuation, and adapter-declared derived-state components. Immediately before each item starts,
`WorkerRuntime` binds its template to the exact current `SessionId` and `StateEpoch`. The first item uses
the already-prepared state; every later item follows one successful composite `RestoreBaseline`, receives
a `PreparedProgramBaselineReceipt`, and therefore binds a fresh epoch. That binding fills only actor-
owned session/epoch identity and cannot rewrite any immutable template field.

Every accepted item consumes one negotiated item-capacity credit until its exact terminal or unstarted
disposition has been durably acknowledged and all completion-ledger storage is released. Resident
pending, staged, active, asynchronously finalizing, ready-but-order-blocked, and unacknowledged-terminal
states are phases of the same credit rather than additional capacity. An item cannot be admitted unless
the worker has reserved the worst-case count/byte capacity declared by its limits.

The initial configurable limits are 16 items and 32 MiB encoded bytes per workset; four hours aggregate
declared active budget; 64 total worker item credits and 32 active-plus-staged items; 16 immutable state
cache entries/512 MiB; two finalizer threads with eight pending captures/256 MiB; and 32 retained
authoritative terminals/128 MiB. The coordinator may buffer at most one additional workset per
negotiated Ready worker, and at most two workers start concurrently.

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
- **RestoreBaseline** names one exact `ProgramBaselineDefinition`/`ProgramBaselineKey` and guarantees that
  the entrypoint starts only after the savestate, exact movie continuation, and every adapter-declared
  derived-state component are prepared together. Deliberate intra-invocation retry restores must request
  the same composite service explicitly and advance the epoch; they are not hidden VM behavior.
- **ContinueSession** uses the current session only when the invocation supplies the expected session
  lineage and `StateEpoch`. A mismatch rejects the invocation before user program logic runs.

An entrypoint may reject a state policy it does not declare. There is no implicit “latest savestate” or
ambient workflow result.

For workset admission, common preparation satisfies the declared state policy once. A multi-item
workset captures the reusable composite baseline; a one-item workset does not. The resolved child
invocation retains that preparation identity/provenance and an exact current-session/epoch guard;
`ProgramRuntime` does not repeat the common load for the first item. Later items start only after
`RestoreBaseline` returns one `PreparedProgramBaselineReceipt` for the complete component set and the
actor binds the resulting epoch. Each complete successful restore advances `StateEpoch` exactly once.

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

Execution completion is not necessarily terminal-envelope completion. If an invocation captured a
state artifact, its `ProgramInstance` may finish and unwind after the actor promotes the immutable
state/movie capture into the worker-global completion/acknowledgement ledger. The ledger retains the
item's typed output/emissions, cleanup/session disposition, exact correlations, reserved terminal
order, and pending artifact roles while bounded background finalization runs. Only the actor may
combine that state with finalization receipts to assemble the authoritative `ProgramResult`.

This asynchronous completion state is not another active invocation, program continuation, workflow
state, or partial terminal. `ProgramRuntime` still owns at most one `ProgramInstance`; a later child may
become that one instance after the previous root unwinds and capacity permits. If finalization fails,
the actor assembles one infrastructure-failed result with the ordinary independently recorded
domain/cleanup dimensions rather than publishing and later revising a successful result.

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

For a state artifact, publication follows document 04's split lifecycle: synchronous paused capture of
immutable state bytes and exact movie metadata, promotion to the worker-global
completion/acknowledgement ledger, bounded background hashing/sidecar/file publication and validation,
then actor-side terminal assembly. Before the final validation receipt exists, the internal
pending-publication receipt is neither an `ArtifactRef` nor a value that may enter a result, workflow
binding, database write, or later invocation.

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

`StateCacheKey` is the transient worker identity for reusable immutable state bytes. It includes:

- the exact state content hash and source artifact identity;
- game ID, ISO hash, emulator build, and required runtime compatibility;
- state lineage/fingerprint fields required by the declared policy; and
- exact `NoMovie` or read-only DTM identity/hash, starts-from-savestate fact, and known continuation
  evidence, plus any session-generation constraint required by that backend/runtime profile.

The cache key is not persisted and is not a substitute for `StateArtifact`, `StateEpoch`, session
identity, or a claim/affinity key. A scoped cache lease authorizes only access to those immutable bytes;
restore still runs the complete `StateService` transaction and advances the epoch. Cache hit, miss,
compression, storage tier, and eviction cannot change result semantics. In-progress recording handles,
live guest handles, resource receipts, and unvalidated output captures are never cache entries. An
asynchronously finalized output may enter the cache only after its final hash and compatibility
validation establish its complete key.

An incompatible reboot, runtime/backend replacement, movie-generation change, or integrity uncertainty
invalidates affected entries. A compatible replacement may retain only entries whose complete key and
session-generation constraint remain valid.

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
identity. A workset pins one exact verified entrypoint/closure during atomic preflight and reuses it for
each child; child admission repeats input, budget, policy, session, epoch, and cancellation validation,
not canonical module verification. Adapter-generated composition variants and immutable parsed service
definitions may likewise be cached only by their complete canonical content/dependency identity. Cache
hits never weaken invocation verification or carry mutable capture, observation, input, or resource
state.

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
2. catalog negotiation as `Partial` or `CompleteExact`, plus negotiated item/count/byte capacity credits
   and later credit-state updates;
3. module transfer or cache lookup by hash;
4. module verification and activation acknowledgement;
5. unified `SubmitWorkset` submission for one to many items, including exact active-or-staged package
   correlation;
6. whole-workset acceptance, staged acceptance, promotion, or rejection before session mutation;
7. ordered item-start, correlated progress, asynchronous-finalization, and workset-state events;
8. exact pending/active item cancellation and exact workset cancellation;
9. authoritative per-item terminal `ProgramResult` delivery in actor-assigned completion order;
10. exact durable acknowledgement of one item terminal and resulting credit release;
11. final bookkeeping-only workset terminal/drain summary; and
12. explicit session disposition and transient `StateCacheKey` availability hints.

Per-item terminals are authoritative and non-lossy. The serialized publisher emits each result as soon
as its synchronous execution/unwind plus every mandatory asynchronous artifact finalization is complete
and the actor can release its terminal-order position. `WorkerRuntime` retains it in one
worker-global completion/acknowledgement ledger until the parent confirms that exact workset ID, item
ordinal, invocation ID, attempt ID, and terminal identity was handled durably. Duplicate delivery is
permitted until acknowledgement and must resolve through the existing idempotent result path; stale,
mismatched, or duplicate acknowledgement cannot release another result or capacity credit.

When synchronous item execution completes or an unstarted item receives its final disposition, the
actor assigns one monotonic terminal-order ordinal across worksets. State finalizers may complete out
of order, but authoritative terminals cannot overtake an earlier ordinal. This ordinal is separate from
the serialized publisher's outbound sequence: because immutable finalization no longer owns the
session, a later item's start/progress may be published while an earlier item is still finalizing, and
each published event receives the next outbound sequence plus exact workset/item/invocation
correlation. No later terminal can become authoritative first.

The worker may continue while item and completion-ledger count/byte credits have been reserved. When
they are exhausted, it keeps Dolphin paused and does not restore/admit the next item or promote the
staged successor package. Progress and optional diagnostics may coalesce or drop under their declared
policy; completion entries and item terminals never may. The executed workset releases its
session/baseline scope as soon as all children are terminal-preparing or classified unstarted and the
scope is clean; prior finalizations and acknowledgements can then drain globally while one staged
successor is promoted. Its bookkeeping summary is emitted only after every item terminal/disposition is
acknowledged, but that summary does not retain session ownership.

`ProcessWorker` may expose a convenience API that accepts one invocation template, but it wraps that
template into a one-item workset and uses this same operation, correlation, result, and acknowledgement
path.

These operations extend WRMS version 1 additively; the protocol version remains 1. They preserve this
single workset path and do not reintroduce separate built-in, authored, or single-item execution modes.
The former `SubmitInvocation` discriminator and the removed guest-instruction-step discriminator remain
reserved. Receiving either rejects the frame before session mutation; neither is renumbered or reused.

The pre-6A process seam reports `Partial` while it transfers and executes one canonical test-only module
through the ordinary preparation protocol. That module is never one of the nine production modules.
Slice 7 permits DB activation only for `CompleteExact`: exactly the nine planned production module
IDs/hashes, their exact dependency manifest, and no extra installed module.

### Worker affinity

Runtime compatibility, session reuse, and module-cache locality are resolved without changing existing
SavorDb affinity, claim, or queue contracts. `ProgramKind` may remain current SavorDb routing or affinity
metadata; it cannot by itself authorize worker-session reuse or select a worker interpreter/controller.

`WorkerWorksetExecutionKey` is the runtime authorization for grouping items. Existing affinity may
identify candidates, but the worker rejects a workset unless every exact key component matches. Workset
reuse is therefore an optimization over one verified module/session/baseline, never evidence that
ambient guest state from the prior item is acceptable.

A worker may report transient exact `StateCacheKey` presence and available negotiated credits so the
coordinator can prefer a compatible warm destination after honoring durable priority/fairness. That
report is a hint only. Submission still names the authoritative immutable artifact/state policy, and
the worker reacquires a scoped cache lease or falls back to normal preparation. A durable successor may
use `ContinueSession` only after its predecessor's existing result/transition transaction commits and
only when exact session, current epoch, state lineage/hash, and clean disposition still match; otherwise
it uses the ordinary artifact restore path.

### Workflow adapters

The existing SavorDb program-kind handler remains the integration boundary. Its implementation or an
adjacent adapter may construct immutable workset item templates from existing job/domain data and
project each resolved `ProgramResult` through existing result writers and transition operations. This
refactor does not change descriptors, stored jobs/claims/leases/results, workflow topology, result
projection, or persistence semantics. Document 06 may add or adapt only narrow execution-facing
operations for real bounded batch claim/lease, item-capacity accounting, and targeted advancement of an
exact terminal step using the existing records and commands. Workflow code does not decode
worker-private program locals.

Workset identity, membership, ordering, baseline handle, terminal-retention state, and acknowledgements
are not persisted runtime records. Existing jobs remain individually claimed, leased, cancelled,
retried, completed, and transitioned. The parent sends a terminal acknowledgement only after its
existing result-projection/idempotency transaction has committed durable handling of that exact item.
It then publishes the affected workflow-step identity and commit sequence to the targeted advancement
path. Targeted advancement does not delay acknowledgement; the existing broad reconciliation scan
remains the recovery guard if the in-process notification is missed.

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
- A promoted immutable state capture may finish finalizing after its producing invocation/workset scope
  releases. Publication/validation failure assembles one infrastructure-failed terminal at its reserved
  sequence; it cannot revise an earlier success because no authoritative terminal existed.
- An exact claim/lease-loss, supersession, or user-cancellation notice for an accepted active or staged
  package cancels the affected item/workset asynchronously. In the absence of such a notice, accepted
  authority persists through staged promotion and ordered child admission without a coordinator
  roundtrip. Loss discovered after an item starts follows ordinary exact-invocation cancellation plus
  the existing attempt/lease and idempotent-terminal recovery rules.
- Artifacts emitted before terminal failure remain incomplete unless their schema declares an independently
  atomic publication transaction.
- Transport loss does not create a second “unknown” program success. Existing SavorDb durable-attempt
  and idempotent-publication mechanisms continue to provide workflow recovery; runtime correlation stays
  in the worker envelope.

An acknowledged child result remains durable after worker loss. An unacknowledged terminal may also
have been persisted before transport loss, a promoted artifact may not yet have published an
authoritative terminal, and a pending or staged child may never have started. Recovery therefore retries
or replays each affected durable job/attempt under the existing lease and idempotency rules; it never
resumes the ephemeral workset/completion ledger as durable state and does not infer success from a
missing acknowledgement or temporary file.

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
  their persisted representation. Exact `StateCacheKey` hints and scoped leases remain transient.
- Compatible independently claimed jobs may be submitted through one finite workset, but every existing
  result, lease, cancellation, retry, transition, and idempotency operation remains per job/attempt.
- State-artifact finalization may overlap a later sole active invocation only after immutable paused
  capture is promoted into the bounded worker-global completion ledger. Result projection still begins
  only from the one final authoritative `ProgramResult`.
- Narrow workset-specific execution interfaces may expose ordered batch claim, exact-set lease renewal,
  claim/start validation, and targeted terminal reconciliation as described in document 06. They
  operate the same rows, lifecycle, idempotency, and per-item semantics and add no persisted runtime
  record.
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
  fresh epoch and one `PreparedProgramBaselineReceipt` created by restoring the complete savestate,
  movie-continuation, and adapter-declared derived-state baseline.
- Repeated worksets over the same exact state produce identical results from a cache hit or artifact
  load; each restore still creates a fresh epoch and cache eviction is unobservable.
- One immutable staged successor can be preflighted without session mutation and can be promoted only
  after active-scope release, clean local capacity, and absence of an exact cancellation notice; it does
  not wait for synchronous coordinator reauthorization.
- A whole workset's claim/start authority is checked before acceptance. No per-item authorization call
  occurs between clean children, while later lease-loss/supersession notices still cancel the exact
  pending or active item.
- A full item/completion-credit window prevents another baseline restore/admission or staged promotion
  until an exact durable acknowledgement creates capacity; progress loss cannot discard a completion
  entry or terminal result.
- Out-of-order state-finalizer completions publish authoritative terminals only in actor-assigned
  completion order, while correlated later item progress cannot be mistaken for an earlier result.
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
- Transport compression and measurement-driven tuning beyond the fixed configurable defaults: 16
  items/32 MiB/four aggregate hours per workset, 64 total and 32 active-plus-staged item credits, 16
  cache entries/512 MiB, two finalizer threads with eight pending captures/256 MiB, 32 retained
  terminals/128 MiB, two concurrent startups, and one coordinator-buffered successor per Ready worker.
  The non-lossy acknowledged per-item terminal contract is not deferred.
- Any SavorDb normalization or artifact-store-interface change is outside this refactor.
- A future incompatible protocol version only if required; WRMS v1 and the reserved
  `SubmitInvocation`/guest-step discriminators remain fixed.
- User-facing module/version selection UI.

## Source references

- `SavorCore/Runner/IPC/Wire.h:8-17,63-83,100-112,142-163`
- `SavorWorker/SavorWorker.cpp:355-472`
- `SavorCore/Phases/Programs/ProgramRegistry.cpp:44-109`
- `SavorDb/Execution/ProgramDB/ProgramKindDescriptor.h:103-185`
- `planning/DBMigrateWorkflows/12-user-defined-script-payload-system-plan.md`
