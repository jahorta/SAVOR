# 03 - Navigation Context Contract

## Status

Draft implemented. The current `NavigationContextRunner` captures a field-navigation starting point,
exports a `.nctx` payload, and saves the matching output savestate. This is the bootstrap for the Navmesh
Survey.

The current code does not implement the older proposed `Ready`, reset-qualified prediction-context
contract. Prediction-start state, temporal/RNG completeness, and later clean-validation requirements are
separate future contracts and must not be treated as Navmesh Survey prerequisites.

## Purpose

The Navigation Context phase has one purpose in the current workflow: give the Navmesh Survey an explicit
starting position and the exact savestate from which every Survey worker begins.

It answers:

1. Which savestate contains the captured field start?
2. Which area/subarea and player placement were captured?
3. Was the player worksheet, motion state, and ground selector valid at the capture point?
4. Which `.nctx` payload belongs to that savestate?

It does not build or validate the navmesh, establish Survey anchors beyond the starting point, model
movement timing, qualify a prediction epoch, or capture a complete field RNG/encounter state.

## Implemented Runtime Boundary

The current PhaseScript:

1. loads the input snapshot;
2. applies neutral input;
3. runs to `NavigationContextInitialPlayerInputReady` at capture PC `0x80111770`;
4. captures the navigation fields;
5. saves the requested output savestate;
6. emits the encoded `.nctx` blob; and
7. returns `Completed` or `Failed`.

The implemented outcome enum contains only:

- `Completed`; and
- `Failed`.

Implemented failures distinguish timeout, VI stall, host failure, unexpected stop, capture-PC mismatch,
unavailable MEM1, invalid worksheet, motion-state mismatch, invalid ground selector, encoding failure, and
savestate failure.

## Implemented `.nctx` Data

The current `NavigationContext` model contains:

- capture PC;
- live player worksheet value observed at capture;
- area and subarea;
- motion state and substate;
- post-input movement-suppression value;
- current XYZ and raw XYZ rotation;
- previous XYZ and raw previous rotation;
- step-distance carry-in;
- whether a ground selector was present; and
- the captured ground TBLID when present.

The worksheet value and any live ground pointer are diagnostic capture values, not portable pointers for a
later worker. A Survey worker resolves its own live state after loading the common bootstrap.

## Survey Bootstrap Identity

A usable Survey input explicitly names both:

- one output savestate; and
- the `.nctx` exported from the same capture.

No Survey job asks for the latest context or infers a savestate from area identity alone. For the first
`a101b` slice, the pair is:

- `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.sav`
- `C:\savor\navigation_context_a201a_a101b_20260723_2100\navigation-context-verification\navigation-context-41.nctx`

This pair supplies the initial Survey position and the common baseline reloaded by both anchor-establishment
and later parallel probe workers.

## Relationship to Survey Anchors

The captured Navigation Context position is the initial Survey anchor. Derived Survey anchors are durable
positional records produced by the Navmesh Survey; they are not new Navigation Context results.

Later workers always:

1. reload the common Navigation Context savestate;
2. teleport to a verified Survey-anchor position;
3. let the game reconstruct ground/collision state; and
4. accept the placement only after it settles usefully.

No per-anchor savestate, serialized ground-selector record, or copied live pointer is part of this
contract.

## Explicit Exclusions

The current Navigation Context does not claim:

- reset-qualified field-entry semantics;
- prediction readiness or model completeness;
- zero-velocity proof;
- a probe clock or timing model;
- complete RNG, encounter, script, or pending-event state;
- camera/controller realization;
- a route, goal, or objective;
- a derived Survey anchor or refinement; or
- an implicit latest content/world/model reference.

Future prediction or clean-validation work may define separate inputs for those needs. It must not
retroactively expand the meaning of the current `.nctx` export.

## Acceptance Rules

- The PhaseScript reaches the exact capture PC and returns `Completed`.
- The capture validates MEM1, the worksheet, motion state, and ground selector under the implemented
  codec/runtime contract.
- The `.nctx` decodes successfully and identifies the expected area/subarea and position.
- The matching output savestate exists and can be loaded.
- Survey requests name the `.sav` and `.nctx` explicitly.
- The Survey treats the pair only as its initial positional bootstrap.
