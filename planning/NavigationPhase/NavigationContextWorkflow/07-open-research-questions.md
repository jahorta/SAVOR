# 07 - Open Research Questions

## Status

Future research register. Items here are blockers or confidence gates, not permission to fill missing
behavior with convenient assumptions. Resolution requires a durable evidence artifact, reviewed contract
change, and updates to the affected workflow/model versions.

## Resolved Planning Decisions

The following are closed at the architectural level:

- Prediction begins only at a reset-qualified field script/context switch or battle return.
- Entering battle terminates the current `NavigationEpoch`; battle return begins a new context.
- A same-script cutscene that returns control remains in one epoch and does not authorize a fresh-entry
  reset assumption.
- The predictor starts with measured zero velocity and neutral authored input.
- Camera/control orientation is excluded from the future `NavigationPredictionStart` and planning inputs.
- Disc and content identity is hash-based; machine paths are locators only.
- Static world, patched exploration, prediction, control solving, and clean validation are separate
  immutable evidence layers.
- The first Navmesh Survey slice uses the explicitly named `navigation-context-41.sav` and adjacent
  `navigation-context-41.nctx` as one common per-area bootstrap.
- Published Survey anchors are positional records. Later workers reload the common bootstrap, teleport,
  allow the game to reconstruct ground/collision state, and accept an anchor only after a usable settle.
- Encounter suppression writes one zero byte to `0x8030b7ad`. Trigger commit is normally suppressed at
  `0x80117e8c` with `0x48000018`; a door job temporarily restores `0x480F86C5` for the intended
  interaction and immediately suppresses again. `eventhook` is outside the first slice.
- Door success requires expected-TBLID identity plus physical crossing and far-side settle. A known lock
  BitVar may be changed only in the isolated job, with original/override values and `initially_locked`
  retained on the portal.
- Parallel workers emit verified anchor records and immutable observations; deterministic domain reduction
  produces the refinement.
- Prediction emits world-space planning and a movement/no-movement/interruption schedule; a later solver
  emits controller input.
- Prediction work references an explicit `NavigationPredictionStart` ID; it does not reuse the
  Survey-bootstrap `.nctx` or resolve an implicit latest result.

## Runtime Entry and Reset Research

1. Which exact runtime functions/breakpoints prove that a field script/context switch has completed?
2. Which fields prove that player control, placement, and active ground are stable?
3. What stable-window length is sufficient for zero velocity and neutral queued input?
4. Which transition paths into and out of battle reset `stepCount`, and at which frame?
5. Which save-load paths perform a qualifying script/context switch, and which do not?
6. Which reset-sensitive encounter/RNG fields change independently of `stepCount` at each boundary?
7. How should an unexpected transition during capture be classified and resumed?

The working reset rule from gameplay testing remains versioned and provisional until these measurements
are captured across representative areas.

## Context Capture Contract

1. What is the minimal exact set of RNG, encounter, table, accumulator, modifier, and pending-event fields
   required by the first supported predictor model?
2. Which values can be read atomically, and which require a multi-frame consistency protocol?
3. How is the active actor/object identity proven for field navigation?
4. How is an unanchored runtime position associated with split/stacked ground pieces without guessing?
5. Which runtime/game revisions require distinct capture contracts?
6. What raw telemetry is necessary to audit a readiness decision later?

## Runtime Patch and Exploration Research

1. What safe SavorCore memory-write/patch API should enforce expected-original-value checks and restoration?
2. The first-slice encounter-suppression write is byte value `0` at `0x8030b7ad`. What expected-original-value,
   application/restoration boundary and cross-runtime validation are required before treating it as a
   reusable profile?
3. What paused executable-patch surface performs required instruction-cache handling and proves
   expected-original, written, restored, and read-back words at `0x80117e8c`?
4. What teleport implementation and positional tolerance define a usable settled anchor without
   serializing ground-selector state?
5. What runner safeguard ends a teleport attempt that never settles, while keeping that execution control
   out of persistent navmesh evidence?
6. What probe density, approach coverage, and adaptive tolerance are sufficient to classify a
   sub-triangle boundary?
7. How should contradictory passability evidence be reproduced and adjudicated?
8. What durable schema best represents positional anchors, portal lock constraints, spatial observations,
   and tested/untested coverage?

