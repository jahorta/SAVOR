# Navigation Context Workflow

## Status

Future plan. This package is the normative workflow specification for producing a reset-qualified
navigation context, materializing its game content, running the **Navmesh Survey**, refining its static
world with runtime evidence, searching predicted outcomes, realizing a selected plan as controller input,
and validating it in an unmodified runtime.

The existing `SavorNavigation` and `SavorQt3D` work supplies an in-memory scenario, geometry, graph,
manual/scripted endpoints, A*, and visualization prototype. It does not yet implement the context
capture, disc extraction, Navmesh Survey, navigation-prediction service, workflow jobs, or durable
artifacts described here.

## Normative Workflow

```text
reset-qualified field entry
  -> nav.capture_context
  -> nav.materialize_content
  -> nav.build_world
  -> nav.explore_geometry
  -> nav.build_refinement
  -> nav.search_predicted_outcomes
  -> explicit user/result selection
  -> nav.solve_controls
  -> nav.validate_route
  -> nav.publish_result
```

The workflow has five non-interchangeable evidence layers:

1. **Static content and world** derived from an identified disc image and versioned parsers.
2. **Clean navigation context** captured at a reset-qualified field entry.
3. **Patched Navmesh Survey observations and refinement** produced for passability, movement-response,
   collision-oddity, survey-anchor, and trigger-activation discovery.
4. **Predicted plan** derived from an explicitly referenced context, content bundle, world refinement,
   objective, search bounds, and model bundle.
5. **Clean runtime validation** performed from the original unpatched context lineage.

No layer silently replaces another. In particular, patched exploration states are never prediction
contexts or clean-validation inputs, and downstream work never resolves an implicit "latest context."

## Core Decisions

- A `NavigationEpoch` begins only after a reset-qualified field script/context switch or battle return
  has completed and player control is stable.
- Entering battle ends the current epoch. Returning to field control begins another context.
- A same-script cutscene that returns control stays inside the current epoch. It does not reset the
  encounter step count and cannot authorize a second clean-entry assumption.
- A save load is reset-qualified only when runtime evidence confirms that it produces the required
  script switch and reset contract.
- Prediction starts with measured zero player velocity and neutral authored input. These are readiness
  evidence, not values fabricated by the planner.
- Camera and control orientation are not fields of `NavigationContextResult` and are not planning inputs.
  They belong to empirical probe telemetry and later controller realization.
- `SavorPredict` owns planning-level temporal and outcome search. It emits a world-space route plus a
  movement/no-movement/interruption schedule, not raw stick input.
- `NavigationControlSolveResult` owns camera/controller realization, and `NavigationValidationResult`
  owns comparison against an unmodified runtime.
- Encounter suppression is a named runtime modification independent of trigger control. Navmesh Survey
  jobs may need to suppress and permit trigger activations dynamically while establishing anchors through
  doors, but the trigger-control mechanism and modes remain research-gated and unresolved.
- Unfinished field-RNG research, including Moonfish behavior, is an unresolved versioned
  model-completeness dependency. This package makes no behavioral claim from that work.

## Current Versus Planned Responsibilities

| Capability | Current repository state | Planned owner |
|---|---|---|
| MLD/SCT loading, geometry, graph, A*, and Qt prototype | Implemented in-memory prototype | `SavorNavigation`, `SavorQt3D` |
| Battle-context-style runtime capture pattern | Existing precedent | `SavorCore`, `SavorWorker`, `SavorDb` |
| Navigation context capture | Not implemented | `SavorCore`, `SavorWorker`, `SavorDb` |
| ISO identity and content extraction | Not implemented for Navigation | Dolphin DiscIO, SPICE/ALX, `SavorNavigation` |
| Runtime patch/write facility | Not implemented | `SavorCore` research and implementation |
| Navmesh Survey and refinement | Not implemented | `SavorNavigation`, `SavorCore`, `SavorWorker` |
| Navigation prediction service | `SavorPredict` is currently battle-focused/exploratory | `SavorPredict` future navigation boundary |
| Local CPU workflow lane | Not implemented | `SavorWorkflow` future executor |
| Durable navigation workflow/artifacts | Not implemented | `SavorDb`, object store, `SavorWorkflow` |
| Product workflow UI | Not implemented | `SavorQt` |

The hidden `dungeon_explorer` workflow placeholder currently has only entry and terminal savestate
bindings. It is a naming and product-integration anchor, not proof that the internal workflow exists.

## Document Map

- [01 - Entry Boundaries and Lifecycle](01-entry-boundaries-and-lifecycle.md)
  - Defines `NavigationEpoch`, reset-qualified entries, same-script interruptions, battle boundaries,
    save-load qualification, and lifecycle outcomes.
- [02 - Disc Content and Provenance](02-disc-content-and-provenance.md)
  - Defines disc identity, extraction manifests, parser ownership, content bundles, hashes, and cache
    invalidation.
- [03 - Navigation Context Contract](03-navigation-context-contract.md)
  - Defines `NavigationContextResult`, readiness evidence, reset-state capture, statuses, and exclusions.
- [04 - Navmesh Survey and World Refinement](04-suppressed-exploration-and-world-refinement.md)
  - Specifies survey anchors, parallel probe waves, passability and movement-response observations,
    trigger-survey direction, coverage, and refinements while separating clean and patched execution.
- [05 - Prediction, Control, and Validation](05-prediction-control-and-validation.md)
  - Defines the planning-level predictor boundary, control realization, validation, and interruption
    handling.
- [06 - Phase, Job, and Artifact Integration](06-phase-job-and-artifact-integration.md)
  - Maps the logical steps, owners, executors, persisted identities, artifacts, and failure behavior onto
    SAVOR.
- [07 - Open Research Questions](07-open-research-questions.md)
  - Records research gates that must not be converted into implementation assumptions.

## Relationship to Other Navigation Specifications

This package is normative for context lifecycle, content provenance, runtime refinement, predictor/control
separation, and workflow lineage. The parent [Navigation planning overview](../README.md) remains the
top-level status and roadmap. [Area Profiles and Analysis Workstreams](../06-area-profiles-and-analysis-workstreams.md)
remains normative for Dungeon, Safe, Overworld, and Unknown/View-only classification and for encounter
geography. Where an older workflow sketch conflicts with this package, this package takes precedence.
In particular, the Navmesh Survey direction in document 04 supersedes fixed job-wide selective-trigger
profiles and collision/anomaly-only survey sketches elsewhere in Navigation planning.

The current target is non-099 Dungeon Navigation. Safe Navigation may later reuse the same 2.5D context,
exploration, control, and validation contracts without encounter work. Area 99/Overworld remains a
separate future design.
