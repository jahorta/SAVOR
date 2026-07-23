# 01 - Scope and Architecture

## Status

Future plan and living draft.

## Goals

Build a reusable field-navigation prediction family that:

1. predicts frame-wise field movement from a live navigation context and validated mesh;
2. solves ordinary navigation without requiring encounter logic;
3. jointly searches navigation, encounter-region, step-counter, and RNG state when an encounter outcome
   matters;
4. produces predicted trajectories for SavorQt and live worker validation;
5. supports iterative model refinement without coupling SavorPredict to controller synthesis.

## Non-goals for the first slice

- Overworld (`a099*`) encounter prediction.
- Automatic conversion of a predicted trajectory into controller inputs inside SavorPredict.
- Automatic model-parameter updates from live validation results.
- Repeated-run reliability scoring.
- Complete modeling of active MLD callback state.
- A universal or heavily abstracted search framework before the concrete searches establish common needs.

## Component model

### `FieldNavigationModel`

The shared deterministic movement model. It advances player navigation state by one field frame and
resolves:

- position and position delta;
- facing angle and facing-angle delta;
- acceleration and velocity;
- collision and geometry response;
- current navigation-mesh location;
- decoded encounter-region ID.

It does not apply RNG, encounter goals, ranking policy, or controller inputs.

### `NavigationPathfinder`

The standalone navigation service. It uses `FieldNavigationModel` to find predicted trajectories to
spatial targets on fields with or without encounters. It also exposes spatial guidance, route
alternatives, and lower-bound estimates that can guide encounter-aware search.

### `EncounterStepModel`

The exact non-overworld encounter-step transition. It uses the shared SavorPredict RNG core and applies
the known encounter eligibility, step increment, probability draw, and encounter-row selection rules.

### `StepEncounterEnumerator`

The restricted encounter service for fields where the StepCounter is the only per-frame RNG consumer.
For an initial seed and encounter table, it repeatedly applies `EncounterStepModel` to enumerate the
outcomes associated with eligible moved-step ordinals. It has no geometry or pathfinding responsibility.

### `TraversalRngModel`

An optional ordered model of non-StepCounter RNG consumers that run during field traversal before the
encounter check. The `a101b` first slice uses an empty implementation and captures no active MLD callback
state. Later field profiles can add the minimum state proven necessary by live comparison.

### `NavigationEncounterSolver`

The joint navigation and encounter search. It advances candidate states through:

1. `FieldNavigationModel`;
2. `TraversalRngModel`;
3. `EncounterStepModel`.

It uses pathfinder-provided spatial guidance and lower bounds, but does not ask the pathfinder for a
finished route and then add waits afterward. Navigation, region selection, step progression, and RNG
evolve together during search.

## Composition

```text
                         +-------------------------+
                         | FieldNavigationModel    |
                         +-----------+-------------+
                                     |
                  +------------------+------------------+
                  |                                     |
        +---------v------------+             +----------v----------------+
        | NavigationPathfinder |             | NavigationEncounterSolver |
        +----------------------+             +-----+---------------+-----+
                                                     |               |
                                           +---------v------+  +-----v---------------+
                                           | TraversalRngModel | | EncounterStepModel |
                                           +------------------+ +----------+----------+
                                                                          |
                                                               +----------v-----------+
                                                               | StepEncounterEnumerator |
                                                               +----------------------+
```

`NavigationPathfinder` and `NavigationEncounterSolver` are sibling services over shared models. There is
no inheritance relationship and no black-box parent/caller pipeline between their public result APIs.

## Why the search remains specialized

Ordinary navigation searches physical state, while encounter-aware navigation searches physical state
plus encounter region, step count, RNG state, and eventually traversal-RNG state. Their goal tests,
dominance keys, and pruning rules differ materially. The first implementation should share deterministic
models, mesh types, trajectory types, and small frontier utilities only where the code demonstrates real
commonality.
