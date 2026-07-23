# 02 - Frame Model and Predicted Trajectory

## Status

Future plan and living draft.

## Inputs

### Navigation context

Live state establishing the start of prediction, initially including:

- player position;
- player facing angle or rotation;
- current velocity and acceleration inputs when available;
- current step count;
- initial RNG seed;
- current field/area identity;
- current navigation-mesh location or enough position data to resolve it.

The exact context will be refined through prediction-versus-live-result testing. The first slice does not
capture active MLD callback state.

### Validated navigation mesh

A mesh whose geometry, connectivity, and collision behavior have been validated against live game data.
SavorPredict consumes the mesh and does not parse or initially validate the source MLD.

### Overlays

- A target overlay for script triggers, treasure boxes, interactables, doors, exits, and other objectives.
- An encounter overlay that maps navigable collision regions to decoded encounter-table IDs and preserves
  raw selector provenance.

### Goal

- A spatial target for ordinary navigation.
- A spatial target that must be reached without an encounter.
- A specified encounter that must be produced in a compatible encounter region.

## Per-frame transition order

For each predicted field frame:

1. Apply the selected internal navigation action or movement primitive.
2. Advance direction-change timing, acceleration, and velocity.
3. Resolve geometry and collision.
4. Produce the resulting absolute position and position delta.
5. Produce the resulting absolute facing angle and facing-angle delta.
6. Resolve the navigation-mesh location and encounter-region ID.
7. Advance any modeled pre-StepCounter traversal RNG consumers.
8. If the position delta is nonzero and the encounter region is eligible, apply `EncounterStepModel`.
9. Record the predicted frame and any internal diagnostic events.

The internal navigation action is a search implementation detail. It is not a public controller command.

## Public trajectory contract

`PredictedNavigationTrajectory` is the public navigation artifact. It contains:

- a trajectory identifier;
- target identity or target encounter region;
- optional initial RNG seed, required for encounter-aware results;
- model/profile version;
- predicted outcome;
- ordered `PredictedNavigationFrame` records;
- aggregate ranking statistics.

Each public `PredictedNavigationFrame` contains:

- relative frame index;
- absolute predicted player position `(x, y, z)`;
- predicted position delta `(dx, dy, dz)` from the prior frame;
- absolute predicted facing angle;
- predicted facing-angle delta from the prior frame.

The first frame uses the input navigation context as its absolute pose and a zero delta unless the final
serialized contract later adopts a separate start-pose header.

## Internal prediction trace

Internal predictor tests retain a richer frame trace where applicable:

- acceleration and velocity;
- mesh polygon/triangle and encounter-region ID;
- movement eligibility and step-count changes;
- RNG state before and after modeled consumers;
- draw counts and encounter-roll results;
- collision/model events and provisional or missing-input status.

This trace follows the battle predictor's principle of preserving enough state to locate the first
prediction divergence. It is not part of the ordinary SavorQt result contract.

## Trajectory statistics

Initial public statistics should include:

- predicted elapsed frames;
- total predicted travel distance;
- accumulated absolute facing change;
- direction-change count;
- eligible moved-frame count;
- stationary-frame count;
- encounter-region crossing count;
- a simple trajectory-complexity measure derived from movement/turn transitions.

SavorPredict reports these values but does not rank trajectories.

## Distinction from controller input

A predicted trajectory states where and how SavorPredict expects the player pose to evolve. It does not
claim that a particular controller sequence can reproduce that evolution. Controller synthesis and live
execution belong to the navigation test phase described in
`05-integration-and-live-navigation-testing.md`.
