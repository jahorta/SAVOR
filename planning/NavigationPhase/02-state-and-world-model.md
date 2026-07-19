# 02 - State and World Model

## Status

Future plan. Low-level SoA file parsing is delegated to the pinned SPICE submodule. This document
describes the SAVOR-owned model built in process by `SavorNavigation` from SpiceMLD output.

## Objectives

Define a planning state representation that supports:
- Overlapping walk surfaces in Y.
- Obstacle and trigger interactions.
- Cutscene interrupts and repositioning.
- Route-planning independent from camera implementation details.

## Library Boundary and Load Result

`SavorNavigation` is a non-Qt C++20 static library and the only SAVOR project that includes SpiceMLD
headers. Its initial public concepts are:

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
  - Contains no Qt or SpiceMLD types.

The interactive prototype keeps this result in memory. The file picker and last-directory setting belong
to `SavorQt3D`; file reading, parsing, conversion, and coordinate policy belong to `SavorNavigation`.

### Parse and conversion flow

1. `SavorQt3D` selects an `.mld` path and starts an asynchronous load.
2. `SavorNavigation` reads the file and calls SpiceMLD.
3. SpiceMLD detects and decompresses AKLZ data, then produces canonical `MldFile`.
4. `SavorNavigation` converts canonical GRND/GOBJ resources plus `world` and `searchWorld` evidence into
   `NavigationAreaModel`. It transiently flattens the compatibility projection's exact `fxn=wall` and
   SPICE-classified trigger object trees plus exact normalized `motscpt` object trees into world-space
   SAVOR meshes, including hierarchical transforms, repeated instances, and weighted bind-pose roots.
5. The UI thread receives only the SAVOR model and its diagnostics. A complete model is pathfinding-ready;
   a partial model may be rendered for diagnosis but is explicitly not pathfinding-ready.

The transient `BlenderIrScene` is destroyed after conversion and never crosses the `SavorNavigation`
boundary; JSON export and persistence remain optional SPICE validation tooling only. The precise axis,
sign, scale, matrix, and triangle-winding policy remains a first-prototype calibration task. All such
conversion must be centralized in `SavorNavigation`; Qt rendering code must not add independent fixes.

## World Model Components

## 1) Walkable Surface Model

- First-prototype source: `NavigationAreaModel` surfaces converted from SpiceMLD `world` and
  `searchWorld`, plus GOBJ blocks referenced by entry `groundAddresses`, for the manually selected MLD.
- Later source expansion: related SCT and package data resolved from an area identity.
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

## 2) Collision Model

- Source: collision and walk-plane data converted into `NavigationAreaModel` for the selected area.
- MVP usage:
  - Coarse collision boundaries for global feasibility.
  - Follow-up worker jobs probe important collision regions and refine effective bounds using observed player coordinates.

### Current direction
- Maintain a rebake step that updates collision bounds from empirical probe results.
- Render exact `fxn=wall` entries as their projected NJ object meshes. Missing or unusable wall geometry
  is diagnosed and makes the model partial; it is never replaced by a misleading cube marker.

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

## 5) Script/Controller Model

- Consume SPICE `.SCT` and object-controller analysis to model trigger and cutscene behavior.
- For cutscenes, identify script section boundaries and jump targets to build transition catalog entries.

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

1. Walk transitions (route-layer controllable)
2. Interaction transitions (button-gated)
3. Cutscene transitions (script-driven)
4. Spawn/reposition transitions (post-cutscene / load)

Each transition stores:
- preconditions
- expected VI cost (MVP coarse estimate)
- post-state mapping

## Data Artifacts (Draft)

- `NavigationAreaModel` (in memory for the interactive prototype):
  - source identity and bounds
  - transformed GRND and ground-role GOBJ meshes with stable entry/block/node source keys
  - surface links/adjacency
  - collision, trigger, and provisional MovingObject geometry plus available source metadata
  - SAVOR-owned wall, trigger, and MovingObject mesh instances with separate geometry-completeness diagnostics
  - normalized parse/conversion diagnostics
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
- Flag mismatches for model correction/rebake.

## Load Failure Semantics

`NavigationAreaLoadResult` must distinguish and report:

- missing or unreadable input file
- empty input
- AKLZ decompression failure reported by SpiceMLD
- malformed or unsupported MLD content
- parse completion with warnings or incomplete navigation content
- SAVOR model-conversion failure

Warnings and preserved unknown entries should remain inspectable in the `SavorQt3D` diagnostics surface.
A failed load leaves the previously displayed model intact. A parse that yields some usable ground
resources may replace it with a clearly marked partial model for visual diagnosis. Missing expected wall
geometry likewise produces a partial model. Missing expected trigger geometry produces a warning and an
approximate cube marker but does not change load status; MovingObjects use the same advisory fallback.
Pathfinding stays disabled unless both ground and wall geometry are complete; trigger and MovingObject
completeness remain advisory until activation/motion semantics are modeled.
