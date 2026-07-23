# 03 - Navigation Pathfinder

## Status

Future plan and living draft.

## Purpose

`NavigationPathfinder` is the reusable spatial-navigation service for fields with or without encounters.
It predicts an achievable frame-wise player trajectory from a navigation context to a spatial target.

It owns:

- navigation-mesh traversal and reachability;
- geometry-aware route alternatives;
- direction-change timing;
- acceleration and velocity evolution;
- frame-wise pose prediction;
- spatial lower bounds and search guidance;
- predicted trajectory statistics.

It does not own:

- RNG or StepCounter behavior;
- encounter selection or avoidance;
- controller-input generation;
- live execution;
- trajectory ranking.

## Search state

The physical search state should contain the minimum movement state required to advance
`FieldNavigationModel`, including:

- mesh location and exact or model-appropriate player position;
- facing angle;
- velocity and acceleration state;
- elapsed predicted frames;
- any geometry-response state proven necessary by live testing.

State precision and equivalence rules must be refined empirically. Do not merge states that can produce
different subsequent movement merely because their positions fall in the same coarse mesh region.

## Search transitions

Search should use deterministic movement primitives rather than independently branching over arbitrary
position deltas for every frame. Candidate primitives may include:

- continue along the current heading;
- begin or continue a direction change;
- accelerate, coast, or decelerate according to the learned model;
- traverse toward a mesh portal or target region;
- remain stationary when a spatial navigation use case actually requires it.

Each primitive expands through `FieldNavigationModel` into one or more predicted frames. The final
trajectory remains frame-wise even if the frontier operates at meaningful movement-decision boundaries.

## Spatial guidance contract

Encounter-aware search needs navigation information without accepting a completed black-box path.
`NavigationPathfinder` should therefore expose or build a reusable spatial guide containing:

- reachability to the requested spatial target or set of encounter-compatible regions;
- a lower bound on remaining distance;
- a lower bound on remaining movement frames;
- candidate mesh corridors or portal choices;
- known region-boundary crossings.

The lower bounds should ignore RNG and may optimistically ignore direction-change or geometry penalties
when necessary to remain safe search heuristics.

## Ordinary navigation result

A successful request returns one or more `PredictedNavigationTrajectory` values. A failed request must
distinguish:

- invalid or unresolved start/target;
- unreachable target in the supplied mesh;
- unsupported movement-model input;
- search budget exhausted without proving unreachability.

The number of alternatives and budget policy remain configurable planning details; they must not be
silently interpreted as a proof that no path exists.

## Reuse by encounter-aware search

`NavigationEncounterSolver` reuses:

- `FieldNavigationModel`;
- mesh and target types;
- spatial guides and lower bounds;
- predicted trajectory and statistics types.

It does not call `NavigationPathfinder` once for a shortest trajectory and then insert timing changes.
Encounter goals can favor a longer route, additional region-0 movement, or a different encounter-region
entry, so the encounter solver retains ownership of its joint frontier.
