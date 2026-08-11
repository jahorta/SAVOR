# Semantic Routing, Workset Capture, and Canonical Progress

Status: normative hard-cut contract, implemented 2026-08-09.

This document supersedes every earlier use of `SGC1`, phase-local stop-group
encoding, generic stop delivery policies, phase-owned capture attachment, live
capture-profile replacement, free-form invocation progress, and
`Execution.JobProgressed.v1` in the Execution Runtime planning package.

## Three separate concepts

The runtime treats these as independent facilities:

1. `SemanticPointSetV1` describes the exact registered points at which one
   operation may stop. Its one canonical wire representation is `SPS1`.
2. Passive observation receives routed-event evidence without acquiring any
   authority over guest execution.
3. A foreground wait is installed and owned directly by the active
   `ExecutionEngine` operation. It is not promoted from a passive group.

`SemanticPointSetV1` contains ordered semantic-point identities and optional
registered hit-time CPU samplers. It contains no lifetime, routing, delivery,
capture, or progress policy. The shared decoder resolves every point and
sampler against the static capability catalog during module verification, so a
malformed set is rejected during workset admission before guest mutation.

The stop router exposes three structurally different registrations:

- passive observation;
- foreground wait; and
- an explicit trusted interruption request.

There are no generic `Pass`, `Consume`, `Fail`, `Guard`, `Intercept`, or
`Progress` delivery modes and no `Consumed` route terminal. Passive-only hits
are delivered and resumed. A foreground match stops its owning operation. A
trusted interruption point requests its registered handler. Interruption
trigger ownership and foreground-wait ownership cannot overlap, while passive
capture may observe either. Every recipient sees the same routed identity,
snapshot, epoch, and sequence.

When an interruption temporarily parks a parent `ContinueUntil`, the parent's
foreground registration is removed. It is reconstructed when the parent
resumes; it does not remain as an implicit passive observer.

## Immutable workset capture

`WorkerWorksetDefinition` owns at most one `WorksetCaptureBindingV1`. The
binding is not Full Phase common input and is not item input. It names:

- the exact `savor.capture.profile/1` bytes, inline or in a content-addressed
  sidecar;
- the normalized profile hash and expected worker-module hash;
- the resolved observation hash joining that profile to the exact progress
  plan; and
- the per-workset capture output directory.

Coordination resolves authoring or database references before dispatch. The
worker never reads `SavorDb`. Host-only admission bounds and hashes the bytes,
parses the profile, verifies its module and formatter identities, merges any
requested registered breakpoint progress providers deterministically, and
prepares passive PC/watchpoint routing before initialization can mutate the
guest.

The profile itself is reused across all items in a workset. Every item receives
a new capture session and new output. `WorkerRuntime` finalizes that session
during item unwind and attaches its artifacts and diagnostics to that item's
terminal envelope. Incomplete optional evidence and queue drops remain
diagnostics; cleanup uncertainty follows the normal failure/taint rules.

Programs do not attach or finalize capture and cannot replace profiles or
externally enable groups in any worker mode. The only program-visible capture
action is a handle-free marker for deterministic profile-declared marker and
window behavior. Profile `Control` subscriptions remain local observation
signals: when the same hit also satisfies a foreground wait they may update
capture windows or flight recorders, but cannot wake or claim execution.

Raw capture PCs and memory watchpoints remain capture-profile facilities. They
are not rewritten as semantic points.

## Canonical progress

Progress is a requested, curated observation product, not generic telemetry.
Every homogeneous worker and the coordinator share one static
`ProgressLibraryRegistry`. Library, point, provider, typed-schema, and
formatter identities contribute to the static runtime ABI.

The initial libraries are:

- `soa.progress.runtime.vi/1`;
- `soa.progress.soa.script_location/1`;
- `soa.progress.battle.events/1`; and
- `soa.progress.predicate.evaluations/1`.

The Battle event provider preserves the researched legacy observations and
formatting for attack damage, counterattack, death, item drop, and the complete
twelve-slot instruction/turn state. Its default probes carry the exact GPR,
memory, and bounded address-program samples required by their typed schemas.
An expert-authored `Progress` probe must reference a registered requested
formatter and supply all fields required by that formatter.

