# 04 - Phase and Job Integration

## Status

Future plan. The Navigation work is split into an interactive prototype milestone and a later workflow/job
integration milestone. SPICE owns file parsing; `SavorNavigation` owns SAVOR's CPU-domain navigation and
analysis models; `SavorQt3D` is the first UI host. `SavorWorkflow` coordinates generic claiming,
dispatch, fan-out, and transition invocation through `SavorDb` services. `SavorDb` owns workflow
lifecycle, authored specifications, artifacts, analysis results, transition services, and UI projections;
`SavorCore` owns simulator-program contracts and VM behavior; and `SavorWorker` executes Dolphin-backed
programs. The existing `SavorPredict` subsystem is currently an exploratory executable/CLI; a later
navigation-prediction surface will own modeled temporal outcome search behind an asynchronous boundary
invoked by a separate `SavorQt` planning module.

The normative future workflow, execution-domain, payload-lineage, and artifact contracts are in
[`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md). This document preserves the
interactive prototype contract and summarizes how it will enter that workflow.

## Goal

Establish the model and reusable widget in a small standalone host before adding Navigation jobs,
persistence, and the widget to `SavorQt`.

## Milestone 1 - Interactive Prototype

### Dependency layout

Current prototype:

`third-party/SPICE (SpiceMLD + SpiceSCT) -> SavorNavigation -> SavorQt3D`

Planned Dungeon-analysis extension in the same host:

`third-party/SPICE (SpiceEct) -> SavorNavigation -> SavorQt3D`

Later predictor-backed product flow:

`SavorQt planning module -> SavorPredict outcome search -> immutable prediction result -> Navigation widget`

- Pin SPICE as a git submodule at `third-party/SPICE`; advance the pin only after required SpiceMLD,
  SpiceSCT, or SpiceEct changes pass their upstream regression tests.
- Add `SavorNavigation` as a non-Qt C++20 static library.
- Only `SavorNavigation` may include or expose knowledge of SpiceMLD, SpiceSCT, or SpiceEct
  implementation types.
- The existing `SavorQt3D` application now links to `SavorNavigation`; its obsolete dependency on the
  deleted `SavorMLD` project has been removed.
- Preserve useful `SavorQt3D` prototype assets: file picker, Quick 3D scene, layer controls, diagnostics,
  and last-directory behavior.

### Interactive input and data flow

Items explicitly marked planned below are future analysis slices in the standalone host, not part of the
implemented MLD/SCT/graph/A* prototype.

1. **Implemented:** The user chooses an `.mld` with the `SavorQt3D` file picker.
2. **Implemented:** The picker remembers the last directory. The known US disc dump is a local development
   fixture only and is never hardcoded into source or project settings.
3. **Implemented:** For a conforming `aNNNC.mld`, `SavorNavigation` derives case-normalized area key `NNNC`
   and checks only the MLD directory for expected `meNNNC.sct`. A matching sibling loads automatically.
4. **Implemented:** If the expected sibling is absent, the MLD remains usable and the UI reports its
   expected name before offering `Load Related SCT...` or continuation without it. A manual selection may
   be elsewhere but is accepted only when its prefix-stripped key matches the current MLD
   case-insensitively.
5. **Implemented:** Loading runs off the UI thread. Opening a new MLD clears the old SCT association before
   starting discovery; rejecting a mismatched manual SCT preserves any valid association already loaded
   for the current MLD.
6. **Planned profile extension:** Treat the selected MLD directory as the complete companion set and look
   there for same-stem `aNNNC.ect`. Apply Area-99 precedence first; otherwise ECT presence establishes
   Dungeon, while a matched SCT without ECT establishes Safe. There is no manual ECT override in this
   milestone.
   Safe/no-encounter profiles treat ECT as not applicable. Area 99/indexed ECT is explicitly deferred and
   is never decoded with dungeon rules.
7. **Implemented MLD/SCT; planned ECT extension:** `SavorNavigation` reads the selected compressed MLD and
   calls SpiceMLD, and calls SpiceSCT for a matched SCT. The planned Dungeon extension calls SpiceEct for
   the associated ECT. The SPICE libraries own AKLZ detection/decompression and their respective parsing.
8. **Implemented:** `SavorNavigation` converts canonical `MldFile` GRND/GOBJ resources plus projected
   `world` and `searchWorld` evidence into `NavigationAreaModel`. A transient, non-exported Blender IR
   projection is flattened into SAVOR-owned meshes for exact `fxn=wall` collision regions and every
   SPICE-classified trigger plus exact normalized `motscpt` MovingObjects; object-role-only GOBJ blocks
   remain excluded from walkable surfaces.
9. **Implemented:** `SavorNavigation` creates `NavigationScriptModel` summaries and a SAVOR-owned opcode-77
   start catalog while retaining the full SCT parse result privately, then builds the stable triangle/portal
   `NavigationTraversalGraph`. Ground traversal preserves ordered linked-EntryID fallback chains and derives
   current-bundle-first collision handoffs rather than matching only coincident external boundaries. It
   combines these with the area as an in-memory `NavigationScenarioModel` containing no public SPICE or Qt
   types.
10. **Planned shared-analysis extension:** A shared layer-preserving 2.5D analysis field retains X/Z
    sampling plus resolved Y, source surface/triangle identity, GRND/GOBJ provenance, collision evidence,
    and raw triangle metadata. Static collision, anomaly, and dungeon encounter workstreams consume this
    common CPU-domain field without
    replacing the authoritative true-3D graph.
11. **Planned Dungeon encounter extension:** For the Dungeon profile, apply the current evidence-qualified
    direct-table selector hypothesis to GRND and ground-role GOBJ metadata and attach normalized flat ECT
    tables. Static encounter and current-route exposure outputs remain analysis-only and do not alter A*.
    Marginal or seeded results require an explicit ready `NavigationContextResult`; a spatial start alone
    never supplies encounter state.
12. **Implemented area/SCT behavior; planned ECT behavior:** The UI thread replaces the displayed model
    after a complete or usable partial MLD load. Partial area models are visibly diagnosed and never
    treated as pathfinding-ready. Missing or failed SCT content is advisory and does not disable manual
    pathfinding. Failure to read, decompress, or parse a present Dungeon-classifying ECT disables only
    resolved encounter-table/exposure output; available static selector metadata remains inspectable and
    the profile remains Dungeon.

The first milestone does not use Dolphin/ISO acquisition, disc-wide area-ID lookup, a workflow job, or a
durable area artifact. The scenario, privately retained SCT parse result, selected path, and overlays
remain in memory.

### Prototype failure classes

- selected file missing, unreadable, or empty
- AKLZ decompression failure
- malformed or unsupported MLD content
- MLD name does not conform to `aNNNC.mld`, so SCT association is unavailable
- expected same-directory `meNNNC.sct` is absent
- manually selected SCT has the wrong prefix-stripped area key and is rejected before parsing
- matching SCT is missing, unreadable, empty, undecompressible, malformed, unsupported, or warning-bearing;
  these are advisory and manual pathfinding remains available
- a discovered Dungeon-classifying ECT is unreadable, empty, undecompressible, malformed, partial, or has
  an unsupported layout; these are advisory to pathfinding and never discard valid MLD/SCT state or
  reclassify the area as Safe
- missing triangle metadata, an unsupported dungeon selector, a selector with no corresponding 1-based
  flat ECT table, zero stage, zero overall rate, or malformed encounter-row weights; preserve each as a
  distinct encounter-analysis state rather than guessing or silently normalizing
- unknown/calibration-free movement-to-check cadence; retain static table and route-distance analysis but
  mark check-count, VI-time, cumulative probability, and seeded results unavailable
- Area 99 or indexed ECT supplied to the dungeon profile; report the explicit deferred profile and do not
  fall back to direct dungeon selector semantics
- parse warnings or incomplete navigation content
- missing or unusable exact-wall object trees, meshes, triangles, or weighted roots
- missing or unusable trigger object trees, meshes, triangles, or weighted roots; this is warning-only and
  retains an approximate red cube marker rather than making the load partial
- missing or unusable MovingObject geometry; this is also warning-only and retains an approximate orange
  cube marker
- conversion into `NavigationAreaModel` failed
- renderer rejected otherwise valid navigation geometry
- malformed/degenerate/non-manifold walk triangles, missing/truncated authored fallback targets,
  discontinuous or stacked-ambiguous collision coverage, or a fallback chain with no usable handoff
- invalid endpoint picks, incomplete or ambiguous opcode-77 start resolution, projected-trigger goal
  resolution failures, or an unreachable start/goal pair

All failures and warnings are shown in the existing diagnostics surface. A failed load leaves the last
successfully rendered model in place. Incomplete but usable ground geometry may be rendered as a partial
model with pathfinding disabled.

### Implemented slice acceptance (2026-07-18)

- Open an AKLZ-compressed MLD directly and perform parsing/adaptation off the UI thread.
- Render native GRND and ground-role GOBJ surfaces in distinct colors, exact `fxn=wall` collision
  boundaries as meshes, trigger entries as their attached red meshes, and retain the other collision,
  unknown, visibility, and diagnostic layers. Exact `motscpt` entries render as orange meshes in a separate
  `MovingObjects` layer. Exact wall entries never fall back to cube markers; triggers and MovingObjects with
  missing projected geometry retain approximate cubes.
- Preserve source entry/block/node identity plus ordered EntryID fallback evidence, including EntryID `0`,
  without confusing a raw authored target with derived navigable adjacency.
- Validate `a101b.mld` as 12 surfaces (6 GRND and 6 ground-role GOBJ), 504 vertices, 401 triangles,
  59 collisions including 51 wall regions and 517 wall meshes (6,795 vertices, 8,347 triangles),
  16 fully projected triggers with 38 meshes (320 vertices, 384 triangles), 11 fully projected `motscpt`
  MovingObjects with 55 meshes (462 vertices, 566 triangles), and 21 remaining unknown entries.
- Report unreadable/empty files, AKLZ decompression errors, parser diagnostics, and incomplete ground
  content without exposing SpiceMLD, SpiceSCT, or SpiceEct types to Qt.

### Prototype acceptance

- Open a compressed US field MLD directly from the disc dump without an external decompression step.
- Display SAVOR-owned surfaces, collision/trigger information, and parser/conversion diagnostics.
- Keep attached trigger geometry distinct from future SCT/controller activation predicates and interaction
  radii.
- Toggle model layers using the existing visibility controls.
- Automatically parse a matching sibling SCT or report the expected name and offer a strict manual
  replacement. Show its match/load status, section count, exact `loop` count, instruction count, and
  diagnostics plus its SAVOR-owned opcode-77 start catalog.
- Derive and render validated traversal portals, keep manual point selection as the default, allow the user
  to choose a resolvable opcode-77 start or manually pick one, manually select a ground or projected-trigger
  goal, and display one deterministic A* route over the in-memory model.
- Keep SpiceMLD, SpiceSCT, SpiceEct, and Qt types out of the `SavorNavigation` public model.

### Active pathfinding-slice acceptance

- `a101b.mld` discovers sibling `me101b.sct`; a missing sibling is recoverable, `me201a.sct` is rejected
  for that MLD, and a rejected replacement does not discard an existing valid SCT.
- Generic SpiceSCT relative `CallSubscript` resolution and typed numeric literal access are covered by
  upstream tests before the SPICE pin advances. Only opcode 77 supplies catalog options; opcode 156 and
  all other instructions are excluded.
- Opcode-77 options preserve branch/switch/call provenance, distinguish authored arrivals from scripted
  repositions, and expose incomplete dynamic/sentinel placements without enabling them.
- The Path toolbar defaults to `Manual point`; a resolvable option anchors to the unique nearest triangle
  on its matching ground `tblId`, while ambiguous or failed resolution preserves the previous start. Manual
  facing is settable, and known opcode-77 yaw supplies scripted facing.
- The traversal graph uses one stable node per valid walkable triangle, bidirectional shared-boundary
  adjacency, and directed collision handoffs. It tests every current-entry GRND/GOBJ surface before applying
  linked EntryIDs in authored order, chooses the first accepting bundle, and may join a source boundary to
  a target triangle footprint without requiring coincident target external boundaries.
- Motion-bearing or non-`ground` entries retain muted-orange bind-pose handoff candidates in Links but do
  not contribute active A* nodes or edges until their runtime state is modeled.
- Short ground clicks set manual starts or ground goals without replacing drag-to-orbit behavior. Real
  projected trigger meshes are goal-selectable; fallback cubes are not. Trigger goals choose the largest
  projected mesh deterministically by world-space bounds, resolve to walkable graph geometry, and render a
  magenta AABB.
- The Links layer shows portals. The Route layer shows a thick path, compact fixed-size start/ground-goal
  markers at the `man` fallback scale, and a short start-facing ray plus graph/route diagnostics.
- Incomplete ground/wall geometry blocks search. Missing SCT, trigger geometry, or MovingObject geometry
  remains advisory under its existing diagnostic/fallback policy. Trigger goal selection does not define
  runtime activation semantics.
- The active slice does not claim runtime-perfect wall blocking, triangle-flag filtering, step-up limits,
  animated/scripted surface state, or collision-selector calibration. Those remain explicit later
  validation/refinement work.

### Planned shared-analysis slice acceptance

- Construct one layer-preserving 2.5D analysis field from the same SAVOR-owned surfaces and triangle keys
  used by the true-3D graph. Stacked surfaces remain distinct and no raster/sample becomes a replacement
  navigation mesh.
- Run the Navmesh Survey from the exact output savestate of a ready `NavigationContextResult`. Expand
  disposable survey anchors outward, distinguish local validity from entry reachability, and release
  ordinary collision, oddity, trigger, and reproduction jobs as their dependencies become available.
- Expose separate collision and anomaly outputs. Static uncertainty or a simulator mismatch is diagnostic
  until an explicit validated refinement promotes it into the routing model.
- Preserve sticky-corner positional-jump and wall-contact ramp-speed measurements as reproducible
  movement-response evidence without optimizing their use inside the survey.
- Represent automatic-trigger approach boundaries, interactable activation envelopes, and state-qualified
  pre/post door transitions. Dynamic trigger suppression/permission within a job is required direction,
  while the concrete hook and gate modes remain research-gated.
- For the Dungeon profile, parse the strictly matched sibling `aNNNC.ect` whose presence established that
  profile through SpiceEct, apply the evidence-qualified direct-table selector hypothesis to both GRND and
  ground-role GOBJ triangles, and keep selector zero,
  missing metadata, unsupported selectors, missing tables, zero stage, and zero rate distinct.
- Display a whole-area static selector/table overlay and segment only the current deterministic route for
  active/no-encounter distance and table transitions. Encounter analysis does not change A*, route
  ranking, trigger semantics, or the current control-solver contract.
- Reference the exact ready `NavigationContextResult` for marginal values. Distinguish an exact
  per-active-check grid result from a cumulative marginal/independence approximation, future
  predicted/seeded model output, and simulator-observed/validated evidence.
- Reuse the same field, graph, collision/anomaly, route, and control pipeline for safe/no-encounter areas
  with encounter status not applicable. Defer Area 99 and its local-lane, position/altitude/scenario,
  `fldEfcontrol`, encounter-zone, and runtime table-set resolution.

## Milestone 2 - Workflow Integration

After the model and widget boundary are validated:

- Hash the selected disc by streaming SHA-256, materialize versioned internal-file manifests through
  Dolphin DiscIO extraction, and normalize SPICE/ALX results into a content bundle rather than relying on
  a machine path or manual file picker.
- Capture a ready `NavigationContextResult` only after a reset-qualified field script/context switch or
  battle return. Entering battle terminates the epoch; a same-script cutscene remains in the existing epoch
  and never creates a fresh-entry assumption.
- Interpret the already loaded SCT plus related package/controller content for target, arrival, and
  transition discovery, automatic start-condition evaluation, and opcode-156 runtime restoration.
- Run the Navmesh Survey from the immutable Navigation Context output savestate. Create short-lived
  verified anchors near work, including state-qualified successor anchors reached through required
  doors/script transitions, then fan out ordinary collision, oddity, later trigger-characterization, and
  reproduction probes. Encounter suppression is independent from research-gated trigger control, which may
  need to suppress and permit activations dynamically within one job. Persist all control history,
  observations, and sub-triangle refinements separately from the static world and clean
  prediction/validation states. `SavorCore` has no general runtime patch/write API today, and no trigger
  hook or gate modes have been selected.
- Define and persist SAVOR-owned context, content, world, 2.5D analysis, exploration/refinement,
  collision/anomaly, optional encounter, prediction, control, and validation schemas.
- Move the reusable Navigation widget into `SavorQt` while retaining `SavorQt3D` as a focused development
  harness if it remains useful.
- Add a separate `SavorQt` outcome-planning module that authors a no-encounter or specific-encounter
  objective, selects a compatible world and explicit context-result ID, invokes `SavorPredict`, monitors
  and compares candidate results, and selects one result for the Navigation widget. The widget does not run
  or rank movement-schedule searches.
- Define a future asynchronous navigation boundary for the current exploratory `SavorPredict`
  application/CLI. Do not make `SavorQt` depend directly on its executable internals or imply that a
  reusable navigation predictor API already exists.
- Expand the existing hidden `dungeon_explorer` workflow-unit contract instead of creating a parallel
  generic Navigation unit. Add a future `safe_explorer` that reuses the same pipeline with encounters not
  applicable. Retain the existing hidden `overworld_explorer` for later Area-99-specific work.
- Once a local CPU workflow lane exists, add CPU-domain analysis steps alongside simulator-program steps,
  solver fanout, route artifacts, and publishing under those explorer workflow units.

Optional SPICE diagnostic exports, including Blender IR or serialized area views, may be used for parser
and rendering comparison. They are not a runtime input contract and must not be required to load or plan
an area.

## Versioning Decision

- GameCube Skies of Arcadia has three versions; only the US build is currently supported by the relevant
  addresses and breakpoints.
- Future `DiscImageIdentity` includes streaming SHA-256, byte size, Game ID, region, and revision. Internal
  content records include disc path plus content hash; normalized bundle keys also include parser/model/
  schema and coordinate-policy versions.
- The manual prototype records the selected source path for diagnostics, but a machine-local path is only
  a locator and never durable identity.

## Workflow Integration Overview (Later Milestone)

Authored workflow-unit specifications and lifecycle state are owned by `SavorDb`; generic claiming,
dispatch, fan-out, and transition invocation are coordinated by `SavorWorkflow`. Workflow units are not
PhaseScript program kinds:

- `dungeon_explorer` already exists as a hidden placeholder and is the first workflow unit to expand.
- `safe_explorer` is a future no-encounter unit that reuses the shared 2.5D, collision/anomaly, route, and
  control pipeline.
- `overworld_explorer` already exists as a hidden placeholder and remains reserved for later overworld and
  Area-99-specific behavior.

The normative logical sequence is:

1. `nav.capture_context`
2. `nav.materialize_content`
3. `nav.build_world`
4. `nav.explore_geometry`
5. `nav.build_refinement`
6. `nav.search_predicted_outcomes`
7. explicit user/result selection
8. `nav.solve_controls`
9. `nav.validate_route`
10. `nav.publish_result`

CPU/domain work includes content normalization, world construction, static profile/encounter analysis,
graph search, and refinement construction. It remains in-process until a local CPU workflow lane exists.
Dolphin workers are reserved for context capture, patched exploration, control solving, and clean runtime
validation. `SavorPredict` owns bounded planning-level outcome search. `SavorDb` owns immutable artifact
references and lineage; patched exploration states never become predictor contexts or clean validation
inputs. Detailed inputs, outputs, failure states, and project ownership are normative in
[`NavigationContextWorkflow/06-phase-job-and-artifact-integration.md`](NavigationContextWorkflow/06-phase-job-and-artifact-integration.md).

### `nav.build_world`

CPU/domain construction of the static SAVOR-owned world, graph, profile, optional encounter model, and
diagnostics from a versioned content bundle. It does not consume a runtime savestate.

### `nav.explore_geometry`

This Dolphin-backed step is the parallel portion of the Navmesh Survey. It consumes the explicitly
referenced ready context output savestate as an immutable bootstrap, establishes verified disposable
anchors, and releases candidate batches by dependency rather than waiting for five global barriers.
Anchor expansion may intentionally traverse a door/script trigger and checkpoint its attributable stable
successor before ordinary probing resumes. Workers emit anchor records and observations only; they never
mutate the shared graph or refinement.

### `nav.build_refinement`

This deterministic CPU/domain step reduces selected survey observations into state-qualified passability,
directional movement-response/oddity evidence, trigger activation/transition evidence, coverage, and a new
immutable `NavigationWorldRefinement`.

### `nav.analyze_collisions`

Historical draft name. Static candidate analysis is folded into world/refinement construction; empirical
coverage now enters through `nav.explore_geometry`. No observation silently mutates source geometry.

### `nav.analyze_dungeon_encounters`

Static Dungeon encounter structure remains CPU/domain analysis attached to `nav.build_world` or a derived
analysis artifact. Any marginal/seeded exposure explicitly references a ready `NavigationContextResult`.
Safe does not run this analysis, and Overworld never substitutes the Dungeon decoder.

### `nav.search_predicted_outcomes`

Consumes one explicit ready context, versioned content/world/refinement and model bundle, objective, and
bounds. It emits world-space witness routes plus planning-level movement/no-movement/interruption schedules,
coverage/frontier data, completeness, and diagnostics. It emits no raw stick/camera tape. A same-script
cutscene remains within the epoch or yields `ModelIncomplete`; battle entry terminates it.

### `nav.plan_route`

Historical standalone route-planning name retained only for the interactive/static A* operation. The
integrated predicted-outcome sequence uses `nav.search_predicted_outcomes`; geometric A* still remains
encounter-neutral and may supply candidates without mutating predictor results.

### `nav.solve_controls`

Realizes the explicitly selected planning witness as controller and camera input in Dolphin and returns a
`NavigationControlSolveResult`. Solver fan-out is separate from predictor planning and does not change the
selected context or prediction artifact.

### `nav.validate_route`

Replays the selected control result from an unpatched clean state compatible with the referenced context
and returns `NavigationValidationResult`. Patched exploration savestates are prohibited here.

### `nav.publish_result`

Publishes immutable context/content/world/refinement/prediction/control/validation lineage, canonical
selected results, metrics, diagnostics, and optional replay references without collapsing evidence layers.

## Artifact and Payload Direction

Workflow payloads carry durable artifact IDs, schema/version identifiers, explicit context-result IDs, and
compatibility hashes rather than machine-local paths or a mutable latest-context reference. Relational
records own lifecycle, selection, status, and lineage; large savestates, content bundles, worlds,
observations, traces, reachability fields, and controller tapes remain object-store artifacts. Static
world, patched exploration observations/refinement, prediction, control solving, and clean validation are
separate immutable layers. The normative artifact family and failure contracts are maintained in the
Navigation Context workflow package rather than duplicated here.

## UI/Visualization Integration

- Develop the reusable Navigation widget in `SavorQt3D` first.
- Render `NavigationAreaModel` geometry, walking planes, links, collision/trigger layers, diagnostics, and
  path overlays.
- The implemented pathfinding slice supplies manual start selection with facing, manual ground or
  projected-trigger goals, the grouped opcode-77 Start selector, and path/portal/endpoint overlays.
- The shared-analysis slice adds separately toggleable collision/anomaly and dungeon encounter layers.
  Navmesh Survey layers also expose survey anchors, tested/untested coverage, state-qualified boundaries,
  sticky-jump/ramp-speed candidates, and automatic/interactable trigger evidence without changing A*
  implicitly.
  Encounter UI distinguishes static selector/table data, explicit context-qualified marginal results, modeled
  predicted/seeded results, and simulator-observed/validated evidence. Static/marginal layers do not change
  the displayed A* route.
- The later `SavorQt` outcome-planning module launches/monitors `SavorPredict` searches and selects one
  immutable result. The Navigation widget only checks compatibility and renders that result as a route
  prefix/cutoff or triangle-keyed 2.5D reachable set/frontier. A selected predictor witness may differ from
  the ordinary A* route; this is an explicit separate overlay, not an implicit route mutation.
- Keep static encounter coloring and Prediction Reachability separate. A predictor frontier is qualified by
  objective, context result, world/model fingerprints, and search bounds; it is not a probability heatmap or
  universal safety boundary.
- `SavorQt3D` may exercise result rendering with fixtures but does not own predictor execution or
  movement-schedule search.
- Install the widget into `SavorQt` only after its public model boundary is stable.
- Navigation remains UI-first; no CLI objective-entry path is planned for MVP.

## Remaining Integration Questions

1. Simulator-worker batch sizing and retry/reproduction policy for dependency-driven Navmesh Survey anchor,
   collision, oddity, and trigger work, plus control solving/validation by route candidate, segment, or
   hybrid.
2. Durable world, 2.5D analysis, script-index, collision/anomaly, and optional encounter schemas.
3. Automatic mapping from game/area identity to the full required package set beyond the strict sibling
   MLD/SCT/ECT filename associations.
4. Coordinate-policy calibration and validation fixtures.
5. 2.5D sampling/refinement policy and the evidence threshold for promoting an anomaly into the route
   model.
6. Trigger identities and categories, safe interception/control points, preservation of initialization and
   environmental controllers, door/forced-movement completion boundaries, and the audited dynamic
   suppression/permission contract. No gate modes are normative yet.
7. Exact runtime capture/calibration for reset-state fields, dungeon encounter-check cadence, and state
   signatures beyond the current `stepCount` working contract.
8. Future `SavorPredict` navigation boundary: process versus library shape, asynchronous invocation,
   cancellation/progress, search fanout, and model-bundle versioning.
9. Immutable prediction-result/trace/witness/reachability schemas, compatibility fingerprints, frontier
   completeness semantics, and the metric under which a multi-path result has a single furthest point.
10. Area-99-specific profile design for `overworld_explorer`; the dungeon direct-table analyzer remains
   prohibited there.

Workflow-specific open research is maintained in
[`NavigationContextWorkflow/07-open-research-questions.md`](NavigationContextWorkflow/07-open-research-questions.md).
