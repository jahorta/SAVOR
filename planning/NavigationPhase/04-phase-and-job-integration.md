# 04 - Phase and Job Integration

## Status

Future plan. The Navigation work is split into an interactive prototype milestone and a later workflow/job
integration milestone. SPICE owns file parsing; `SavorNavigation` owns SAVOR's navigation model and
adapter; `SavorQt3D` is the first UI host.

## Goal

Establish the model and reusable widget in a small standalone host before adding Navigation jobs,
persistence, and the widget to `SavorQt`.

## Milestone 1 - Interactive Prototype

### Dependency layout

`third-party/SPICE -> SavorNavigation -> SavorQt3D`

- Pin SPICE as a git submodule at `third-party/SPICE`, currently at commit `8ebdf50`.
- Add `SavorNavigation` as a non-Qt C++20 static library.
- Only `SavorNavigation` may include or expose knowledge of SpiceMLD implementation types.
- The existing `SavorQt3D` application now links to `SavorNavigation`; its obsolete dependency on the
  deleted `SavorMLD` project has been removed.
- Preserve useful `SavorQt3D` prototype assets: file picker, Quick 3D scene, layer controls, diagnostics,
  and last-directory behavior.

### Interactive input and data flow

1. The user chooses an `.mld` with the `SavorQt3D` file picker.
2. The picker remembers the last directory. The known US disc dump is a local development fixture only
   and is never hardcoded into source or project settings.
3. Loading runs off the UI thread.
4. `SavorNavigation` reads the selected compressed file and calls SpiceMLD.
5. SpiceMLD performs AKLZ detection/decompression and MLD parsing.
6. `SavorNavigation` converts canonical `MldFile` GRND/GOBJ resources plus projected `world` and
   `searchWorld` evidence into `NavigationAreaModel`. A transient, non-exported Blender IR projection is
   flattened into SAVOR-owned meshes for exact `fxn=wall` collision regions and every SPICE-classified
   trigger plus exact normalized `motscpt` MovingObjects; object-role-only GOBJ blocks remain excluded from
   walkable surfaces.
7. The UI thread replaces the displayed model after a complete or usable partial load. Partial models are
   visibly diagnosed and never treated as pathfinding-ready.

The first milestone does not use Dolphin/ISO acquisition, automatic area-ID lookup, a workflow job, or a
durable area artifact. The `NavigationAreaModel`, selected path, and overlays remain in memory.

### Prototype failure classes

- selected file missing, unreadable, or empty
- AKLZ decompression failure
- malformed or unsupported MLD content
- parse warnings or incomplete navigation content
- missing or unusable exact-wall object trees, meshes, triangles, or weighted roots
- missing or unusable trigger object trees, meshes, triangles, or weighted roots; this is warning-only and
  retains an approximate red cube marker rather than making the load partial
- missing or unusable MovingObject geometry; this is also warning-only and retains an approximate orange
  cube marker
- conversion into `NavigationAreaModel` failed
- renderer rejected otherwise valid navigation geometry

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
- Preserve the source entry/block/node identity and report provisional link evidence without presenting
  it as navigable adjacency.
- Validate `a101b.mld` as 12 surfaces (6 GRND and 6 ground-role GOBJ), 504 vertices, 401 triangles,
  59 collisions including 51 wall regions and 517 wall meshes (6,795 vertices, 8,347 triangles),
  16 fully projected triggers with 38 meshes (320 vertices, 384 triangles), 11 fully projected `motscpt`
  MovingObjects with 55 meshes (462 vertices, 566 triangles), and 21 remaining unknown entries.
- Report unreadable/empty files, AKLZ decompression errors, parser diagnostics, and incomplete ground
  content without exposing SpiceMLD types to Qt.

### Prototype acceptance

- Open a compressed US field MLD directly from the disc dump without an external decompression step.
- Display SAVOR-owned surfaces, collision/trigger information, and parser/conversion diagnostics.
- Keep attached trigger geometry distinct from future SCT/controller activation predicates and interaction
  radii.
- Toggle model layers using the existing visibility controls.
- In the next slice, render derived links, select or supply two positions, and display an initial path
  overlay over the in-memory model.
- Keep SpiceMLD and Qt types out of the `SavorNavigation` public model.

## Milestone 2 - Workflow Integration

After the model and widget boundary are validated:

- Add automatic area/content identity resolution rather than requiring a manual file picker.
- Add related SCT/package content needed for target and transition discovery.
- Define and persist a SAVOR-owned `nav_world_blob` schema.
- Move the reusable Navigation widget into `SavorQt` while retaining `SavorQt3D` as a focused development
  harness if it remains useful.
- Add Navigation program/step kinds, solver fanout, route artifacts, and publishing.

Optional SPICE diagnostic exports, including Blender IR or serialized area views, may be used for parser
and rendering comparison. They are not a runtime input contract and must not be required to load or plan
an area.

## Versioning Decision

- GameCube Skies of Arcadia has three versions; only the US build is currently supported by the relevant
  addresses and breakpoints.
- Future persisted asset keys include Game ID plus area/content identity.
- The manual prototype records the selected source path for diagnostics but does not treat a machine-local
  path as a durable identity.

## Proposed Program Kind (Later Milestone)

Draft identifiers:

- `program_kind`: `navigation_phase`
- step kinds:
  - `nav.build_world`
  - `nav.plan_global`
  - `nav.solve_controls`
  - `nav.publish_result`

### `nav.build_world`

Inputs:

- source MLD/content artifact reference
- game ID and resolved area identity
- navigation model-build options

Outputs:

- SAVOR-owned `nav_world_blob`
- optional `nav_script_index`
- parse and conversion diagnostics

### `nav.plan_global`

Inputs:

- start and target descriptors
- `nav_world_blob`
- planning configuration

Outputs:

- top-K route skeletons
- route splines

### `nav.solve_controls`

Inputs:

- route splines
- solver configuration

Outputs:

- executable input-tape candidates
- solver telemetry, including deviation, frame counts, and trigger hits

This work can fan out by route candidate, segment, or a later selected hybrid policy.

### `nav.publish_result`

Inputs:

- selected successful candidate

Outputs:

- canonical route artifact
- summary metrics
- optional replay package

## Draft Later-Milestone Payload Fields

- `seed_snapshot_path`
- `source_artifact_id`
- `game_id`
- `area_id`
- `start_descriptor`
- `target_descriptor`
- `planner_config_json`
- `solver_config_json`

## Draft Later-Milestone Result Fields

- `status_code`
- `objective_reached`
- `total_vi_frames`
- `route_artifact_id`
- `input_tape_artifact_id`
- `diagnostics_artifact_id`

## Draft Later-Milestone Artifact Inventory

- `nav_world_blob.bin`
- `nav_script_index.bin`
- `nav_route_candidates.jsonl`
- `nav_splines.bin`
- `nav_best_input_tape.bin`
- `nav_run_report.json`

## UI/Visualization Integration

- Develop the reusable Navigation widget in `SavorQt3D` first.
- Render `NavigationAreaModel` geometry, walking planes, links, collision/trigger layers, diagnostics, and
  path overlays.
- Add start/target selection after basic model loading and visibility are stable.
- Install the widget into `SavorQt` only after its public model boundary is stable.
- Navigation remains UI-first; no CLI objective-entry path is planned for MVP.

## Remaining Integration Questions

1. Worker fanout by route candidate, segment, or hybrid.
2. Durable `nav_world_blob` and `nav_script_index` schemas.
3. Automatic mapping from game/area identity to the full set of required MLD/SCT/package files.
4. Coordinate-policy calibration and validation fixtures.
