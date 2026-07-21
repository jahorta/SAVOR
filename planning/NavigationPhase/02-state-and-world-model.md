# 02 - State and World Model

## Status

Future plan. Low-level SoA file parsing is delegated to the pinned SPICE submodule. The implemented
prototype foundation builds SAVOR-owned scenario, area, script/start-catalog, traversal, and route models
in process from SpiceMLD and SpiceSCT output. The planned Dungeon Navigation extension adds private
SpiceEct conversion, area profiles, runtime collision evidence, movement-anomaly evidence, and an optional
dungeon encounter model. These additions do not change the rule that SPICE types stay behind the
`SavorNavigation` boundary.

Dungeon Navigation is the current implementation target. A later Safe Navigation profile will reuse the
surface-constrained 2.5D world, graph, collision validation, and movement-anomaly components without an
encounter model. Area 99 remains a separate Overworld Navigation model.

The reset lifecycle, disc/content provenance, context-result, suppressed-exploration, prediction,
control-solving, validation, and artifact-lineage contracts are normative in
[`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md). This document summarizes how those
future layers relate to the current navigation world model.

## Objectives

Define a planning state representation that supports:
- Overlapping walk surfaces in Y.
- Obstacle and trigger interactions.
- Cutscene interrupts and repositioning.
- Ordered content-derived area profiles and explicit companion-file evidence.
- Collision geometry whose conversion completeness is distinct from runtime-validation confidence.
- Reproducible movement/collision-anomaly observations.
- Optional per-triangle Dungeon encounter data and evidence-qualified route exposure.
- Selected predictor results projected as provenance-checked route prefixes or layer-preserving 2.5D
  reachable sets/frontiers without putting temporal search in the map widget.
- Route-planning independent from camera implementation details.

## Library Boundary and Load Result

`SavorNavigation` is a non-Qt C++20 static library and the only SAVOR project that includes SpiceMLD,
SpiceSCT, or the planned SpiceEct headers. Its public concepts for the implemented prototype and planned
Dungeon extension are SAVOR-owned:

- `NavigationAreaIdentity`
  - Recognizes an MLD name shaped as `aNNNC.mld` and stores the original source plus a case-normalized,
    prefix-stripped `NNNC` area key.
  - Derives the expected SCT name `meNNNC.sct` and planned ECT name `aNNNC.ect`. A companion is associated
    only when its own prefix-stripped key matches case-insensitively; provenance retains original paths
    and spelling.
  - A nonconforming MLD remains loadable as geometry, but SCT discovery and manual association are
    unavailable and diagnosed; it cannot receive a guaranteed navigation profile.
- `NavigationAreaProfile` and companion evidence (planned)
  - Classify complete companion sets in strict order as `Overworld`, `Dungeon`, `Safe`, or
    `UnknownViewOnly`.
  - Any key beginning with `099` is Overworld regardless of companion presence. For non-099 keys, matched
    ECT presence selects Dungeon before ECT parse status is considered; matched SCT with no ECT selects
    Safe only when the selected directory is treated as the complete companion set. Remaining inputs are
    Unknown/View-only.
  - Preserve ECT presence/association independently from I/O, AKLZ, and parse status. An invalid ECT
    remains evidence that the non-099 scenario is Dungeon while making encounter content incomplete.
  - Preserve Safe traversability evidence separately: keys below `200a` are known traversable, while keys
    at or above `200a` are candidates with unverified traversability. Safe excludes random encounters, not
    scripted interruptions.
- `DiscImageIdentity`, `DiscContentManifest`, and `NavigationContentBundle` (planned)
  - Identify a source disc by streaming SHA-256, byte size, game ID, region, and revision. A machine-local
    ISO path is a locator only and never durable identity.
  - Record each Dolphin DiscIO-extracted internal path and content hash, then key the normalized bundle by
    disc hash, internal-file hashes, parser/model/schema versions, and coordinate-policy version.
  - SPICE/ALX own source-format parsing; `SavorNavigation` owns normalized navigation content. Cache reuse
    requires exact identity/version compatibility.
- `NavigationEpoch` and `NavigationContextResult` (planned)
  - Start an epoch only after a reset-qualified completed field script/context switch or battle return has
    restored stable control. Entering battle ends the epoch. Save load qualifies only when runtime evidence
    confirms a reset-producing script switch.
  - Capture immutable transition provenance, clean source-savestate identity, area/profile/content/world
    references, stable player placement/ground, measured zero velocity, neutral authored input, measured
    `stepCount`, independently captured encounter/RNG fields, and readiness diagnostics.
  - A same-script cutscene return remains inside the same epoch and does not reset `stepCount`; it never
    creates another context. Camera/control orientation is excluded from the result and predictor input.
  - Readiness distinguishes `Ready`, `NonResetContinuation`, `VelocityNonZero`, `ControlNotStable`,
    `ResetStateMismatch`, and `IncompleteCapture`. Downstream predictions reference a context-result ID
    explicitly; there is no implicit latest context.

- `NavigationAreaLoader`
  - Accepts a filesystem path selected by the UI.
  - Reads the compressed file and invokes SpiceMLD without pre-decompressing it in SAVOR.
  - Calls `MldParser::parseBytes` once and treats canonical `MldFile` as the primary parse representation.
  - Requests a transient compatibility projection for `world`, `searchWorld`, and targeted wall/trigger/
    MovingObject geometry; Blender IR export and retention are disabled.
- `NavigationAreaLoadResult`
  - Returns a complete model, a visible but incomplete partial model, or a load failure.
  - Carries source identity and normalized diagnostics suitable for display without exposing SpiceMLD
    diagnostic types.
- `NavigationAreaModel`
  - Owns all data needed by the viewer and route planner.
  - Preserves surface source kind (`GRND` or ground-role `GOBJ`), entry/block/node identity, transformed
    meshes, provisional entry links, collision/trigger/MovingObject regions, bounds, source identity, and
    normalized diagnostics. Exact wall, trigger, and `motscpt` MovingObject regions own SAVOR
    `NavigationRegionMesh` instances with
    object/chunk/node/attach provenance.
  - Exposes `hasCompleteGroundGeometry` and `hasCompleteWallGeometry`; pathfinding must not run when either
    is false. It also exposes `hasCompleteTriggerGeometry` and `failedTriggerRegionCount` for visualization
    diagnostics plus equivalent MovingObject completeness fields; trigger or MovingObject incompleteness
    does not make an otherwise usable model partial.
  - Contains no Qt, SpiceMLD, or SpiceSCT types.
- `NavigationScriptModel`
  - Owns the matched SCT's source identity, association/load status, normalized diagnostics, section
    summaries, total instruction count, exact case-normalized `loop` section count, and
    `NavigationStartCatalog`.
  - Contains no SpiceSCT types. The complete `SctParseResult` remains privately retained behind an opaque
    `SavorNavigation` implementation boundary for later transition/state analysis.
- `NavigationStartCatalog`, `NavigationStartOption`, and `NavigationStartVariant`
  - Describe opcode-77 placements using SAVOR-owned ground selector, XYZ/yaw values, availability,
    authored-arrival or scripted-reposition kind, instruction/section/call provenance, and condition paths.
  - Preserve true/false predicate polarity, switch choices/cases, nested conditions, and resolved calls.
    Multiple paths to the same bit-exact placement remain OR variants; conditions within a path are AND
    clauses. Unsupported or cyclic flow remains visible through opaque condition text and diagnostics.
  - Opcode 156 and every non-77 opcode are excluded from the catalog.
- `NavigationTraversalGraph`
  - Owns one stable node per valid walkable triangle, triangle centroid/normal data, and directed portal
    edges. Intramesh shared-boundary adjacency is bidirectional; cross-surface traversal is directed when
    matching `NavigationGroundLink` evidence and overlapping boundary geometry agree.
- `NavigationScenarioModel`
  - Owns the loaded `NavigationAreaModel`, optional matched `NavigationScriptModel`, traversal graph, and
    combined diagnostics for one in-memory prototype scenario.
  - The planned extension also owns profile/companion evidence and a Dungeon-only optional encounter
    model. Runtime observations and passability refinements are immutable evidence layers referencing the
    static scenario rather than mutations hidden inside it.
- `NavigationGraphAnchor`, `NavigationPathQuery`, and `NavigationPathResult`
  - Keep exact and snapped endpoint positions, stable triangle identity, snap distance, route status,
    cost/length, route points, and specific invalid/unreachable diagnostics in SAVOR-owned types.
- `NavigationStartResolver`
  - Converts an available opcode-77 placement through `NavigationCoordinatePolicy`, matches its selector
    to `NavigationSurface::tblId`, and anchors to the unique nearest matching triangle.
  - Reports incomplete placement, missing ground/graph, tied surface ambiguity, or anchoring failure
    without replacing a previously valid start. A known converted yaw supplies the scripted start facing.
- `NavigationTriggerGoalResolver` and `NavigationTriggerGoalTarget`
  - Accept only trigger regions with real usable projected meshes; approximate fallback cubes never enter
    goal resolution.
  - Choose the largest projected mesh deterministically by world-space AABB squared diagonal, then stable
    triangle-count/index tie breaks. Resolve its nearest unambiguous point onto walkable graph geometry and
    retain the selected bounds for display.
- `NavigationCollisionValidationModel` (planned, shared)
  - Stores probe observations keyed to source geometry and runtime probe position, including expected and
    observed contact/response, active ground/resource identity, approach, input/camera context, and
    provenance.
  - Reports coverage, confidence, and discrepancies separately from source-geometry completeness; it
    never silently rewrites extracted meshes.
- `NavigationMovementAnomalyModel` (planned, shared)
  - Stores candidates at corners, seams, slopes, and boundaries with approach pose/facing, camera/input
    sequence, baseline and observed displacement or speed, VI-frame cost, resulting route position,
    repetition, variance, and a beneficial/neutral/harmful/unresolved classification.
  - A candidate is analysis evidence, not a route action or edge-cost change until a positive result is
    measured and reproducible.
- `NavigationExplorationObservationSet` and `NavigationWorldRefinement` (planned, shared)
  - Keep encounter-suppressed and selectively trigger-suppressed exploration runs separate from their
    clean source context and static world. Each named patch profile, runtime observation, state signature,
    coverage fact, and provenance record is immutable.
  - Represent locally blocked/passable sub-triangle regions, effective boundaries, and confidence because
    projected walls may intersect a walkable triangle. A refinement never rewrites source geometry and a
    patched exploration savestate never becomes a predictor context or clean validation input.
- `NavigationDungeonEncounterModel` (planned, optional)
  - Exists only for Dungeon profiles with usable encounter content and owns SAVOR-converted ECT tables,
    rates, rows, source/status provenance, per-triangle selector interpretations with evidence provenance,
    and diagnostics.
  - Keys encounter geography by `NavigationTriangleKey`. It is not a `NavigationRegion`, trigger volume,
    or surface `tblId`; the latter identifies a collision resource rather than an encounter table.
  - Owns static selector/table structure and assumed or marginal route exposure. Predicted or seeded model
    output belongs to `NavigationPredictionResult`; observed or validated runtime output belongs to
    validation artifacts. None of these change the current geometric/slope A* cost.
- `NavigationPredictionResult` (planned SAVOR-owned cross-project contract)
  - Is produced by a future navigation surface in the existing `SavorPredict` predictor subsystem, whose
    present exploratory executable/CLI is not yet a reusable SavorQt API.
  - Records the explicit `NavigationContextResult` reference, objective, immutable
    content/world/refinement/model/search provenance, search completeness and bounds, witness world-space
    route and movement/no-movement/interruption schedule references, predicted trace/result, and either a
    route-prefix cutoff or `NavigationTriangleKey`-keyed 2.5D reachability/frontier data.
  - Contains no raw stick/camera realization. `NavigationControlSolveResult` owns controller tape and
    camera realization, and `NavigationValidationResult` owns comparison against a clean runtime.
  - Is distinct from `NavigationDungeonEncounterModel`: the encounter model describes static/marginal
    scenario facts, while a prediction result describes one bounded temporal search from one start state.
- `NavigationPredictionOverlay` (planned Qt-neutral adapter)
  - Lets `SavorNavigation` compare the prediction's game/area/world/graph/coordinate-policy fingerprints
    with the loaded scenario and spatially adapt compatible coverage/frontier data for the widget.
  - Never enumerates movement schedules or recomputes predictor semantics. Spatial mismatch rejects the
    overlay; a matching world with older context may be shown as historical/stale; missing fingerprints are
    unverifiable rather than compatible by assumption.

The interactive prototype keeps the scenario and privately retained SCT parse result in memory. The file
pickers and last-directory setting belong to `SavorQt3D`; identity matching, file reading, parsing,
conversion, graph/search construction, and coordinate policy belong to `SavorNavigation`.

### Parse and conversion flow

1. `SavorQt3D` selects an `.mld` path and starts an asynchronous scenario load.
2. `SavorNavigation` derives `NavigationAreaIdentity`. For `aNNNC.mld`, it checks only the MLD directory
   for `meNNNC.sct`; it performs no disc-wide search or companion-name guessing.
3. `SavorNavigation` reads the MLD and calls SpiceMLD. SpiceMLD detects and decompresses AKLZ data, then
   produces canonical `MldFile`.
4. When the strictly matched SCT exists, `SavorNavigation` reads it and calls SpiceSCT, which owns AKLZ
   handling and SCT parsing. A missing or failed SCT is advisory and does not discard usable MLD content.
5. `SavorNavigation` converts canonical GRND/GOBJ resources plus `world` and `searchWorld` evidence into
   `NavigationAreaModel`. It transiently flattens the compatibility projection's exact `fxn=wall` and
   SPICE-classified trigger object trees plus exact normalized `motscpt` object trees into world-space
   SAVOR meshes, including hierarchical transforms, repeated instances, and weighted bind-pose roots.
6. `SavorNavigation` analyzes all opcode-77 occurrences in the parsed SCT, classifies initialization-path
   placements as authored arrivals and other occurrences as scripted repositions, and attaches their
   condition/call provenance to `NavigationScriptModel`. Opcode 156 is ignored.
7. `NavigationGraphBuilder` converts complete walkable geometry and provisional links into stable triangle
   nodes plus validated portals. It diagnoses degenerate/malformed/non-manifold geometry, unresolved
   ambiguous links, and links with no geometric portal rather than inventing connectivity.
8. The UI thread receives only the SAVOR scenario and diagnostics. A complete area model is
   pathfinding-ready even if SCT association or parsing failed; a partial area may be rendered for
   diagnosis but is explicitly not pathfinding-ready.

If automatic discovery fails, `SavorQt3D` offers `Load Related SCT...`. A manual SCT may be outside the
MLD directory but is rejected before parsing unless its `meNNNC.sct` key matches the current `aNNNC.mld`
key. Rejection reports both expected and selected identities and preserves any already loaded valid SCT.
Opening another MLD clears the prior association before performing discovery for the new area.

### Planned profile and ECT extension

1. For a conforming `aNNNC.mld`, inspect the selected MLD directory as the complete automatic companion
   set for exact same-key `meNNNC.sct` and `aNNNC.ect` names. Preserve presence, original path/spelling,
   and association status before attempting either parse. There is no manual ECT override in this
   milestone.
2. Classify in strict order: a `099*` key is Overworld; otherwise matched ECT presence is Dungeon;
   otherwise a matched SCT and no ECT is Safe; otherwise the input is Unknown/View-only. Never use ECT
   parse success to decide whether an associated non-099 area is Dungeon.
3. Preserve Safe evidence independently from classification. Keys below `200a` are known traversable;
   keys at or above `200a` remain Safe candidates until traversability is validated.
4. For Dungeon only, read the associated ECT and call SpiceEct, which owns AKLZ detection/decompression
   and flat ECT parsing. Convert usable tables, rates, rows, identity, status, and diagnostics immediately
   into SAVOR-owned data. No SpiceEct type crosses into Qt, workflow payloads, or persistence contracts.
5. Apply the current authored-selector hypothesis to GRND and ground-role GOBJ triangle metadata and
   associate each result with its stable `NavigationTriangleKey`. Preserve interpretation confidence; do
   not union overlapping collision resources, because the runtime active resource and winning triangle
   select the applicable value.
6. Build a `NavigationDungeonEncounterModel` only when sufficient Dungeon encounter content is usable.
   ECT failure leaves the scenario classified Dungeon and preserves ordinary geometry/pathfinding while
   disabling only table-backed encounter results. Safe scenarios never receive an encounter model.

Profile classification, companion association, parser status, geometry completeness, traversability
evidence, and runtime-validation confidence are separate state dimensions. A successful parse or mesh
conversion must not implicitly strengthen any of the others.

The transient `BlenderIrScene` is destroyed after conversion and never crosses the `SavorNavigation`
boundary; JSON export and persistence remain optional SPICE validation tooling only. The precise axis,
sign, scale, matrix, and triangle-winding policy remains a first-prototype calibration task. All such
conversion must be centralized in `SavorNavigation`; Qt rendering code must not add independent fixes.

## World Model Components

## 1) Walkable Surface Model

- First-prototype source: `NavigationAreaModel` surfaces converted from SpiceMLD `world` and
  `searchWorld`, plus GOBJ blocks referenced by entry `groundAddresses`, for the manually selected MLD.
- Later semantic expansion: interpret the already matched SCT plus related package/controller data
  resolved from an area identity.
- Representation options:
  - Polygon adjacency graph (coarse).
  - 3D navmesh with portal transitions (preferred).
- Requirements:
  - Preserve vertical layering where projections overlap in X/Z.
  - Explicitly represent legal transitions between layers.
  - Preserve SPICE-provided link/connectivity metadata for inter-polygon movement.

### Current direction
- Use a true 3D graph/navmesh search (A* in 3D state space) rather than flattening to 2D.
- Preserve SpiceMLD walking-plane/link metadata while converting it into SAVOR navigation edge metadata.
- Treat `groundAddresses` as the role discriminator for GOBJ inclusion. Consume matching canonical
  `MldFile.groundResources`, apply entry and node hierarchy transforms in `SavorNavigation`, and exclude
  object-role-only GOBJ blocks from navigation surfaces.
- Preserve mesh vertex normals when present, generate missing normals, and retain available triangle
  metadata. The initial coordinate policy is identity pending visual calibration.
- Create bidirectional intramesh edges only for shared or tolerance-welded triangle boundaries. Keep
  stacked X/Z-overlapping surfaces disconnected unless an explicit MLD link and overlapping boundary
  geometry validate a directed cross-surface portal.
- For provisional links with multiple candidate surfaces, evaluate every geometrically possible pairing;
  if none yields a portal, emit a warning and leave the surfaces disconnected.

## 2) Collision Model

- Source: collision and walk-plane data converted into `NavigationAreaModel` for the selected area.
- MVP usage:
  - Coarse collision boundaries for global feasibility.
  - Follow-up worker jobs probe important collision regions and refine effective bounds using observed player coordinates.

### Current direction
- Preserve extracted collision geometry as source truth while attaching empirical validation observations;
  discrepancies lower runtime confidence and remain inspectable rather than silently rebaking the mesh.
- Render exact `fxn=wall` entries as their projected NJ object meshes. Missing or unusable wall geometry
  is diagnosed and makes the model partial; it is never replaced by a misleading cube marker.
- Treat `hasCompleteGroundGeometry` and `hasCompleteWallGeometry` strictly as conversion-completeness
  signals. Validation coverage separately identifies which surfaces, boundaries, and approach contexts
  have runtime evidence.

## 3) Interaction/Trigger Model

- First-prototype source: trigger/object information present in the selected MLD and exposed through
  `NavigationAreaModel`. Every SPICE-classified trigger currently receives its attached projected object
  meshes. Missing trigger geometry is warning-only and retains the approximate red cube marker so the
  trigger remains visible.
- Later source expansion: SpiceMLD/SpiceSCT target discovery across scripts, treasure chests, doors, load
  zones, and related area metadata.
- Types:
  - interaction prompts (chest/door/etc.)
  - load/zone transitions
  - cutscene start triggers
- The current model stores source function/entry/table identity, transform, object addresses, projected
  geometry, and projection completeness. It does not infer a player interaction radius or runtime
  activation rule from the mesh.
- A real projected trigger mesh may be selected as a manual route goal. The resolver uses the largest
  projected mesh under the deterministic world-space-bounds policy, anchors it to the nearest unambiguous
  walkable graph geometry, and exposes its bounds for a magenta AABB. The approximate red fallback cube is
  visibility-only and never goal-selectable.
- Later SCT/controller integration should add:
  - activation predicate
  - required flags
  - resulting state transitions

## 4) Provisional Moving-Object Model

- Exact normalized `motscpt` entries are promoted from the parser's preserved-unknown collection into a
  SAVOR-only `MovingObject` region kind and rendered in a separate `MovingObjects` layer.
- The current model retains source identity, transform, payload size, projected mesh geometry, provenance,
  and completeness diagnostics. Missing geometry is warning-only and retains an approximate orange cube.
- The name is deliberately provisional: some entries may be doors, but this slice does not infer door
  identity, open/closed state, animation, collision changes, or activation behavior.

## 5) Script/Controller and Start-Catalog Model

- Active slice: parse the one filename-matched `.sct`, preserve its source and complete private parse
  result, and expose normalized section/instruction summaries plus all opcode-77 occurrences. SPICE's
  generic relative `CallSubscript` targets and typed numeric literals must be correct and regression-tested
  before the vendored pin advances.
- An opcode-77 occurrence is resolvable only when ground selector and XYZ reduce to finite constants; yaw
  is retained when known and supplies scripted start facing, but is not required by A*. Sentinel, dynamic,
  malformed, or otherwise incomplete placements remain catalogued and unavailable.
- Exact `init` placements, placements reached from `init`, and `loop` placements under the
  `BitVar 1910 == 0` initialization path are `AuthoredArrival`; other occurrences are lower-confidence
  `ScriptedReposition` options. Group only bit-exact placements of the same kind and retain every path as
  a variant.
- Preserve raw `IntVar 15` values, label `10000` as Battle Return and `20000` as Save Load, and derive a
  previous-area stem only when matching sibling filenames corroborate the conversion.
- Opcode 156 is not catalogued because the source of its restored position/rotation is unresolved. Later
  work will evaluate conditions against runtime state, model restoration data, and apply trigger,
  cutscene, and transition semantics.

## 6) Coordinate Policy

- `NavigationCoordinatePolicy` is the sole location for axes, signs, scale, transform, normal, winding,
  and up-axis decisions.
- The first pathfinding slice retains the current identity policy while calibration remains open.
- MLD geometry, graph construction, Qt rendering, and opcode-77 positions/facing must all use the same
  policy; no renderer-local correction is permitted.

## 7) Shared Runtime-Evidence Model

Collision validation and movement-anomaly discovery are shared by Dungeon and future Safe Navigation.
They operate on the same SAVOR-owned geometry and stable identities but do not change its parse or
conversion status.

- Collision observations retain the expected contact, observed contact/response, world position,
  ground/resource identity, source provenance, approach direction, and input/camera context.
- Validation coverage is granular by tested surface, wall boundary, and approach. Untested geometry is
  complete-but-unvalidated, not validated by implication.
- Discrepancies remain first-class evidence and lower confidence for affected route segments. Corrected
  effective bounds, if later introduced, must be a separate derived layer rather than an untracked mesh
  mutation.
- Movement-anomaly candidates are generated around validated or explicitly low-confidence corners, seams,
  slopes, and boundaries. Each candidate records approach pose/facing, camera and input sequence, baseline
  and observed motion, VI-frame cost, route position, repeat count, variance, and outcome classification.
- Sticky, unusual, or divergent collision behavior is not assumed beneficial. Only reproducible measured
  speedups may later become planner actions or edge-cost adjustments.
- `SavorNavigation` owns candidate generation and result interpretation. Dolphin-backed probes use
  `SavorCore` for memory/input/checkpoint telemetry and `SavorWorker` for isolated execution; worker
  failure yields no empirical claim.

## 8) Optional Dungeon Encounter Model

The encounter model is static, optional, Dungeon-only analysis data attached to the scenario. SpiceEct
provides parsing, while `SavorNavigation` owns all semantics and public types.

- The first-pass authored-selector hypothesis derives a value for each GRND or ground-role GOBJ triangle
  with usable metadata from `triangleMetadata.rawU16[2] & 0x7fff`; the mask removes the strip-winding bit,
  and the decimal tens digit is treated as the dungeon encounter-table selector. Preserve this
  interpretation's evidence/confidence until it is validated beyond the current Catacombs evidence.
- Selector zero means no encounter-distance accumulation. Nonzero supported selectors identify candidate
  flat ECT tables. Missing metadata is Unknown, and unsupported or out-of-range selectors remain explicit
  incomplete data rather than being coerced to zero.
- Every selector record is keyed by `NavigationTriangleKey`. Surface `tblId` remains collision-resource
  identity, not an encounter-table ID.
- When resources overlap, do not union their selectors. Runtime exposure depends on the active collision
  resource and winning triangle, which require observation or explicit input.
- Static route exposure can report selector/table segments and eligible distance. It may report marginal
  risk only when an explicit encounter start state and check cadence are supplied, predicted/seeded
  outcomes only from a complete evidence-qualified `SavorPredict` model, and observed/validated outcomes
  only from suitable runtime evidence.
- A manual or opcode-77 spatial start never silently supplies encounter step counter, movement
  accumulator, modifier, suppressor, table-set, cadence, or RNG state.
- The first encounter slice is analysis-only and does not alter deterministic A*. Safe has no encounter
  model, encounter cost, or encounter-suppression requirement.

## 9) Predictor Result and Reachability Model (Planned)

Predictor-backed outcome planning is not part of the static encounter model or Navigation widget. A
separate future planning module in `SavorQt` authors an objective and bounds, selects a world/start
by explicitly referencing a ready `NavigationContextResult`, invokes `SavorPredict` through an
asynchronous boundary, compares results, and chooses one for display. `SavorPredict` owns planning-level
field-update/RNG state evolution, path and movement/no-movement/interruption schedule search, objective
evaluation, witness selection, and frontier computation. It does not own raw stick/camera mapping.

The immutable prediction result contains:

- schema/result/run identity and objective (`NoEncounter`, `SpecificEncounter`, or a later supported kind);
- game, area/profile, MLD/SCT/ECT content identity, navigation-world/geometry/graph hashes, and
  coordinate-policy version;
- exact `NavigationContextResult` identity, anchor, reset-boundary provenance, and captured state fields
  required by the selected versioned model bundle;
- predictor build and model-bundle identity with per-component evidence/completeness;
- explicit search bounds such as update horizon, pause budget, allowed path set, node/time budget, and a
  completion state such as complete-within-bounds, budget-exhausted, cancelled, or model-incomplete;
- one selected witness path and complete movement/no-movement/interruption schedule reference plus an
  ordered spatial/RNG trace reference;
- `coverageKind = RoutePrefix | SurfaceSet`, with stable triangle/local-coordinate coverage that does not
  mark an entire triangle reachable merely because one witness crossed it;
- frontier geometry, last confirmed objective-satisfying state, optional next terminal-event position,
  terminal reason, predicted encounter/formation when applicable, metrics, and diagnostics.

A fixed selected path produces a reachable prefix and cutoff. A branching path search produces a
layer-preserving 2.5D reachable set and may have multiple frontier edges; a single "furthest" point exists
only under a declared progress or optimization metric. For a no-encounter objective, retain both the last
confirmed safe state and the subsequent predicted encounter when known. For a specific-encounter
objective, retain target-hit locations rather than mislabeling them as a safety frontier.

The full schedule remains audit/replay provenance. The Navigation widget consumes only the already
computed spatial coverage/frontier and selected witness projection. It must not treat a search-budget or
prediction-horizon boundary as universally impassable, or a sampled schedule frequency as natural
encounter probability. The whole-area static selector/table overlay remains an independent layer.

A same-script cutscene that returns control stays inside the same `NavigationEpoch`. A predictor that
cannot model it returns `ModelIncomplete`; it never treats the post-cutscene state as a fresh context.
Entering battle terminates the prediction epoch. Any later field prediction must reference the separate
context captured after the reset-qualified battle return. The detailed field-RNG model remains a versioned
completeness dependency; unfinished Moonfish/field analysis supplies no behavioral assumptions here.

## Planner State (Route Layer)

Proposed route-planner state tuple:

`S_route = {surface_or_node, position_proxy, movement_mode, scenario_flags, cutscene_state}`

Where:
- `surface_or_node`: nav graph anchor.
- `position_proxy`: point or local param on edge/portal.
- `movement_mode`: normal, interaction, forced-move, etc.
- `scenario_flags`: doors/chests/cutscene gate flags.
- `cutscene_state`: inactive, active(cutscene_key), post-transition.

> Camera is intentionally excluded from route-planner state.

## Control-Solver State (Execution Layer)

A downstream solver layer receives route splines and computes camera+player inputs:
- instruction to load/consume route spline
- iterative follow-and-correct loop with savestate checkpoints
- emit best input tape candidate and telemetry

## Transition Classes

1. Walk transitions (route-layer controllable, same epoch)
2. Interaction transitions (button-gated, same epoch unless they cause a qualifying field switch)
3. Same-script cutscene transitions (script-driven, same epoch and no `stepCount` reset)
4. Field script/context switches (new epoch only after control/readiness validation)
5. Battle entry (terminates the epoch) and battle return (candidate new reset-qualified context)

Each transition stores:
- preconditions
- expected VI cost (MVP coarse estimate)
- post-state mapping
- epoch effect (`Continue`, `Terminate`, or `BeginAfterValidatedReset`)

## Data Artifacts (Draft)

- `NavigationAreaModel` (in memory for the interactive prototype):
  - source identity and bounds
  - transformed GRND and ground-role GOBJ meshes with stable entry/block/node source keys
  - surface links/adjacency
  - collision, trigger, and provisional MovingObject geometry plus available source metadata
  - SAVOR-owned wall, trigger, and MovingObject mesh instances with separate geometry-completeness diagnostics
  - normalized parse/conversion diagnostics
- `NavigationScriptModel` (in memory for the interactive prototype):
  - strictly matched source identity and association/load status
  - normalized section/instruction summaries and diagnostics
  - SAVOR-owned opcode-77 start options, variants, conditions, and provenance
  - opaque private retention of the full SpiceSCT parse result
- `NavigationScenarioModel` (in memory for the interactive prototype):
  - area model, optional matched script, traversal graph, and combined diagnostics
- Planned profile and companion evidence (in memory):
  - ordered `NavigationAreaProfile`, complete-set provenance, expected/present companion identities, and
    association status kept separately from each parser's load/parse status
- Planned `NavigationCollisionValidationModel` and `NavigationMovementAnomalyModel` (shared, optional):
  - source-keyed runtime observations, coverage/confidence, discrepancies, reproducibility, and diagnostic
    evidence without mutating geometry-completeness flags
- Planned `NavigationDungeonEncounterModel` (Dungeon only, optional):
  - SAVOR-owned ECT tables/rates/rows plus selector records keyed by `NavigationTriangleKey`
  - explicit unknown/unsupported/ambiguous states and structural or assumed/marginal evidence level
- Planned `NavigationPredictionResult` artifact family (selected result, not static scenario state):
  - compact objective/provenance/coverage/frontier summary
  - separate large ordered prediction trace, witness planning schedule, and optional multi-path
    reachability field references
  - derived compatibility state (`Current`, `StaleContext`, `Incompatible`, or `Unverifiable`) rather than a
    mutable stale flag stored in the immutable artifact
- Planned Navigation Context workflow artifacts:
  - immutable `NavigationContextResult` explicitly referenced by every prediction
  - disc identity, internal-file manifest, and versioned `NavigationContentBundle`
  - patched exploration observation sets and separate `NavigationWorldRefinement` layers
  - `NavigationControlSolveResult` with controller/camera realization, and
    `NavigationValidationResult` from an unpatched clean runtime
- `NavigationPathResult` (in memory for the interactive prototype):
  - anchored endpoints, route status, deterministic portal path/polyline, cost/length, and diagnostics
- Endpoint visualization state (in memory in `SavorQt3D`):
  - compact fixed-size start and ground-goal markers at the `man` fallback scale
  - manual or opcode-77 start facing rendered as a short ray
  - selected projected-trigger goal identity and magenta world-space AABB
- `nav_world_blob`:
  - deferred SAVOR-owned serialization of the navigation model for later workflow integration
- `nav_script_index`:
  - deferred decoded script section map and transition hints
- `nav_transition_catalog`:
  - cutscene/interaction transition outcomes
- `nav_route_candidate`:
  - graph path + spline + metadata

## Validation Strategy

- 3D viewer overlay of reconstructed world model.
- Overlay selected route spline and objectives.
- Compare at least one known field area against SPICE/Blender validation output to calibrate coordinate
  policy, but do not make that output a runtime dependency.
- Replay sampled route segments and compare predicted vs observed transitions.
- Record collision mismatches against their source geometry and lower affected runtime confidence without
  equating the observation with parser or mesh-conversion failure.
- Probe candidate collision anomalies repeatedly with preserved approach, input, camera, and timing
  context; classify positive, neutral, harmful, unresolved, and divergent results rather than assuming an
  unusual response is beneficial.
- For Dungeon encounters, validate active-resource/winning-triangle selection and authored selector
  behavior in at least one dungeon beyond the current Catacombs evidence before treating the decode as a
  universal field rule.
- For predictor overlays, verify that exact fingerprints project the expected route prefix or 2.5D frontier,
  spatial mismatches are rejected, stale-context results remain visibly historical, and budget/horizon
  termination is never displayed as a universal safety boundary.

## Load Failure Semantics

`NavigationAreaLoadResult` must distinguish and report:

- missing or unreadable input file
- empty input
- AKLZ decompression failure reported by SpiceMLD
- malformed or unsupported MLD content
- parse completion with warnings or incomplete navigation content
- SAVOR model-conversion failure
- nonconforming MLD identity, missing expected sibling SCT, and mismatched manually selected SCT
- unreadable/empty SCT, SpiceSCT AKLZ decompression failure, malformed/unsupported SCT content, and SCT
  parse warnings
- expected/present ECT association independently from unreadable/empty input, SpiceEct AKLZ decompression
  failure, malformed/partial/unsupported content, table-resolution gaps, and parse warnings
- missing triangle metadata, unsupported encounter selectors, unresolved/out-of-range tables, and
  ambiguous overlapping collision resources
- incomplete opcode-77 placements and start-resolution failures such as a missing ground `tblId`, tied
  matching surfaces, or an unusable traversal graph
- projected-trigger goal failures such as missing real mesh geometry, incomplete graph geometry, tied
  walkable surfaces, or anchoring failure
- unsupported prediction schema, spatial fingerprint mismatch, missing compatibility fingerprints,
  model-incomplete prediction, budget/horizon termination, missing witness/coverage artifacts, or invalid
  triangle/local-coordinate references

Warnings and preserved unknown entries should remain inspectable in the `SavorQt3D` diagnostics surface.
A failed load leaves the previously displayed model intact. A parse that yields some usable ground
resources may replace it with a clearly marked partial model for visual diagnosis. Missing expected wall
geometry likewise produces a partial model. Missing expected trigger geometry produces a warning and an
approximate cube marker but does not change load status; that cube is not goal-selectable. MovingObjects
use the same advisory fallback.
Pathfinding stays disabled unless both ground and wall geometry are complete; trigger and MovingObject
completeness remain advisory until activation/motion semantics are modeled. Selecting projected trigger
geometry as a goal does not imply its runtime activation rule. SCT absence or failure is
also advisory: the user may still choose manual route endpoints. Incomplete catalog entries remain
inspectable but unavailable, and failed resolution preserves the current valid start. A manual SCT mismatch
never replaces an already valid association.

A matched ECT's presence classifies a non-099 scenario as Dungeon even when ECT loading or parsing fails;
the failure disables table-backed encounter analysis but does not disable ordinary pathfinding. Missing
ECT may classify a matched-SCT non-099 scenario as Safe only when the selected directory represents the
complete companion set. Below-`200a` Safe content retains known-traversable evidence, while later keys
remain candidates rather than being promoted by filename alone. Safe means no random encounters, not no
scripted interruptions.

Geometry completeness and runtime confidence never substitute for one another. Complete ground/wall
conversion can remain unvalidated or contradicted at runtime; incomplete conversion remains incomplete
even if a limited probe succeeds. Worker timeout, divergence, or absent telemetry creates no empirical
collision, anomaly, or encounter claim and does not overwrite structural evidence. Unknown encounter
state or cadence blocks marginal exposure, while unsupported selectors or unresolved active resources
remain incomplete rather than silently meaning no encounter. Area 99 is always Overworld/deferred and is
never decoded using Dungeon encounter semantics.

Prediction failures never invalidate the underlying scenario, static encounter overlay, or ordinary route.
An incompatible result is not rendered; an unverifiable or stale-context result may remain inspectable only
with an explicit status. A failed replacement preserves the previously selected compatible overlay.