`ProgressPlanV1` is fully resolved before dispatch. It contains exact library
and point revisions, provider kinds and configuration, formatter and schema
identities, runtime-sample trigger PCs, and a canonical content hash.
`ProgramKindDescriptor` declares defaults, which planning includes unless it
explicitly disables a library or point. The current defaults are:

| Program kind | Default progress |
|---|---|
| `battle.single_turn` | Battle events and predicate evaluations |
| `tasmovie.validation` | VI and script location |
| `battle.context` | none |
| SeedProbe | none |
| checkpoint sterilization | none |

All future program kinds must declare their default set explicitly. Every TAS
Movie Full Phase workset remains a singleton regardless of its observation
plan.

The three provider forms share the same result contract:

- breakpoint-backed providers are lowered into passive capture;
- runtime-sampled providers are sampled at exact admitted semantic stops; and
- phase-library providers publish registered semantic products such as a
  predicate evaluation at a public hook.

Workers publish `CanonicalProgressEventV1`, containing workset/item/job,
invocation/attempt, a monotonic ordinal, library and point identity, optional
routed provenance, an exact typed schema and payload, and stable worker-rendered
text. Diagnostics, health events, capture incompleteness, and telemetry cannot
enter this stream. Runtime completion notification is likewise a distinct
lifecycle-only event; progress is never used to wake the worker actor.

`JobExecutionCoordinator` validates correlation and persists progress
idempotently by `(job_id, attempt_id, ordinal)`. Exact replay is accepted and a
conflicting replay is rejected. `exec_job_progress` retains typed evidence and
display text; `Execution.JobProgressed.v2` drives the UI projection. The UI
read model preserves the structured rows and also exposes the latest display
text on running jobs. Workflow archive/rehydration includes the structured
progress stream and remaps its job and dispatch/workset identities.

## Failure and lifecycle rules

- Capture/profile/progress hash, schema, module, sampler, point, or formatter
  failure rejects admission before guest mutation.
- A passive observer can neither stop execution nor become a foreground wait.
- Foreground registrations belong to one operation and are released with it.
- Coherent current-state queries require the active workset epoch and an
  authoritative backend-confirmed paused snapshot at the expected PC. They do
  not require a routed stop sequence: an exact restored pause is valid without
  inventing a routed event. Routed sequences remain event provenance for
  routing, capture, progress, and diagnostics.
- Requested progress is persisted; telemetry is never promoted to progress.
- Each capture session is finalized exactly once during item unwind.
- Physical-integrity uncertainty still fails or taints through the canonical
  session lifecycle.
- Workset observation bindings and plans are immutable after admission.

## Canonical enum and definition identity

Runtime enums that select actions, reducers, semantic-point kinds, or typed IR
values never obtain meaning from a vector position. Canonical action and
reducer definitions carry their enum key explicitly and are resolved by that
key or by exact dependency identity. `SemanticPointKind` is one shared model
enum used by both references and registered descriptors. When transport and
runtime layers intentionally retain distinct enums, conversion remains an
exhaustive explicit mapping rather than a numeric cast.

Domain enums exposed as closed IR schemas own a local value/name map beside the
domain enum. That map supplies both schema members and exact numeric decoding;
unknown values fail instead of being range-checked and cast. Collection order
is deterministic presentation only and never execution authority.

## Primary implementation evidence

- `SavorCore/Runner/Runtime/ProgramRuntime/Composition/SemanticObservationComposition.*`
- `SavorCore/Runner/Runtime/StopPoints/StopPointRouter.*`
- `SavorCore/Runner/Runtime/Execution/ExecutionEngine.*`
- `SavorCore/Runner/Runtime/Worksets/WorksetTypes.*`
- `SavorCore/Runner/Runtime/Worksets/WorksetStager.*`
- `SavorCore/Runner/Runtime/Progress/ProgressTypes.*`
- `SavorCore/Runner/Runtime/WorkerRuntime.*`
- `SavorWorkflow/Execution/JobExecutionCoordinator.*`
- `SavorDb/migration/Execution/202608091000_execution_structured_job_progress.sql`
- `SavorDb/migration/UIRead/202608091000_uiread_structured_job_progress.sql`
- `SavorDb/Archive/ArchivePackageService.cpp`
- `SavorDb/Archive/RehydrateExecutor.cpp`
- `SavorQt/GUI/Tabs/RunningTab.cpp`
