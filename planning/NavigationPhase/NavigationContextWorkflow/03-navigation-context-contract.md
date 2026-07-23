# 03 - Navigation Context Contract

## Status

Future plan. `NavigationContextResult` is the runtime evidence package that makes a prediction start
admissible. It follows the existing Battle Context pattern conceptually, but no Navigation capture program,
codec, database row, or workflow integration is currently implemented.

## Contract Purpose

A navigation context answers four questions:

1. Which exact clean runtime state and transition produced this field entry?
2. Is the actor ready to accept authored navigation input?
3. Which runtime fields must the predictor use as its initial temporal/encounter state?
4. Which content/world artifacts are compatible with that state?

It is not a generic snapshot of every possible field variable and is not a controller calibration record.

## NavigationContextResult

The SAVOR-owned result contains these logical groups.

### Identity and lineage

- immutable context result ID and schema version;
- `navigation_epoch_id`;
- source clean savestate ID and content hash;
- exact ready output savestate ID and content hash. This is the authoritative clean bootstrap for a later
  Navmesh Survey and may equal the source only when capture produced no derived clean state;
- producing workflow/step/job and worker/runtime build identity;
- `DiscImageIdentity` and area key/profile references;
- field script/context identity and active MLD/SCT/ECT identities when observable;
- reset transition kind and provenance; and
- capture VI frame/timestamp.

### Spatial start

- active actor/object identity, without universally renaming it "player" where evidence is weaker;
- world-space position and yaw/facing when known;
- active collision ground/resource selector and resolved `NavigationTriangleKey`/anchor when available;
- placement-settled evidence and anchor diagnostics; and
- coordinate-policy version.

### Readiness evidence

- player/control-enabled state;
- measured velocity vector and zero-velocity test result;
- measured/observed authored input state and queued-input state;
- forced movement, cutscene, transition, and encounter-pending indicators;
- stabilization window and individual observation frames; and
- readiness status plus diagnostics.

### Temporal and encounter state

- measured `stepCount` and its address/schema provenance;
- movement accumulator and modifier fields when required by the selected encounter model;
- active encounter table/set identifiers and pending encounter state when available;
- exact RNG state material required by the chosen model bundle;
- other independently captured reset-sensitive fields; and
- per-field confidence, availability, and capture-source metadata.

The field set is versioned by a `navigation_context_capture_contract_version`. A model may require a
strict superset; the predictor rejects an insufficient context rather than synthesizing values.

Every `Ready` result has one explicit output savestate reference satisfying the same measured readiness
evidence. A missing or ambiguous output savestate makes the result incomplete for Navmesh Survey
bootstrap; downstream survey jobs never choose an implicit latest savestate.

### Readiness and completeness

- overall status;
- reset-contract version and comparison result;
- capability/completeness flags;
- warnings and diagnostics; and
- opaque or content-addressed raw telemetry needed to audit the decision.

## Explicit Exclusions

`NavigationContextResult` does not contain:

- camera/control orientation as a predictor input;
- a raw controller tape or stick-to-world transform;
- a guessed route, goal, or objective;
- patched exploration state;
- an implicit reference to the latest content/world/model artifact; or
- a fabricated zero velocity, step count, or encounter reset.

Camera and input orientation may be captured separately as empirical telemetry for collision/anomaly
probes and for `NavigationControlSolveResult`. Their absence from the planning context is deliberate: the
predictor plans in world/navigation space, and the later solver discovers how to realize that plan.

## Required Statuses

| Status | Meaning | Planning consequence |
|---|---|---|
| `Ready` | Reset-qualified, stable, and complete for the requested model | Navmesh Survey and prediction may reference this result |
| `NonResetContinuation` | Control returned without a qualifying reset, such as a same-script cutscene | Continue the existing epoch; never start a fresh one |
| `VelocityNonZero` | Actor placement may be visible, but measured velocity is not zero | Wait within a bounded stabilization policy or reject |
| `ControlNotStable` | Control, placement, input, or pending-event state is not settled | Do not plan from the capture |
| `ResetStateMismatch` | Observed reset fields do not satisfy the versioned entry contract | Reject clean-entry prediction |
| `IncompleteCapture` | Required field, identity, or provenance could not be captured | Reject models requiring that capability |
| `UnsupportedContext` | Area/profile/runtime version has no compatible capture contract | Do not infer a default |

Implementations may retain finer failure codes, but these distinctions must remain queryable.

## Zero-Velocity and Neutral-Input Rule

The predictor receives zero velocity only after runtime measurement proves it. A capture should observe a
bounded stable window rather than relying on one transient read if the field can update asynchronously.
The same applies to neutral input and queued authored input.

If stabilization does not complete within the capture budget, the result is `VelocityNonZero` or
`ControlNotStable`. SAVOR must not write zero into memory or edit the savestate to make the context fit.

## Reset-State Rule

The current working contract treats field script/context switches and battle transitions as resetting
`stepCount`, while same-script cutscenes do not. Capture records the measured value and the transition
evidence separately. It does not infer that RNG, movement accumulation, encounter tables, or other fields
share the same reset behavior.

Each required field therefore has one of:

- measured value with exact runtime provenance;
- derived value with named derivation and source fields;
- unavailable with a capability failure; or
- intentionally not required by the selected model version.

## Content and World Binding

A context captures runtime identity, but content and world materialization may finish later. Compatibility
is established by explicit IDs/hashes:

- disc and internal-content identities;
- area key/profile;
- coordinate policy;
- normalized world/graph identity; and
- model/capture schema versions.

No consumer asks for "the latest navigation context." A prediction request names one
`navigation_context_result_id`; a route validation names that context plus one selected prediction and
control result.

## Capture Procedure

The logical capture sequence is:

1. Observe a candidate transition boundary.
2. Verify field/context identity and clean source lineage.
3. Wait for bounded placement/control stabilization.
4. Measure velocity, neutral input, pending-event state, and reset-sensitive fields.
5. Anchor the spatial point when compatible world geometry is available, or record an unanchored position
   for later resolution.
6. Evaluate the versioned readiness contract.
7. At the accepted `Ready` frame, persist the exact clean output savestate and content hash.
8. Persist an immutable result and raw/audit evidence that reference that output explicitly.

The workflow may retry observation before producing a final result, but it never mutates a failed result
into `Ready`.

## Relationship to Same-Script Events

When control returns from a same-script cutscene, an observation may confirm that execution can continue,
but it is not a new `NavigationContextResult` for fresh-entry prediction. The existing context and evolved
model state remain authoritative. If the predictor cannot represent the event and its state changes, the
prediction terminates with `ModelIncomplete`.

## Acceptance Rules

- Every ready context traces to one clean savestate/runtime lineage and one reset-qualified boundary.
- Measured velocity is zero and authored input is neutral at capture.
- Context data contains no camera-to-stick planning requirement.
- Step count and other encounter fields retain independent evidence.
- Downstream artifacts reference an explicit context result ID.
- Patched exploration captures cannot satisfy this contract.
