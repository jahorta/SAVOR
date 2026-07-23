# SavorPredict Navigation Models

## Status

Future plan and living draft.

This folder is the authoritative planning set for reusable field-navigation prediction in SavorPredict.
The contracts, model boundaries, search strategy, and validation process are expected to evolve through
prediction-versus-live-result testing, following the iterative approach used by the battle predictor.

The broader `planning/NavigationPhase/` documents continue to own workflow-level navigation planning,
SPICE integration, UI objectives, persistence, and phase orchestration. This folder owns the
SavorPredict models that produce predicted field-navigation trajectories and encounter outcomes.

## Scope

The planned model family supports:

- standalone navigation through a validated field navigation mesh;
- encounter-aware navigation that seeks a specified encounter;
- navigation to a target without triggering an encounter;
- enumeration of encounter outcomes when the step counter is the only per-frame RNG consumer;
- live worker tests that attempt to reproduce predicted trajectories with controller inputs.

Area 99 (`a099*`) is the overworld and is outside the initial scope. The first validation field is
`a101b`, which uses the step-counter-only encounter model.

## Terminology

- **Predicted navigation trajectory:** a seed-specific or seed-independent frame-wise prediction of player
  position and facing. It is not an executable controller input tape.
- **Eligible moved frame:** a field update whose resolved player-position delta is nonzero and whose
  encounter-region ID allows encounter progression.
- **Encounter region:** a mesh/collision region whose decoded selector identifies the active
  non-overworld encounter table. Region `0` is a no-encounter region.
- **Live navigation test:** a worker-owned execution phase that attempts to reproduce a predicted
  trajectory using controller inputs and records the observed trajectory.

## Documents

- `01-scope-and-architecture.md`
  - Model boundaries, ownership, and composition.
- `02-frame-model-and-predicted-trajectory.md`
  - Per-frame state transitions and trajectory contracts.
- `03-navigation-pathfinder.md`
  - Standalone pathfinding and reusable spatial guidance.
- `04-encounter-model-and-joint-search.md`
  - Exact encounter-step behavior and encounter-aware search.
- `05-integration-and-live-navigation-testing.md`
  - SAVOR, SavorQt, and worker-facing contracts.
- `06-validation-refinement-and-roadmap.md`
  - Live comparison, refinement artifacts, staged delivery, and open questions.
- `07-research-evidence.md`
  - Source-backed encounter-system findings.

## Ownership boundary

- SPICE owns MLD/SCT and related file parsing, navigation-mesh source data, decoded collision selectors,
  and target discovery.
- SavorPredict owns deterministic field-navigation models, predicted trajectories, encounter-step
  simulation, and search.
- SavorWorkflow and SavorWorker own live navigation-test orchestration and controller-input execution.
- SavorQt owns trajectory presentation, operator selection, and result ranking.
