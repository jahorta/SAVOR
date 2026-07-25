# 03 - Search and Optimization Strategy

## Status

Mixed status. The in-memory graph, endpoints, opcode-77 starts, and deterministic A* are implemented;
the draft Navigation Context capture also exists. The Navmesh Survey, shared analysis workstreams,
spline/control solver, and workflow integration remain future. This
document assumes `SavorNavigation` has converted canonical SpiceMLD data, projected
world/search evidence, and transiently flattened wall, trigger, and provisional MovingObject geometry into
an in-memory `NavigationAreaModel`. A filename-matched SCT may also be parsed and retained in the enclosing
`NavigationScenarioModel`; its statically resolvable opcode-77 placements may provide an optional start,
but never a goal or search edge. Route search and Qt code consume only SAVOR-owned types; they do not
consume SpiceMLD, SpiceSCT, SpiceEct, Blender types, or a serialized SPICE area-view artifact.

The lifecycle, content, exploration/refinement, prediction, control, and validation pipeline is normative
in [`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md). This document summarizes how the
current graph/search work and profile analyses participate in it.

## High-Level Strategy

Use a shared analysis and execution pipeline:

1. **Navigation Context capture** producing the explicit `.nctx` plus matching savestate used as the
   Navmesh Survey's initial position and common baseline.
2. **Layer-preserving 2.5D analysis** over versioned content, walkable surfaces, collision evidence, and
   triangle metadata.
3. **Navmesh Survey and world refinement** using one named Navigation Context `.sav`/`.nctx`, a finite
   door-anchor establishment wave, clean-baseline teleport-and-settle fan-out, and immutable spatial
   evidence.
4. **Route planner** over the authoritative 3D graph/navmesh abstractions.
5. **Dungeon-encounter analysis** as a separate profile-specific overlay over the shared 2.5D products.
6. **Later prediction-start capture and outcome search** using a future `NavigationPredictionStart` and a
   separate `SavorQt`/`SavorPredict` boundary.
7. **Control solver** that follows route splines and finds executable player+camera inputs in simulator
   workers, followed by validation from an unpatched clean runtime.

MVP prioritizes correctness and continuity over advanced optimization.

The earlier geometry slice stopped after loading and visualizing one manually selected MLD. The implemented
pathfinding slice retains the identity coordinate policy, loads the strictly matched SCT, builds a
condition-preserving opcode-77 start catalog, derives traversable adjacency, supports manual or catalogued
start selection plus a manual ground or projected-trigger goal, and renders one deterministic A* route.
Worker-backed control solving and persisted route artifacts follow after the model and widget boundary
are validated.

## Shared 2.5D Analysis Pipeline

The shared analysis pipeline samples or partitions each walkable layer in X/Z while retaining the resolved
Y position, source surface and triangle identity, normal, GRND/GOBJ provenance, collision evidence, and raw
triangle metadata. It is **2.5D per layer**, not a global 2D flattening:

- Vertically stacked surfaces remain distinct unless an explicit validated graph portal joins them.
- The existing true-3D traversal graph remains authoritative for anchoring and A*.
- The same layer-aware products feed static collision inspection, anomaly detection, and dungeon encounter
  overlays so those workstreams do not build incompatible spatial maps.
- The sampling resolution and any later adaptive refinement are analysis configuration, not baked into the
  source `NavigationAreaModel` or persisted as parser-owned geometry.

This deterministic construction belongs to the CPU-domain `SavorNavigation` layer. Simulator workers may
later attach observed samples to the same stable surface/triangle identities; they do not own the static
field construction.

## Stage A: Route Search (MVP)

## Baseline algorithm
- `NavigationGraphBuilder` creates one stable graph node per valid walkable triangle, retaining its source
  surface/triangle key, centroid, and normal. Surfaces requiring runtime animation or script state remain
  visible but are excluded from active nodes.
- Create bidirectional intramesh edges across shared or tolerance-welded boundary segments.
- For each external source-triangle boundary, partition its projected span at intersections with current
  and candidate target triangle footprints, then probe just outside each interval.
- Query all other surfaces owned by the current MLD entry first. If they provide unique height-continuous
  coverage, create a same-entry handoff and do not consult linked entries for that interval.
- On a complete current-entry miss, query linked EntryID bundles in authored order and create a directed
  fallback handoff to the first accepting bundle. Preserve EntryID `0`; a missing EntryID truncates the
  effective chain, while missing geometry remains diagnostic evidence and permits consideration of later
  authored targets.
- Choose the height-nearest accepting triangle. Use stable triangle-key ordering for coincident duplicates,
  but leave effectively tied stacked heights unresolved. A discontinuous first hit does not fall through
  to a later target.
- Merge adjacent intervals only when their source/target triangles and fallback provenance match. Derive a
  reverse edge only from the reverse entry's independently validated coverage.
- Diagnose and omit malformed, degenerate, or non-manifold triangles from the search graph.
- Run deterministic A* over this true-3D graph with a stable node-key tie break.
- Use 3D Euclidean distance as the admissible heuristic. Edge cost follows centroid -> portal -> centroid
  geometry plus a nonnegative coarse slope multiplier derived from the centralized up axis.
- Reject models where either `hasCompleteGroundGeometry` or `hasCompleteWallGeometry` is false; partial
  models are diagnostic visualization inputs, not valid search worlds.
- Treat `hasCompleteTriggerGeometry` as a visualization diagnostic rather than a search-readiness gate in
  this prototype. Attached trigger meshes do not yet encode SCT/controller activation semantics.
- Treat `hasCompleteMovingObjectGeometry` the same way. `motscpt` mesh presence alone does not establish
  door identity, animation state, collision behavior, or traversability.
- Treat exact normalized `ground` entries without nonzero motion resources as statically traversable.
  Retain motion-bearing or non-`ground` bind-pose handoff candidates as runtime-dependent diagnostics, but
  do not let A* cross them until their state is modeled.

## Edge costs (MVP)
- 3D geometric traversal distance through the shared boundary or cross-surface portal
- nonnegative coarse slope modifier

`cost_first_path = geometric_distance * slope_multiplier`

Interaction, cutscene duration, SCT/controller state, moving-object state, frame-time calibration, heavy
variance, and risk-weighted routing are deferred. Dungeon encounter analysis is an overlay in its first
slice and does not alter A* edge costs or the selected route.

## Candidate strategy
- Produce one deterministic route for the active slice.
- `NavigationGraphAnchor` records the picked surface/triangle, exact point, snapped point, and snap
  distance. Only complete walkable ground geometry can be anchored.
- `NavigationStartResolver` converts a selected available opcode-77 placement through the centralized
  coordinate policy, matches its ground selector against surface `tblId`, and chooses the unique nearest
  matching triangle. It reports effectively tied stacked surfaces as ambiguous instead of guessing.
- `NavigationPathResult` distinguishes invalid endpoints, incomplete geometry, unreachable goals, and a
  successful route. A same-triangle query remains a valid direct route.
- Return a SAVOR-owned polyline from exact/snapped start through portal midpoints to exact/snapped goal,
  plus exact selected graph-edge indices, route cost, geometric length, and diagnostics. Retaining the
  selected edge disambiguates collision-handoff provenance when parallel edges join one triangle pair.
- Funnel/string-pulling, splines, top-K alternatives, and simulator-time ranking are later refinements.

## Interactive endpoint selection and overlays

- The Path toolbar exposes a grouped `Start:` selector whose first/default item is `Manual point`.
  Resolvable authored arrivals and scripted repositions set the existing start anchor; incomplete entries
  remain visible but disabled with an explanation.
- `SavorQt3D` also exposes mutually exclusive `Set Start` and `Set Goal` modes. A short left click on ground
  sets a manual start or ground goal; Set Goal also accepts a real projected trigger mesh. Pointer movement
  beyond the click threshold retains orbit behavior. A successful manual start pick returns the selector
  to `Manual point`.
- Each rendered ground surface carries a stable surface identifier. Qt reports that identifier and the
  scene-space hit point; C++ anchors it to a triangle rather than relying on a Qt triangle index.
- Projected trigger meshes are goal-only. Trigger fallback cubes, walls, MovingObjects, markers, links, and
  the route overlay are not endpoint sources.
- `NavigationTriggerGoalResolver` chooses the largest usable projected mesh by deterministic world-space
  AABB size, resolves its nearest unambiguous point to walkable graph geometry, and retains the selected
  bounds. This produces a route endpoint, not an inferred activation predicate or interaction radius.
- Populate the existing Links layer with derived collision handoffs: active static handoffs are yellow and
  runtime-dependent bind-pose candidates are muted orange. Add a Route layer with a thick route line and
  compact fixed-size start/ground-goal markers matching the `man` fallback scale. Render manual or known
  opcode-77 start facing as a short green ray; render a selected trigger goal as a magenta AABB. Recompute
  whenever either endpoint changes.
- Selecting a resolved catalog start preserves the goal and recomputes the route. A failed resolution
  preserves the previous valid start. Loading a new MLD resets the selection; a rejected SCT replacement
  preserves the existing catalog and selection.
- Show graph node/edge/portal counts, endpoint snap distances, route length/cost, and a specific
  unreachable/invalid reason in diagnostics.

## SCT start-catalog boundary for this slice

- Loading `aNNNC.mld` attempts to parse sibling `meNNNC.sct`; a valid manual replacement must have the
  same prefix-stripped key.
- Report SCT source, match/load status, section count, exact `loop` count, instruction count, and parser
  diagnostics.
- Catalog every opcode-77 occurrence with condition and call provenance. Initialization-path placements are
  authored arrivals; other occurrences are lower-confidence scripted repositions. Only constant finite
  ground/XYZ values are eligible for graph resolution.
- Manual start remains the default and goal selection remains manual from ground or real projected trigger
  geometry. SAVOR does not evaluate current game state to choose a variant automatically.
- Ignore opcode 156 entirely until the runtime source and lifetime of its saved transform are modeled.

## Navmesh Survey and collision workstream

The Navmesh Survey supplies one runtime-refined navmesh per loaded area. Its persistent evidence is
spatial and does not perform movement optimization or infer timing:

1. Load the explicitly named Navigation Context `.sav`/`.nctx` as the common bootstrap.
2. Run an anchor-establishment wave from that position. Normally suppress triggers; briefly restore the
   exact callsite instruction for an intended door interaction, immediately suppress again, and verify
   expected TBLID, opening, crossing, and far-side settle.
3. When a door is BitVar-locked, change only that bit in the disposable job and retain the original value,
   override, and `initially_locked` constraint on the portal.
4. Reload the common bootstrap, teleport to each proposed position, let normal game updates reconstruct
   ground/collision state, and publish only anchors that settle usefully.
5. Run a second parallel wave from the verified anchors to probe walls, edges, ramps, stairs, handoffs,
   uncertain portals, and coverage gaps.
6. Reproduce important or contradictory spatial evidence and deterministically build an immutable
   refinement.

Area loads are separate survey files. Static diagnostics do not silently rewrite the graph; a correction
affects routing only through a specific, evidence-backed refinement. Timing-dependent collision-oddity
studies and generalized trigger-envelope characterization remain separate later workstreams.

## Dungeon encounter analysis workstream

The first encounter slice is deliberately dungeon-only and analysis-only:

- For a Dungeon-classified `aNNNC.mld`, parse the same-directory, same-stem `aNNNC.ect` whose presence
  established that profile. There is no manual ECT override. SpiceEct owns AKLZ and flat-table parsing;
  only normalized SAVOR-owned encounter tables and diagnostics leave `SavorNavigation`.
- Apply the current authored-selector hypothesis to GRND and ground-role GOBJ triangle metadata. Preserve
  confidence in that interpretation separately from confidence in active runtime resource/triangle
  selection; parser success alone proves neither. The ground-entry `tblId` remains collision-resource
  routing metadata and is not an encounter table ID.
- Treat selector zero, missing metadata, unsupported selector values, a missing ECT table, zero stage,
  and zero overall rate as distinct states. Preserve the 1-based selector-to-flat-table relationship and
  the authored row order/weights.
- Render the whole-area selector/table field independently of the selected start. When a route exists,
  segment that current deterministic route by encounter state and report active/no-encounter geometric
  distance plus table transitions. Do not invent a whole-map cumulative-risk value for destinations that
  can be reached by different paths.
- Marginal or seeded analysis requires an explicitly selected future `NavigationPredictionStart`. It records
  a reset-qualified entry, measured `stepCount` and zero velocity, stable control/ground placement, and
  independently captured encounter fields. A manual or opcode-77 spatial start does not prove any of
  those values, and the workflow never fabricates them to create a fresh-entry assumption.
- An exact recovered per-active-check probability may be reported for a known check state. A cumulative
  route value that multiplies marginal survival probabilities is an independence approximation, not an
  exact seeded TAS result. Formation selection remains a separate sequential RNG process and malformed or
  ineligible row pools are never silently normalized.
- Exact check cadence and shared-RNG behavior require captured evidence. A complete versioned
  `SavorPredict` model
  may later produce predicted/seeded encounter and formation results; Dolphin replay produces the separate
  observed/validated evidence level. Geometric distance is not converted to VI frames or encounter checks
  without calibration.
- Encounter results do not alter A*, route ranking, trigger semantics, or control solving in this slice.

Safe/no-encounter areas reuse the same 2.5D, graph, collision, anomaly, route, and control pipeline with an
encounter status of not applicable; absence of encounter data is expected rather than a load failure. Area
99 is explicitly deferred because its selector is a local lane that also requires position buckets,
altitude, scenario page, `fldEfcontrol` data, encounter-zone resolution, and runtime table-set context.

## Predictor-Backed Outcome Planning (Later `SavorQt` Slice)

The static selector/table overlay and marginal route exposure do not search movement schedules. A separate
future planning module in `SavorQt` authors an objective, selects a compatible navigation world and
explicit ready `NavigationPredictionStart`, invokes the existing `SavorPredict` subsystem through a future asynchronous boundary, compares
candidate results, and chooses one for display. The current exploratory `SavorPredict` executable/CLI must
gain an explicit navigation prediction surface; the Navigation widget must not link to its internals or run
the search itself.

Initial Dungeon objectives are:

- reach as far as possible without a random encounter; and
- obtain a specified encounter or formation when the modeled evidence supports that distinction.

`SavorPredict` owns planning-level temporal field/RNG/movement-state evolution, branching over candidate
paths and movement/no-movement/interruption choices, objective evaluation, witness selection, and frontier
computation. Its model bundle must expose completeness for every required field RNG source; unfinished
Moonfish/field analysis is not a source of behavioral assumptions. A* may supply ordinary geometric
candidates, but the predictor may select a different witness path because temporal outcome is part of its
explicit objective. This does not retroactively change the static A* result or its edge costs.

The predictor returns an immutable, bounds-qualified result rather than a map-side schedule. A fixed-path
result contains a reachable route prefix and cutoff. A branching search contains a layer-preserving set of
reachable triangle/local-coordinate states and one or more frontier edges keyed by
`NavigationTriangleKey`. The result also carries its objective, start/world/model fingerprints, search
bounds and completeness, terminal reason, selected witness path, complete
movement/no-movement/interruption schedule reference, trace reference, and predicted encounter result. Raw controller and
camera realization belongs to the later `NavigationControlSolveResult`, not to the prediction contract.

The reusable Navigation widget only validates and renders the selected result's spatial projection. It
keeps the whole-area static encounter layer separate and never treats sampled-search frequency as natural
probability. Spatially incompatible results are rejected; matching geometry with older start/model/objective
context may be shown as historical/stale; missing fingerprints are unverifiable. A search-budget,
prediction-horizon, schedule-exhaustion, event-interruption, or model-incomplete frontier remains labeled by
that terminal reason and is not presented as a universal gameplay boundary.

One prediction spans one `NavigationEpoch`. A same-script cutscene that returns control stays in that
epoch and does not reset `stepCount`; the predictor models the interruption or returns `ModelIncomplete`.
Entering battle terminates the epoch. A post-battle plan must reference a separately captured context after
the reset-qualified field return, never an implicit continuation or a clean-entry guess.

## Stage B: Route-to-Spline

- Convert route nodes/edges into a followable spline/path representation.
- Mark interaction waypoints and cutscene/transition boundaries.
- Persist checkpoints for downstream workers once workflow integration begins; the interactive prototype
  may keep its selected path in memory.

## Stage C: Control Solver (Simulator Workers)

Given spline segments:
- Iterate on camera+player inputs to follow spline.
- Use savestate checkpoints at segment boundaries.
- Emit best successful candidate per segment.
- When a selected predictor result supplies a witness movement schedule, preserve its timing constraints
  while translating or validating it as executable controller input rather than asking the widget to
  reproduce the predictor search.

Planned VM support (new instructions):
- Load route spline into VM context.
- Attempt iterative spline-following policy.
- Return telemetry (deviation, frames, collision/anomaly observations, trigger hit status, and encounter
  observations when the selected explorer profile supports them).

Static world adaptation, 2.5D construction, graph search, route segmentation, and marginal dungeon
encounter analysis remain CPU-domain work. Only operations that require Dolphin state, frame stepping,
controller input, runtime collision behavior, or observed shared-RNG ordering belong in simulator workers.
Modeled seeded outcome search belongs to the future `SavorPredict` execution boundary; it is neither a
`SavorNavigation` static-analysis operation nor a Navigation-widget responsibility.

## Stage D: Cutscene Handling

When an interruption triggers:

1. Detect and classify the runtime boundary.
2. For a same-script cutscene, execute/model it inside the current epoch, preserve the ongoing encounter
   state including `stepCount`, re-anchor afterward, and continue. If the predictor cannot model that
   boundary, return `ModelIncomplete` without creating a clean context.
3. For battle entry, terminate the current epoch and its prediction. Capture a separate context only after
   the battle return satisfies the reset-qualified readiness contract.
4. For a completed field script/context switch, begin a new epoch only after stable control, settled
   placement/ground, measured zero velocity, and reset-state evidence are captured.

## Objective Function (MVP)

Primary:
- minimize VI frames to objective completion signal.

Secondary (lightweight):
- prefer successful/clean trigger completion.

## Determinism Policy (MVP)

Assume deterministic replay from savestate + identical inputs.
- No mandatory repeated validation gate initially.
- Add repeat-run validation only if failures appear.

## Telemetry to Capture

- route candidate ID
- segment boundaries
- per-segment frame counts
- trigger/cutscene section hits
- final outcome code
- spline deviation metrics
- collision/anomaly sample identity and observed-versus-predicted outcome
- encounter table segment and explicit `NavigationPredictionStart` provenance
- selected prediction-result identity, objective, context/content/world/model compatibility, search bounds and
  completeness, frontier terminal reason, and witness route/schedule references
- for simulator-backed encounter probes: eligible-check frame, step state, table/rate/modifier, RNG state,
  encounter result, and selected formation provenance

## Open Experiments (post-MVP)

1. Calibrate coordinate, weld, outward-probe, height-continuity, and portal tolerances against known areas
   and the runtime selector.
2. Evaluate catalogued SCT condition paths against runtime game state and resolve incoming transitions.
3. Determine and model the runtime restore data consumed by opcode 156.
4. Better slope/collision-aware and frame-time edge costs.
5. Robust collision-boost exploitation strategy.
6. Advanced optimizers for global+local coupling and top-K route ranking.
7. Add wall blocking, decoded triangle-flag filtering, player step-up rules, and animation/script-driven
   surface availability after their runtime semantics are data- or disassembly-backed.
8. Optional stability-aware ranking if determinism issues emerge.
9. Calibrate within-wave 2.5D sampling/refinement and decide when an observation or oddity may update the
   routing model.
10. Validate exactly which runtime fields reset at script-switch and battle-transition boundaries; the
   current working contract confirms `stepCount` behavior but does not infer every encounter field.
11. Complete and version the required dungeon field-RNG model bundle. Until the separate Moonfish/field
    research is accepted, its detailed lifecycle and draw behavior remain unresolved and unused here.
12. Define the asynchronous `SavorPredict` navigation invocation, bounded-search strategy, frontier
    completeness semantics, and witness schedule-to-controller realization boundary.
13. Add expected battle-time or encounter-risk route costs only after occurrence, formation, battle-cost,
    and post-battle resume semantics are validated.
14. Add an Area-99-specific encounter profile only after its position/altitude/scenario/table-set context
    can be reproduced without applying the dungeon direct-table rule.
