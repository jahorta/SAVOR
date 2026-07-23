# 06 - Validation, Refinement, and Roadmap

## Status

Future plan and living draft.

## Validation philosophy

Navigation behavior will be learned and refined through successive prediction-versus-live-result trials,
as with the battle predictor. Static research establishes known game rules; live runs determine which
movement state, geometry effects, and timing details must be added to reproduce field behavior.

Predictor correctness and live controller reproducibility are separate questions:

- SavorPredict predicts a trajectory and encounter result.
- `NavigationTestPhase` attempts to reproduce that trajectory.
- Comparison artifacts identify whether divergence came from the movement model, controller following,
  encounter model, missing context, or an unsupported field RNG source.

## First slice: `a101b`

The first slice uses `a101b` because its traversal encounter behavior is step-counter-only.

Deliverables:

1. Load a validated `a101b` navigation mesh and encounter overlay.
2. Initialize navigation state, step count, encounter table, and RNG from live capture.
3. Validate `FieldNavigationModel` frame-wise movement predictions on short controlled segments.
4. Validate `EncounterStepModel` and `StepEncounterEnumerator` independently for known seeds.
5. Run `NavigationPathfinder` to a simple spatial target and emit a predicted trajectory.
6. Run `NavigationEncounterSolver` for:
   - a target location with no encounter;
   - a selected encounter in a compatible region.
7. Execute selected trajectories through `NavigationTestPhase`.
8. Compare predicted and observed results and update the planning gap list.

Arbitrary idle waits are excluded from the first encounter search because they change neither RNG nor
step count in this profile.

## Comparison artifact

A reusable comparison artifact aligns the predicted and observed trajectory by relative frame and
records:

- predicted and observed absolute position;
- predicted and observed position delta;
- predicted and observed facing angle and facing delta;
- position and angular residuals;
- live controller input applied;
- predicted and observed target/encounter completion;
- first divergent frame and classified divergence category;
- optional internal movement, encounter-region, step-count, and RNG fields when captured.

The artifact should preserve profile/model versions and source trajectory identity so a later model
change can be evaluated against the same evidence.

## Acceptance targets for the planning phase

Initial implementation acceptance should be defined with real trial data rather than invented numerical
tolerances. Before coding a pass/fail threshold, collect:

- straight movement from rest;
- sustained movement at stable velocity;
- direction changes at several angles;
- acceleration and deceleration near a target;
- movement across representative geometry and collision boundaries;
- transitions between encounter region `0` and nonzero regions.

Use those trials to establish position, facing, and timing tolerances for `NavigationTestPhase`.

## Later slices

### Movement-model refinement

- Add direction-change, acceleration, velocity, and geometry rules identified by comparison artifacts.
- Extend navigation context only when a missing live field explains observed divergence.
- Preserve provisional/unsupported status for behavior that is not yet reproducible.

### Non-StepCounter traversal RNG

- Select a non-overworld field with a confirmed traversal RNG consumer.
- Add a field-specific `TraversalRngModel`.
- Introduce only the callback phase/state required to reproduce the observed draw order.
- Enable wait transitions and validate that stationary frames shift RNG without advancing the step count.
- Re-run joint-search pruning against live traces.

### Broader field coverage

- Confirm the encounter-selector convention in additional fields.
- Validate GRND and GOBJ encounter-region overlays.
- Add model/profile versioning rules as multiple field behaviors become supported.

## Remaining open questions

1. What is the exact spatial and versioning contract for navigation meshes and encounter overlays?
2. What search budget or proof standard distinguishes exhaustive failure from budget exhaustion?
3. Which additional navigation-context fields are required after the first live comparisons?
4. Which active MLD callback state is required by the first non-StepCounter traversal RNG profile?
5. How broadly does the decoded encounter-selector convention hold beyond `a101b` and the researched
   Catacombs assets?
6. What live-following tolerances should be used after the initial controlled trajectory corpus exists?
