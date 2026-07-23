# 04 - Encounter Model and Joint Search

## Status

Future plan and living draft.

## Supported goals

`NavigationEncounterSolver` accepts exactly one goal:

- **No encounter:** reach a supplied spatial target without triggering any encounter.
- **Target encounter:** produce a supplied encounter in a mesh region where its encounter table is active.

Every returned encounter-aware trajectory applies to exactly one initial RNG seed.

## Exact non-overworld StepCounter model

For the normal non-overworld path:

- Player movement contributes the 3D distance between current and previous positions to
  `EncounterMovementDistanceAccum_80347410`.
- `StepCounter_800C1C24` requires that accumulator to be nonzero on an otherwise eligible field update.
- It clears the accumulator and increments `stepCount` by exactly one.
- Movement magnitude does not scale the increment or probability threshold.
- It consumes one RNG draw for the encounter probability test.
- If the probability test succeeds, encounter-row selection consumes another draw for every inspected
  ECT row until a row is accepted.

Consequences:

- A stationary frame does not advance `stepCount` and does not consume the StepCounter probability draw.
- Movement in encounter region `0` does not advance the normal encounter process.
- A tiny nonzero displacement and a large displacement each produce one advancement when all other gates
  are equal.
- Direction, acceleration, velocity, and geometry matter because they determine whether a frame moves and
  which encounter region contains the resolved player position.

`EncounterStepModel` must use the shared SavorPredict RNG core and preserve the variable row-selection
draw path. It must not replace the implementation with a single cumulative-weight roll.

## Encounter-region model

For non-overworld fields, the decoded collision selector supplies the encounter-table ID used by the
StepCounter. Region `0` is a no-encounter region. The overlay must be sampled after the frame's movement
and collision resolution so the step check uses the resolved region for that update.

The first implementation uses `a101b`. Area 99 adds an overworld-specific spatial lookup and remains
unsupported.

## Joint search state

The first-slice state contains:

- physical navigation state required by `FieldNavigationModel`;
- current encounter-region ID;
- `stepCount`;
- current RNG state;
- elapsed predicted frames and accumulated statistics.

Later profiles can add `TraversalRngModel` state when live evidence proves that a field's pre-StepCounter
RNG consumption depends on persistent callback phase or other context.

## Joint transition

For each candidate movement primitive:

1. Expand the physical transition frame by frame.
2. Resolve whether each frame is stationary or moved.
3. Resolve its encounter region.
4. Apply modeled traversal RNG consumers in field-update order.
5. Apply `EncounterStepModel` only to eligible moved frames.
6. Reject a no-encounter candidate immediately if an encounter starts.
7. Accept a target-encounter candidate only when the requested encounter starts in a compatible region.
8. Append the predicted pose frames to the candidate trajectory chain.

## `a101b` step-only specialization

`a101b` is the clean first validation field:

- `TraversalRngModel` is empty.
- A stationary wait changes neither RNG state nor step count and is therefore dominated.
- Absolute idle timing can be omitted from encounter search unless it changes the physical movement state.
- `StepEncounterEnumerator` can precompute, for each seed and encounter table, the encounter result at
  each eligible moved-step ordinal within the configured horizon.
- The joint search targets region sequences and eligible moved-frame counts that align with the desired
  precomputed outcome.
- For no-encounter navigation, any ordinal that would start an encounter is a dead transition.

This specialization reduces the first search from arbitrary frame timing to the actual game variable:
the sequence of eligible moved frames and encounter regions.

## Later traversal-RNG profiles

When a modeled field callback can consume RNG while the player is stationary, a wait can shift RNG
without advancing the step counter. Wait transitions then become legal only where
`TraversalRngModel` reports that waiting changes RNG or future modeled state.

Wait pruning should:

- discard waits that change neither physical nor RNG state;
- group or canonicalize waits only when the active RNG-consumer regime and physical state make their
  placement equivalent;
- search deterministic wait chains at meaningful regime boundaries rather than branching over arbitrary
  waits at every frame;
- use target encounter outcomes to prioritize useful future RNG states.

## Dominance and failure

At minimum, an exact dominance key includes the physical state, encounter region, step count, RNG state,
and traversal-RNG state when present. If two candidates reach the same effective state, retain the
lower-cost candidate according to the solver's search cost.

Results must distinguish:

- successful target encounter;
- successful arrival without encounter;
- proven spatial unreachability;
- unsupported or missing model input;
- search budget exhausted without proving failure.

The completeness standard and budget policy remain open and are tracked in
`06-validation-refinement-and-roadmap.md`.
