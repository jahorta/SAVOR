# 01 - Scope and Success Criteria

## Status

Future plan. SPICE owns MLD/SCT/ECT and other Skies of Arcadia filetype parsing. The implemented
prototype foundation currently links the vendored SpiceMLD and SpiceSCT libraries privately from
`SavorNavigation`, converts their output into SAVOR-owned navigation models, and owns route-planning
semantics. `SavorQt3D` is the first standalone host for the reusable Navigation widget.
The draft Navigation Context capture and `a101b` export also exist; the Navmesh Survey remains planned.

The current implementation target is **Dungeon Navigation**. Its planned encounter layer will add a
private SpiceEct dependency without changing the public SPICE boundary. A later **Safe Navigation** phase
will reuse the same surface-constrained 2.5D world, routing, collision-validation, and movement-analysis
foundation without random-encounter behavior. Area 99 remains a separate Overworld Navigation problem.
The future runtime/prediction lifecycle is specified normatively in
[`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md); this document remains the scope and
success-criteria summary.

## Problem Statement

Given:
- An AKLZ-compressed MLD selected manually from a GameCube Legends disc dump and, when available, its
  strictly matched SCT and same-key ECT companions.
- A manual start or selectable statically resolvable opcode-77 placement, plus a manually selected ground
  or projected-trigger goal, for the first pathfinding slice.

We need to produce a planning route and schedule that can be realized as an input strategy and reaches the
objective in the fewest VI frames, with acceptable determinism/reliability. `SavorPredict` owns the
planning-level world-space route and movement/no-movement/interruption schedule; a later control solver
owns raw controller and camera realization. For Dungeon Navigation, we must also distinguish the authored
collision model from runtime-validated collision behavior, characterize reproducible movement/collision
anomalies, and
  map route-relative random-encounter exposure without making unsupported probability or runtime-state
  claims. A later `SavorQt` planning module may request a specific modeled outcome from `SavorPredict`, but
  the Navigation map only displays a selected prediction result and never owns movement-schedule search.

For the first prototype, SpiceMLD supplies walkable geometry, collision geometry, link metadata, and
available trigger information from the selected MLD. SpiceSCT parses the related script; `SavorNavigation`
catalogues opcode-77 placements and can use a statically resolvable placement as the start. Manual start
selection remains the default and the goal remains manual, selected from ground or real projected trigger
geometry. Later milestones add automatic condition/state evaluation, opcode-156 runtime restore handling,
broader transition semantics, automatic content resolution beyond the sibling naming rule, workflow jobs,
and durable artifacts.

The existing viewer, start selection, traversal graph, and A* behavior remain the implemented prototype
foundation. Area-profile classification, ECT-backed dungeon encounter analysis, collision validation, and
movement-anomaly discovery described below are planned additions unless explicitly identified otherwise.

## Future Prediction Epoch and Start Scope

This later contract is separate from the implemented Navigation Context `.nctx` that only bootstraps the
Navmesh Survey. Prediction begins from a future ready `NavigationPredictionStart` captured after a
reset-qualified field entry. A qualifying entry is a completed field script/context switch with control
restored, or a battle return with field control restored. Readiness is measured from a clean, unpatched
source savestate: ground and placement settled, player velocity zero, authored input neutral, no forced
movement/cutscene/transition/encounter pending, and the required reset-state capture consistent with the
working contract. Zero velocity and `stepCount` are observed evidence, not values fabricated for the
predictor. Save load qualifies only when runtime evidence confirms a reset-producing script switch.

Entering battle terminates the current `NavigationEpoch`; a validated field return starts a new context.
A cutscene that runs in the same field script and returns control stays inside the current epoch and does
not reset `stepCount`. The predictor must model that interruption in the same search or return
`ModelIncomplete`; it must never restart from a clean-entry assumption. Camera/control orientation is not
part of `NavigationPredictionStart` or predictor input. It remains execution telemetry for later anomaly
probes and a concern of the downstream control solver.

## First Interactive Prototype

1. **Manual file acquisition**
   - Select one `.mld` with a file picker and remember the last directory.
   - For a conforming `aNNNC.mld`, derive area key `NNNC` and look in the same directory for
     `meNNNC.sct`, comparing the prefix-stripped key case-insensitively.
   - Parse the matching sibling automatically. If it is missing, warn and offer a manual SCT picker; a
     manually selected SCT may be elsewhere but must match the current area key.
   - Reject a mismatched SCT before parsing, report the expected and selected names, and preserve any
     already loaded valid SCT. A nonconforming MLD remains viewable but has no automatic SCT association.
   - Use the local US disc dump as a developer fixture, not a hardcoded runtime default.
   - Do not require Dolphin, ISO traversal, or disc-wide area-name resolution.

2. **Direct in-process parsing**
   - `SavorNavigation` reads the compressed bytes and calls SpiceMLD/SpiceSCT.
   - SPICE owns AKLZ detection/decompression and MLD/SCT parsing; SAVOR does not duplicate them.
   - Runtime parsing starts from canonical `MldFile`. A compatibility projection supplies `world`,
     `searchWorld`, and a transient in-memory Blender IR scene used only inside `SavorNavigation` to
     flatten NJ object geometry for exact `fxn=wall` collision regions, every SPICE-classified trigger,
     and exact normalized `motscpt` entries; export remains disabled.
   - A GOBJ contributes navigation geometry only when an MLD entry references its block through
     `groundAddresses`. Object-role-only GOBJ blocks are counted for diagnostics and excluded from the
     navigation surface set.

3. **SAVOR-owned model**
   - Convert parser results into an in-memory `NavigationScenarioModel` containing the
     `NavigationAreaModel`, optional `NavigationScriptModel`, traversal graph, and normalized diagnostics
     before applying data to Qt.
   - Retain the full SCT parse result only behind an internal opaque boundary; expose source/status/
     section/instruction summaries and a SAVOR-owned opcode-77 start catalog rather than SpiceSCT types.
   - Do not expose SpiceMLD or SpiceSCT types to `SavorQt3D`, the widget, the planner, or future
     persistence code.

4. **Standalone widget host**
   - Revive `SavorQt3D` as the development/test application.
   - Retain its existing file picker, Quick 3D renderer, layer visibility controls, and diagnostics where
     useful; the obsolete `SavorMLD` dependency path has been removed.
   - Load and convert the selected file off the UI thread, then apply the completed model on the UI thread.

5. **First interactive path**
   - Build a true-3D traversal graph from valid walkable triangles, shared boundaries, and collision-
     coverage handoffs derived from ordered MLD linked-EntryID fallback chains.
   - Treat all GRND/GOBJ surfaces owned by one entry as its current collision bundle. Continue within that
     bundle wherever it still accepts the movement; only a complete current-bundle miss may transfer to
     the first accepting linked bundle in authored order. Preserve EntryID `0` and missing-entry chain
     truncation instead of treating the link list as an unordered surface-pair set.
   - Allow a handoff where a source boundary continues onto a target triangle footprint even when the
     target's external mesh boundary does not coincide with it. Keep tied stacked heights unresolved.
   - Keep manual ground picking as the default for the start. Let the goal select either ground or a real
     projected trigger mesh, then resolve both endpoints to stable graph triangles and render one
     deterministic A* route plus its derived portals.
   - For a trigger goal, ignore fallback cubes, choose the largest usable projected mesh by deterministic
     world-space bounds, resolve it to walkable graph geometry, and render those bounds as a magenta AABB.
   - Offer statically readable opcode-77 placements as optional starts. Classify initialization placements
     as authored arrivals and other occurrences as scripted repositions while preserving their branch,
     switch, condition, and call provenance. Use known opcode-77 yaw as the scripted start facing; manual
     starts expose a facing control and a short rendered facing ray.
   - Ignore opcode 156; do not infer its coordinates from unresolved runtime restore state or automatically
     choose an opcode-77 variant from current game state.

## Area Profile Scope

`NavigationAreaProfile` is SAVOR-owned and is derived from the normalized area key plus the complete
same-directory companion-file set. Classification is ordered; later rules never override an earlier one:

1. **Overworld**
   - Any key beginning with `099` is Overworld regardless of SCT or ECT presence. Area 99 never falls
     through to Dungeon handling.
2. **Dungeon**
   - A non-099 MLD with a case-insensitively matched, same-key ECT is Dungeon.
   - ECT presence establishes the profile. Readability, decompression, and parse status separately
     determine encounter-data availability and never reclassify the area.
3. **Safe**
   - A non-099 MLD with a matched SCT and no matched ECT is a Safe Navigation candidate when the selected
     directory is treated as the complete companion set.
   - Keys below `200a` have evidence of traversability. Keys at or above `200a` remain candidates whose
     traversability has not yet been validated.
   - Safe means no random encounters; it does not mean no scripted triggers, events, cutscenes, doors, or
     other interruptions.
4. **Unknown / View-only**
   - Nonconforming names and MLDs with neither a matched SCT nor ECT remain inspectable without a
     navigation-phase guarantee.

For a conforming `aNNNC.mld`, the expected companions are `meNNNC.sct` and `aNNNC.ect`. Matching is
case-insensitive while original paths remain provenance. Automatic discovery stays limited to the MLD
directory. The current target implements Dungeon Navigation; Safe is a later sibling profile, and
Overworld/Area 99 is deferred.

## In-Scope (MVP)

1. **Dungeon-first static world navigation**
   - Consume walking-plane and target-discovery data exposed through `NavigationAreaModel`.
   - Include both native GRND meshes and GOBJ meshes used in the ground role, while preserving their
     source kind and entry/block/node identity.
   - Represent non-walkable obstacles and trigger volumes in SAVOR navigation types, including their
     attached projected object meshes when available.
   - Preserve exact `motscpt` entries as provisional `MovingObject` regions for inspection without yet
     asserting that every entry is a door or modeling its runtime motion/controller behavior.

2. **Objective-based routing**
   - First pathfinding slice: route from a manual or resolvable opcode-77 start to a manually picked ground
     or projected-trigger goal resolved onto the loaded walkable geometry.
   - Later objective workflow: route between named objectives:
     - chest/interaction
     - doorway/zone transition
     - map exit/load trigger

3. **Cutscene-aware progression**
   - Detect and model same-script cutscene interruptions without creating a new prediction epoch.
   - Continue from the post-cutscene state in the same prediction when supported; otherwise return
     `ModelIncomplete`.
   - Treat battle entry as epoch termination and battle return as a separate reset-qualified context.

4. **Time-optimal baseline**
   - Optimize for completion time in VI frames, but keep optimization strategy simple in MVP.

5. **Simulator-backed route execution**
   - Produce a route/spline and solve for executable inputs in workers.

6. **UI-first workflow**
   - Navigation phase is UI-driven (no CLI workflow for objective specification in MVP).

7. **Shared Navmesh Survey and collision validation**
   - Bootstrap every worker from the same explicitly named Navigation Context `.sav`/`.nctx`.
   - In a first wave, use disposable jobs to cross required in-area doors and publish durable positional
     anchors only after clean-baseline teleport-and-settle replay succeeds.
   - In a second wave, reload the common baseline, teleport parallel workers to verified anchors, and
     collect spatial passability observations.
   - Compare extracted GRND/GOBJ and wall geometry with runtime contact and response evidence without
     silently rebaking or rewriting source geometry.
   - Track geometry-conversion completeness separately from runtime validation coverage and confidence.
   - Normally suppress trigger commit, briefly restore it only for an intended door interaction, verify
     the expected TBLID and physical crossing, and immediately suppress it again.
   - Permit an isolated job to override a known door-lock BitVar while recording the original value,
     required value, and `initially_locked` portal metadata.
   - Reuse the same observation and confidence model for the later Safe Navigation phase.

8. **Later shared movement-response and collision-oddity discovery**
   - Keep this timing-dependent analysis separate from the spatial Navmesh Survey.
   - Probe collision corners, seams, slopes, and boundary interactions for reproducible movement changes.
   - Record the approach pose, facing, camera/input context, observed speed or displacement change,
     VI-frame cost, repetition, variance, and whether the result is beneficial, neutral, harmful, or
     unresolved.
   - Detect sticky-corner holds followed by possible positional jumps and wall-contact ramp-speed behavior,
     including the staircase case where angled wall contact may outperform ordinary ascent.
   - Do not assume that every unusual or sticky collision response is a useful speedup.
   - Supply measured movement-response and reproducibility data to later optimization without choosing or
     ranking movement techniques in the survey itself.

9. **Dungeon encounter analysis**
   - Parse a matched ECT privately through SpiceEct and convert it to SAVOR-owned encounter data.
   - Associate authored encounter selectors with stable walkable triangles by `NavigationTriangleKey`.
   - Keep encounter analysis optional: ordinary route planning remains available when the present
     Dungeon-classifying ECT is unreadable, undecompressible, malformed, partial, or unsupported.
   - Present static selector/table structure and route-relative exposure at an evidence level justified by
     the supplied encounter state and runtime validation. Do not change geometric A* costs in the first
     encounter-analysis slice.

10. **Predictor-backed outcome planning (later `SavorQt` slice)**
    - Add a planning module outside the Navigation widget that invokes the `SavorPredict` predictor
      subsystem through a future asynchronous boundary.
    - Search paths and movement/no-movement/interruption schedules for an authored objective such as no
      encounter or a specific encounter, using an explicit `NavigationPredictionStart`, content/world,
      model, and search-bound provenance.
    - Return an immutable selected-result contract containing a witness path/schedule plus either a
      route-prefix cutoff or a `NavigationTriangleKey`-keyed 2.5D reachable set and frontier.
    - Let the Navigation widget validate and render that result without rerunning, extending, or ranking
      the predictor search.

## Non-Goals (MVP)

- Full global route planning across multiple maps with long-term resource constraints.
- Dolphin/ISO-backed MLD discovery or automatic area-ID lookup in the first prototype.
- A SAVOR-side MLD parser or AKLZ decompressor.
- Automatic SCT condition/game-state evaluation, opcode-156 runtime restoration, incoming-transition
  execution, controller activation semantics, or filename inference beyond the strict same-directory
  `aNNNC.mld` -> `meNNNC.sct` association.
- Durable `nav_world_blob` persistence or a required serialized SPICE area-view artifact in the first
  prototype.
- Blender IR as a runtime data contract, persisted artifact, or Qt-facing type. A transient internal
  projection is permitted solely to convert NJ object geometry into SAVOR-owned meshes.
- Implementing the separate Safe Navigation workflow; this milestone documents its reuse boundary only.
- Combat strategy co-optimization or expected battle-time/risk-weighted route selection.
- Area-99/Overworld movement, altitude control, contextual encounter lookup, and RNG semantics in the
  Dungeon milestone.
- Treating every non-099 MLD as a validated Dungeon or Safe navigation area merely because its filename
  conforms. Companion evidence and Safe traversability evidence remain explicit.
- Automatic inference of encounter step counters, suppressor state, eligible-check cadence, or shared RNG
  state from a spatial start point alone.
- Running `SavorPredict`, enumerating movement schedules, or deriving a predictor frontier inside the
  Navigation widget or `SavorQt3D` prototype host.
- Treating a manual/opcode-77 spatial start, arbitrary savestate, or same-script cutscene return as a
  reset-qualified predictor context.
- Using a patched exploration state as a prediction start or clean validation input.
- Treating the fraction of sampled/tested schedules that reach a point as natural encounter probability.
  Predictor reachability is objective- and search-bound-qualified; the static selector/table and marginal
  exposure layers remain separate.
- Treating the current exploratory `SavorPredict` executable/CLI as an already-stable Qt-linkable API. Its
  navigation prediction surface and asynchronous invocation boundary remain future design work.
- Heavy optimization/meta-optimization for search budgets in first pass.
- Mandatory repeat-run validation gates in first pass (add if deterministic assumptions fail in practice).

## Success Criteria (MVP)

1. **Functional completion**
   - Can produce a successful route between at least two objective pairs in one dungeon.

2. **Cutscene continuity**
   - If a mandatory same-script cutscene triggers, the planner continues within the same epoch and still
     reaches the objective, or returns `ModelIncomplete` without inventing a reset. Battle entry closes the
     epoch and any post-battle continuation explicitly references a newly captured context.

3. **Performance target**
   - Planning + execution pipeline completes within an acceptable offline budget (draft target: < 10 minutes per objective pair on dev hardware).

4. **Quality target**
   - Beats a hand-authored baseline route in at least one benchmark objective pair.

5. **World-model visibility**
   - `SavorQt3D` can render a `SavorNavigation` 3D world model, available walking planes, potential
     targets, and selected path for inspection.

6. **Parser boundary**
   - A selected AKLZ-compressed US field MLD loads without pre-decompression, and no SpiceMLD type crosses
     the `SavorNavigation` public boundary.

7. **First-slice fixture contract**
   - `a101b.mld` produces 6 GRND surfaces and 6 ground-role GOBJ surfaces (504 vertices and 401 triangles
     total), plus 59 collision entries, 16 trigger entries, 11 provisional `motscpt` MovingObjects, and
     21 remaining unknown entries.
   - Its 51 exact `fxn=wall` regions produce 517 SAVOR-owned mesh instances, 6,795 vertices, and 8,347
     triangles spanning 18 source object addresses, with no failed wall regions.
   - Its 16 trigger regions all project successfully: 11 `goscript` entries produce 11 meshes/88 vertices/
     132 triangles, 4 `treasure` entries produce 20 meshes/144 vertices/168 triangles, and 1 `wallmot`
     entry produces 7 meshes/88 vertices/84 triangles (38 meshes, 320 vertices, and 384 triangles total).
   - Its 11 exact `motscpt` entries produce 55 meshes, 462 vertices, and 566 triangles with no failed
     MovingObject regions.
   - Object-role-only GOBJ blocks do not appear as navigation surfaces.

8. **Matched SCT loading**
   - Opening `a101b.mld` automatically attempts sibling `me101b.sct`; a missing sibling produces a
     recoverable warning and manual-load affordance.
   - A matching SCT parses asynchronously through SpiceSCT and exposes normalized section/instruction
     summaries plus a SAVOR-owned opcode-77 start catalog without exposing SpiceSCT types.
   - A mismatched SCT is rejected without replacing a currently valid association, while parse failures
     leave the MLD and manual pathfinding usable.
   - Fixture-backed parsing preserves `me101b.sct` as 51 sections with one exact `loop` containing 20
     instructions and `me201a.sct` as 115 sections with one exact `loop` containing 20 instructions.

9. **Selectable opcode-77 starts**
   - `me101b.sct` yields the save-load ground-5 placement at `(23.5, 64, 146.5)`, the normal ground-0
     fallback at `(-64, 16, -86)`, and the `NYUJO_EVENT` ground-0 scripted reposition at
     `(-32, 16, -104)`, with their yaw and condition/call provenance retained.
   - Only finite constant ground/XYZ placements can be resolved. Incomplete options remain inspectable but
     unavailable; opcode 156 produces no option.
   - The toolbar defaults to `Manual point`; a resolvable option anchors to a unique nearest triangle on
     its matching ground `tblId`, while ambiguous or failed resolution preserves the existing valid start.
     Known opcode-77 yaw supplies the selected start's facing.

10. **Initial pathfinding**
   - Complete ground/wall geometry produces a stable triangle/portal traversal graph.
   - Ground handoffs preserve ordered EntryID fallback provenance and implement current-bundle-first,
     first-accepting-target collision coverage. The `a101b` lower GRND, ground-role GOBJ staircase, and
     upper GRND are connected without requiring coincident external mesh boundaries.
   - Static A* excludes motion-bearing or non-`ground` entries. Their bind-pose geometry and conditional
     handoff candidates remain inspectable as runtime-dependent evidence.
   - A manual or resolved opcode-77 start and a manually picked ground or projected-trigger goal can be
     connected by a deterministic A* route, or produce a visible and specific invalid/unreachable
     diagnostic.
   - Compact fixed-size start and ground-goal markers match the `man` fallback scale. A short start-facing
     ray and a selected trigger goal's magenta world-space AABB are inspectable with derived portals and
     route geometry in `SavorQt3D`.

11. **Deterministic area profiling**
   - Area 99 is always classified Overworld before companion-file rules are considered.
   - A non-099 matched ECT classifies an area as Dungeon even when ECT parsing fails; a matched SCT with no
     ECT yields Safe only when the directory is accepted as the complete companion set.
   - A below-`200a` Safe area retains known-traversable evidence, while an at-or-above-`200a` Safe area is
     visibly marked candidate/unverified.

12. **Collision-validation evidence**
   - The survey starts every job from the same named Navigation Context bootstrap and records every
     positional anchor, teleport/settle check, door crossing, runtime modification, and BitVar override.
   - Runtime observations can be traced to the tested source surface or wall boundary, probe position,
     approach, requested position, resulting position, and settled position.
   - Validation coverage and discrepancies are visible independently of
     `hasCompleteGroundGeometry`/`hasCompleteWallGeometry`; geometry completeness never masquerades as
     runtime collision confidence.
   - Door evidence retains the expected TBLID, physical crossing, far-side anchor, and initial-lock
     constraint. Broader trigger-envelope characterization is a separate later workstream.

13. **Later movement-anomaly evidence**
   - Candidate anomalies retain enough input, camera, timing, baseline, and repeated-run evidence to
     reproduce and classify their effect.
   - Sticky-corner candidates preserve the held interval and any later positional discontinuity; ramp-speed
     candidates preserve ordinary and wall-contact ascent baselines, vertical gain, and net progress.
   - Only measured, reproducible positive results may later become route actions or edge-cost changes;
     neutral, harmful, unresolved, or divergent results remain diagnostic evidence.

14. **Optional Dungeon encounter model**
   - Each selector interpreted under the current authored-field hypothesis is attached to its source
     triangle by `NavigationTriangleKey`, and both GRND and ground-role GOBJ triangles participate.
   - The model preserves unknown metadata, unsupported selectors, unresolved tables, overlapping-resource
     ambiguity, and ECT diagnostics rather than coercing them to no encounter.
   - Failure to read, decompress, or parse a present Dungeon-classifying ECT disables table-backed
     encounter results but preserves Dungeon classification and ordinary manual/scripted-start
     pathfinding. ECT absence instead participates in Safe or Unknown/View-only classification.

15. **Evidence-qualified output**
   - Static encounter structure, assumed/marginal exposure, complete-model predicted/seeded outcomes, and
     simulator-observed/validated outcomes are visibly distinct; absent encounter state or check cadence
     cannot produce a stronger claim.
   - Worker timeout, divergence, or missing telemetry produces no empirical collision, anomaly, or
     encounter claim and never overwrites lower-level structural evidence.

16. **Reset-qualified prediction input**
   - Every prediction explicitly references one future ready `NavigationPredictionStart`; it does not
     reinterpret the Survey-bootstrap `.nctx` as predictor state.
   - Readiness proves measured zero velocity, stable control/ground placement, neutral queued input, and a
     qualifying transition boundary. `NonResetContinuation`, `VelocityNonZero`, `ControlNotStable`,
     `ResetStateMismatch`, and `IncompleteCapture` remain explicit unavailable outcomes.

## Success and Failure Semantics

- An unusable MLD or failed SAVOR world conversion is a scenario-load failure. Usable partial geometry
  remains viewable, but pathfinding stays disabled until required ground and wall geometry are complete.
- SCT absence or failure preserves geometry inspection and manual endpoints while marking scripted starts
  and transition information incomplete.
- ECT **association/presence** and ECT **load/parse status** are independent. A matched ECT keeps a
  non-099 area Dungeon even if ECT I/O, AKLZ decompression, or parsing fails; only encounter-backed results
  become unavailable.
- Missing ECT is evidence for Safe classification only for a complete companion directory. It is not by
  itself proof of traversability; below-`200a` and at-or-above-`200a` evidence levels remain distinct.
- Geometry completeness reports successful source conversion. Runtime collision validation coverage and
  confidence are separate and may remain unknown, partial, or contradicted for otherwise complete
  geometry.
- Unsupported encounter selectors, missing triangle metadata, unresolved active collision resources,
  unknown step/check state, and worker failures remain explicit incomplete outcomes. None silently mean
  no encounter or successful validation.

## Deliverables

- Pinned SPICE submodule integration and a `SavorNavigation` adapter contract for navigation-relevant
  world data, a selectable opcode-77 SCT start catalog, and planned private ECT conversion.
- Reusable Navigation widget running in the standalone `SavorQt3D` host.
- SAVOR-owned area-profile/companion evidence plus Navmesh Survey positional-anchor, spatial
  collision-validation, door/access-constraint, coverage, and immutable refinement models.
- Separate later movement-response/oddity and generalized trigger-activation models.
- Optional Dungeon encounter model keyed by `NavigationTriangleKey`, with structural and assumed/marginal
  evidence; separate predictor-result and simulator-validation artifacts keep predicted/seeded and
  observed/validated evidence distinct.
- Future predictor-result contract and map projection for objective-qualified route prefixes or 2.5D
  reachability frontiers, with immutable context/content/world/model/search provenance and witness
  references.
- Future `NavigationPredictionStart`, disc/content bundle, patched exploration/refinement,
  planning result, control-solve result, and clean-validation contracts with explicit artifact lineage.
- Planner/refiner artifact format for routes and candidate telemetry.
- Phase integration contract (job payload/result schema and step kinds).
- 3D world-model viewer and path overlay support in UI.
- Benchmark set + reporting template.

## Acceptance Benchmarks (Draft)

- Benchmark A: simple straight traversal with 0 cutscenes.
- Benchmark B: traversal requiring 1 mandatory interaction cutscene.
- Benchmark C: traversal with overlapping walk mesh elevation and tight collision corners.
- Benchmark D: load the concrete `a101b` Navigation Context bootstrap, apply encounter and trigger
  suppression, briefly enable door `4101`, record and override its initial BitVar lock when required,
  prove opening and physical crossing, replay the far-side positional anchor from the common baseline,
  and run parallel spatial probes from both sides.

## Risks

- Geometry mismatch between extracted data and runtime collision behavior.
- A complete mesh conversion can still disagree with runtime collision. Geometry completeness and runtime
  validation confidence must remain separate signals throughout planning and UI.
- Incorrect coordinate conversion, matrix interpretation, or triangle winding. The first prototype must
  calibrate a centralized `SavorNavigation` coordinate policy against known areas instead of distributing
  renderer-specific fixes.
- Attached trigger meshes may be selected as route goals, but their bounds remain geometry visualization;
  trigger/script coupling, player interaction radius, and SCT/controller activation predicates are not yet
  represented.
- The brief first-slice trigger-enable window is global rather than intrinsically TBLID-selective. Every
  door job must verify the selected object and reject a wrong-target activation before publishing an
  anchor.
- Opcode-77 classification distinguishes likely authored arrivals from lower-confidence scripted
  repositions, but does not establish which option current runtime state selects; automatic evaluation
  remains an explicit later slice.
- Moving platform/controller rules requiring a second modeling pass.
- The first collision-coverage graph does not yet reproduce wall blocking, decoded triangle flags,
  player step-up limits, animation/script-driven surface state, or every runtime collision-selector mode.
  Derived handoffs remain static candidate topology until those rules and simulator evidence refine them.
- `motscpt` may include doors, but entry meaning and motion/activation behavior require controller/SCT
  evidence before becoming navigation semantics.
- ECT presence classifies Dungeon content, but parser success does not validate selector semantics,
  encounter cadence, suppressors, or RNG state. Encounter claims must remain evidence-qualified.
- A predictor frontier can be stale, spatially incompatible, model-incomplete, or truncated by its search
  budget/horizon. None of those states is a universal gameplay boundary, and the widget must not present a
  sampled schedule frequency as encounter probability.
- The below-`200a` Safe rule is evidence-backed scope guidance, not a proof that every later Safe candidate
  is traversable.
