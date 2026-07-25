# Navigation Context Workflow

## Status

Mixed current/future plan. The draft Navigation Context export exists and supplies the starting point for
the planned **Navmesh Survey**. Content materialization, per-area runtime refinement, predicted-outcome
search, controller realization, and clean validation remain future workflow stages.

The existing `SavorNavigation` and `SavorQt3D` work supplies an in-memory scenario, geometry, graph,
manual/scripted endpoints, A*, and visualization prototype. The draft Navigation Context phase is also
implemented and has produced the concrete `a101b` `.sav`/`.nctx` bootstrap named in document 04. The
Navmesh Survey, Navigation-specific disc extraction, navigation-prediction service, full workflow, and
durable Survey artifacts remain unimplemented.

## Normative Workflow

```text
explicit source savestate
  -> nav.capture_context
  -> nav.materialize_content
  -> nav.build_world
  -> nav.explore_geometry
  -> nav.build_refinement
  -> nav.capture_prediction_start
  -> nav.search_predicted_outcomes
  -> explicit user/result selection
  -> nav.solve_controls
  -> nav.validate_route
  -> nav.publish_result
```

The workflow has five non-interchangeable evidence layers:

1. **Static content and world** derived from an identified disc image and versioned parsers.
2. **Navigation Context bootstrap** exported as one explicit `.nctx` plus matching savestate.
3. **Patched Navmesh Survey observations and refinement** produced for per-area spatial passability,
   connectivity, collision boundaries, verified positional anchors, and door/access constraints.
4. **Predicted plan** derived from an explicitly referenced future `NavigationPredictionStart`, content
   bundle, world refinement, objective, search bounds, and model bundle.
5. **Clean runtime validation** performed from the unpatched prediction-start lineage.

No layer silently replaces another. In particular, patched exploration states are never prediction
starts or clean-validation inputs. The future `nav.capture_prediction_start` is distinct from the current
Survey-bootstrap `nav.capture_context`; downstream work never resolves an implicit latest result.

## Core Decisions

The epoch, reset, prediction, and validation decisions below govern later workflow stages. They are not
additional readiness requirements for the current Navigation Context export or Navmesh Survey bootstrap.

- A `NavigationEpoch` begins only after a reset-qualified field script/context switch or battle return
  has completed and player control is stable.
- Entering battle ends the current epoch. Returning to field control begins another context.
- A same-script cutscene that returns control stays inside the current epoch. It does not reset the
  encounter step count and cannot authorize a second clean-entry assumption.
- A save load is reset-qualified only when runtime evidence confirms that it produces the required
  script switch and reset contract.
- Prediction starts with measured zero player velocity and neutral authored input. These are readiness
  evidence, not values fabricated by the planner.
- Camera and control orientation are not fields of the future prediction-start contract and are not
  planning inputs.
  They belong to empirical probe telemetry and later controller realization.
- `SavorPredict` owns planning-level temporal and outcome search. It emits a world-space route plus a
  movement/no-movement/interruption schedule, not raw stick input.
- `NavigationControlSolveResult` owns camera/controller realization, and `NavigationValidationResult`
  owns comparison against an unmodified runtime.
- Encounter suppression is a named runtime modification independent of trigger control. The first
  Navmesh Survey slice normally suppresses trigger commit at `0x80117e8c`, briefly restores the original
  instruction only for an intended door interaction, and immediately suppresses it again. Broader
  trigger characterization remains future work.
- Unfinished field-RNG research, including Moonfish behavior, is an unresolved versioned
  model-completeness dependency. This package makes no behavioral claim from that work.

## Current Versus Planned Responsibilities