The following broader questions are deferred beyond the door-only first slice:

1. Where are all automatic and interactable trigger classes detected, dispatched, and completed across
   `eventhook`, field initialization, MovingObjects, platforms, and collision-resource controllers?
2. What generalized trigger-control or allowlist contract can exclude unrelated activations?
3. For automatic triggers, what spatial/state evidence defines every available approach boundary? For
   interactable triggers, which position, distance, facing, input, occlusion, and state dimensions define
   the activation envelope?
4. Which separate telemetry and parameter sweeps characterize timing-dependent movement anomalies such
   as sticky-corner jumps or wall-contact ramp-speed effects?

These deferred questions do not block the exact first-slice door toggle, expected-TBLID verification,
BitVar override, physical crossing, or common-baseline teleport-and-settle flow. They do block a
general-purpose trigger-characterization job.

## Predictor and Field-Model Research

1. What reusable asynchronous boundary should expose navigation search from `SavorPredict`: library,
   service/process protocol, or another isolated executor?
2. Which field-update and RNG sources are required for the first no-encounter objective?
3. Which additional sources are required to choose a specific encounter and formation?
4. How are same-script cutscene duration, forced movement, state writes, and returned-control timing modeled
   without beginning a new epoch?
5. Which interruption classes can be abstracted, and which require instruction/runtime simulation?
6. How is movement eligibility represented independently from controller realization?
7. What search bounds and deterministic tie-breaking make candidate reproduction stable?
8. What model-completeness status is required before a prediction can be offered for clean validation?

The ongoing Moonfish/field analysis is explicitly unfinished. Until reviewed results are converted into a
versioned model component, these documents assume no pull cadence, ordering, start frame, per-frame count,
or movement/no-movement effect from that source.

## Content and Provenance Research

1. Which Dolphin DiscIO interface and thread/process boundary is appropriate for desktop extraction?
2. Which internal file paths and ALX data sets are required by the first Dungeon predictor model?
3. How should disc hashing expose progress, cancellation, and changed-while-reading detection?
4. Which SPICE/ALX parser revisions and normalization settings belong in bundle identity?
5. How should external loose-file fixtures be labeled so they cannot be confused with disc-derived content?
6. What content manifest granularity supports fast area lookup without extracting every file eagerly?

## Workflow and Persistence Research

1. What is the intended local CPU executor lane for hashing, parsing, world building, refinement, and
   publishing?
2. Which proposed logical steps become workflow step kinds versus in-process sub-operations?
3. What are the canonical schemas/codecs for context, content bundle, world/refinement, prediction,
   control, and validation results?
4. Which fields are relational columns, which are projection data, and which remain object-store blobs?
5. How should explicit candidate selection and replacement be represented in workflow lifecycle state?
6. What retry/idempotency keys are necessary for large fan-out exploration and predictor searches?
7. How are runtime-modification and trigger-control approvals and runtime-build compatibility stored and
   surfaced?
8. What retention policy applies to ordered spatial Survey telemetry, failed probes, later temporal-model
   telemetry, and historical model versions?

## Validation and Acceptance Research

1. What spatial and temporal divergence thresholds distinguish model error from control error?
2. Which clean replay count is required before calling a result reproducible?
3. Which runtime telemetry proves the encounter/formation objective independently of predictor output?
4. How are same-script interruptions compared when their duration or camera behavior varies?
5. When may a validated observation update a world refinement or model, and what review gate is required?
6. What terminal savestate and boundary evidence is sufficient to feed a subsequent context-capture step?

## Deferred Profiles

- Safe Navigation may reuse the 2.5D context/exploration/control contracts, but higher-numbered SCT-only
  traversability and event-continuation coverage remain open.
- Area 99/Overworld requires a separate movement, altitude, content, and RNG design. Nothing in this
  workflow makes Dungeon field-model assumptions valid for Overworld.

## Resolution Template

Closing an item should record:

- question and affected contracts;
- exact game/runtime/content versions;
- source research, Ghidra/runtime evidence, and reproducible fixture;
- confidence and known counterexamples;
- accepted semantic rule;
- schema/model/runtime-modification/trigger-control version changes;
- regression tests or validation procedure; and
- migration/staleness behavior for historical artifacts.

An informal conclusion or one successful playthrough may motivate research but does not close a model or
workflow contract.
