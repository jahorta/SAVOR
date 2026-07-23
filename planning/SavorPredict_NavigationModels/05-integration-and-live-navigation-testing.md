# 05 - Integration and Live Navigation Testing

## Status

Future plan and living draft.

## SavorPredict request boundary

An ordinary navigation request supplies:

- navigation context;
- validated navigation mesh;
- target overlay and selected spatial target;
- movement-model/profile configuration;
- search budget.

An encounter-aware request additionally supplies:

- encounter overlay and encounter-table data;
- one or more initial RNG seeds to test;
- either a no-encounter target location or a target encounter.

The service treats each seed independently and returns seed-specific trajectories.

## Public result boundary

SavorPredict returns:

- zero or more `PredictedNavigationTrajectory` artifacts;
- the spatial target or target encounter region;
- predicted goal outcome;
- per-trajectory statistics for ranking;
- an explicit failure classification when no trajectory is returned.

SavorPredict does not return:

- controller inputs;
- claims of live executability;
- reliability scores;
- repeated-run validation results;
- detailed predictor evidence in the normal SavorQt result.

## SavorQt responsibilities

SavorQt:

- renders predicted trajectories over the field mesh and overlays;
- displays predicted outcome and trajectory statistics;
- ranks or filters alternatives according to operator-selected criteria;
- allows an operator or workflow to select a trajectory for live testing;
- displays predicted-versus-observed comparisons returned by the worker phase.

Initial ranking inputs are predicted elapsed frames, distance, accumulated facing change, direction-change
count, trajectory complexity, eligible moved frames, stationary frames, and encounter-region crossings.
SavorQt owns ranking policy; SavorPredict only computes the statistics.

## `NavigationTestPhase`

The reusable live navigation-test phase belongs to SavorWorkflow/SavorWorker rather than SavorPredict.

### Inputs

- selected `PredictedNavigationTrajectory`;
- starting savestate or equivalent worker runtime state;
- ISO and Dolphin runtime configuration;
- target identity and expected encounter outcome;
- live capture/profile configuration;
- following tolerances and test budget.

### Behavior

For each trajectory frame, the worker attempts to choose controller inputs that move the live player
toward the predicted pose and facing. Controller synthesis can use feedback from captured live state, but
must preserve both the emitted input tape and observed state for auditability.

The first implementation should treat the predicted trajectory as a target to follow, not as proof that
each pose is controller-realizable.

### Outputs

- the controller input tape actually executed;
- observed frame-wise position and facing;
- observed position and facing deltas;
- target-completion and encounter outcome;
- total and per-frame deviation statistics;
- first-divergence information;
- a predicted-versus-observed comparison artifact.

The live test result is separate from the predictor result. A test failure does not rewrite the original
prediction artifact.

## Refinement feedback

Comparison artifacts become inputs to later model-analysis work. They should support aligning predicted
and observed frames and measuring:

- position and facing residuals;
- direction-change timing error;
- acceleration and velocity error;
- collision or geometry divergence;
- encounter-region, step-count, and RNG divergence when captured.

The first phase does not automatically update SavorPredict parameters. Refinement remains an explicit
offline or developer-controlled operation so a bad or unrepresentative run cannot silently mutate the
model.

## Workflow placement

The exact persisted step kinds and schemas will be selected when implementation begins, but the logical
flow is:

```text
Predict trajectories
    -> SavorQt ranks/selects
    -> NavigationTestPhase follows selected trajectory
    -> observed trace and comparison artifact
    -> model refinement work
```

The same navigation-test phase can validate ordinary pathfinder trajectories and encounter-aware
trajectories.