| Capability | Current repository state | Planned owner |
|---|---|---|
| MLD/SCT loading, geometry, graph, A*, and Qt prototype | Implemented in-memory prototype | `SavorNavigation`, `SavorQt3D` |
| Battle-context-style runtime capture pattern | Existing precedent | `SavorCore`, `SavorWorker`, `SavorDb` |
| Navigation context capture | Draft implemented; concrete `a101b` `.sav`/`.nctx` exported | `SavorCore`, `SavorWorker`, `SavorDb` |
| ISO identity and content extraction | Not implemented for Navigation | Dolphin DiscIO, SPICE/ALX, `SavorNavigation` |
| Runtime patch/teleport support | Paused `u32` write exists; byte write, safe executable-patch lifecycle, and teleport/settle operations are missing | `SavorCore` |
| Navmesh Survey and refinement | Not implemented | `SavorNavigation`, `SavorCore`, `SavorWorker` |
| Prediction-start capture | Not implemented; separate from the Survey-bootstrap `.nctx` | `SavorCore`, `SavorWorker`, `SavorDb` |
| Navigation prediction service | `SavorPredict` is currently battle-focused/exploratory | `SavorPredict` future navigation boundary |
| Local CPU workflow lane | Not implemented | `SavorWorkflow` future executor |
| Durable navigation workflow/artifacts | Not implemented | `SavorDb`, object store, `SavorWorkflow` |
| Product workflow UI | Not implemented | `SavorQt` |

The hidden `dungeon_explorer` workflow placeholder currently has only entry and terminal savestate
bindings. It is a naming and product-integration anchor, not proof that the internal workflow exists.

## Document Map

- [01 - Future Prediction Entry Boundaries and Lifecycle](01-entry-boundaries-and-lifecycle.md)
  - Defines the later `NavigationEpoch`, reset-qualified prediction starts, same-script interruptions,
    battle boundaries, save-load qualification, and lifecycle outcomes.
- [02 - Disc Content and Provenance](02-disc-content-and-provenance.md)
  - Defines disc identity, extraction manifests, parser ownership, content bundles, hashes, and cache
    invalidation.
- [03 - Navigation Context Contract](03-navigation-context-contract.md)
  - Describes the implemented `.nctx` plus matching-savestate capture used only as the Survey bootstrap.
- [04 - Navmesh Survey and World Refinement](04-suppressed-exploration-and-world-refinement.md)
  - Specifies the per-area spatial Survey, common-baseline anchor-establishment and teleport fan-out
    waves, door/BitVar handling, coverage, and refinement while separating baseline and modified jobs.
- [05 - Prediction, Control, and Validation](05-prediction-control-and-validation.md)
  - Defines the planning-level predictor boundary, control realization, validation, and interruption
    handling.
- [06 - Phase, Job, and Artifact Integration](06-phase-job-and-artifact-integration.md)
  - Maps the logical steps, owners, executors, persisted identities, artifacts, and failure behavior onto
    SAVOR.
- [07 - Open Research Questions](07-open-research-questions.md)
  - Records research gates that must not be converted into implementation assumptions.

## Relationship to Other Navigation Specifications

The authoritative [Execution Runtime Refactor Guidance](../../ExecutionRuntime/README.md) defines the
future worker/session/program foundation used to implement these phases. This package remains authoritative
for Navigation Context and Navmesh Survey domain semantics, evidence, waves, and artifact lineage; the
Execution Runtime package determines how those bounded programs, actions, services, and workflow bindings
are expressed. Neither document makes the currently unimplemented Navmesh Survey appear implemented.

This package is normative for context lifecycle, content provenance, runtime refinement, predictor/control
separation, and workflow lineage. The parent [Navigation planning overview](../README.md) remains the
top-level status and roadmap. [Area Profiles and Analysis Workstreams](../06-area-profiles-and-analysis-workstreams.md)
remains normative for Dungeon, Safe, Overworld, and Unknown/View-only classification and for encounter
geography. Where an older workflow sketch conflicts with this package, this package takes precedence.
In particular, the Navmesh Survey direction in document 04 supersedes fixed job-wide selective-trigger
profiles, per-anchor-savestate designs, serialized-ground replay requirements, timing-based Survey
telemetry, and collision/anomaly-only sketches elsewhere in Navigation planning.

The current target is non-099 Dungeon Navigation. Safe Navigation may later reuse the same 2.5D context,
exploration, control, and validation contracts without encounter work. Area 99/Overworld remains a
separate future design.
