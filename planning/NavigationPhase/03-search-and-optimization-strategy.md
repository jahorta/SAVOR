# 03 - Search and Optimization Strategy

## Status

Future plan. This document assumes `SavorNavigation` has converted canonical SpiceMLD data, projected
world/search evidence, and transiently flattened wall, trigger, and provisional MovingObject geometry into an in-memory
`NavigationAreaModel`.
Route search and Qt code consume only that SAVOR-owned model; they do not consume SpiceMLD/Blender types
or a serialized SPICE area-view artifact.

## High-Level Strategy

Use a two-layer pipeline:

1. **Route planner** over 3D graph/navmesh abstractions.
2. **Control solver** that follows route splines and finds executable player+camera inputs.

MVP prioritizes correctness and continuity over advanced optimization.

The implemented first slice stops after loading and visualizing one manually selected MLD. It includes
native GRND surfaces and GOBJ meshes referenced in the ground role, but intentionally does not render
links or run path search. The next slice calibrates coordinates, derives traversable adjacency, adds
start/target selection, and exercises path search. Worker-backed control solving and persisted route
artifacts follow after the model and widget boundary are validated.

## Stage A: Route Search (MVP)

## Baseline algorithm
- A* over the 3D walk graph/navmesh stored in `NavigationAreaModel`.
- Heuristic: geometric distance + coarse transition penalties.
- Reject models where either `hasCompleteGroundGeometry` or `hasCompleteWallGeometry` is false; partial
  models are diagnostic visualization inputs, not valid search worlds.
- Treat `hasCompleteTriggerGeometry` as a visualization diagnostic rather than a search-readiness gate in
  this prototype. Attached trigger meshes do not yet encode SCT/controller activation semantics.
- Treat `hasCompleteMovingObjectGeometry` the same way. `motscpt` mesh presence alone does not establish
  door identity, animation state, collision behavior, or traversability.

## Edge costs (MVP)
- base traversal estimate
- coarse slope modifier
- interaction/cutscene duration estimate

`cost_mvp = expected_vi_coarse`

No heavy variance or risk modeling in first pass.

## Candidate strategy
- Produce top-K route candidates (small K, TBD).
- Keep pipeline simple and observable.
- Return route geometry in SAVOR-owned types that the Navigation widget can overlay without parser or Qt
  dependencies in the planner.

## Stage B: Route-to-Spline

- Convert route nodes/edges into a followable spline/path representation.
- Mark interaction waypoints and cutscene/transition boundaries.
- Persist checkpoints for downstream workers once workflow integration begins; the interactive prototype
  may keep its selected path in memory.

## Stage C: Control Solver (Workers)

Given spline segments:
- Iterate on camera+player inputs to follow spline.
- Use savestate checkpoints at segment boundaries.
- Emit best successful candidate per segment.

Planned VM support (new instructions):
- Load route spline into VM context.
- Attempt iterative spline-following policy.
- Return telemetry (deviation, frames, trigger hit status).

## Stage D: Cutscene Handling

When a cutscene triggers:
1. Detect section transition/runtime cutscene boundary.
2. Execute through cutscene.
3. Re-anchor at post-cutscene state.
4. Continue with remaining route/spline.

## Objective Function (MVP)

Primary:
- minimize VI frames to objective completion signal.

Secondary (lightweight):
- prefer successful/clean trigger completion.

## Determinism Policy (MVP)

Assume deterministic replay from savestate + identical inputs.
- No mandatory repeated validation gate initially.
- Add repeat-run validation only if failures appear.

## Telemetry to Capture

- route candidate ID
- segment boundaries
- per-segment frame counts
- trigger/cutscene section hits
- final outcome code
- spline deviation metrics

## Open Experiments (post-MVP)

1. Better slope/collision-aware edge costs.
2. Robust collision-boost exploitation strategy.
3. Advanced optimizers for global+local coupling.
4. Optional stability-aware ranking if determinism issues emerge.
