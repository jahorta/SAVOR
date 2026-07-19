# Navigation Phase Planning

## Status

Future plan.

SAVOR owns navigation workflow orchestration, route planning, control solving, simulator execution, UI
selection, and result persistence. SPICE (Skies Package Interchange and Content Encoder) owns Skies of
Arcadia filetype parsing and content inspection. SAVOR will vendor SPICE as a pinned git submodule at
`third-party/SPICE`, currently pinned to SPICE commit `8ebdf50`.

The dependency boundary is:

`third-party/SPICE -> SavorNavigation -> SavorQt3D prototype host -> eventual SavorQt integration`

`SavorNavigation` is a non-Qt C++20 static library. It is the only SAVOR project allowed to include
SpiceMLD types. It converts SpiceMLD parse output into SAVOR-owned navigation types so the Qt projects,
planner, and future persistence code do not depend on the parser's internal model.

This folder tracks planning documents for a new **Navigation Phase** focused on moving between world objectives in Skies of Arcadia (e.g., chest, door, loading zone) while minimizing total completion time in VI frames.

## Goals

- Load a manually selected, AKLZ-compressed MLD through SpiceMLD and construct an in-memory,
  SAVOR-owned representation of the area's walkable space.
- Plan objective-to-objective movement with a frame-time cost function.
- Account for interruptions (cutscenes, forced transitions, camera shifts).
- Refine candidate input tapes in simulator for time-optimal results.
- Integrate into existing SAVOR job/phase infrastructure.
- Render a 3D area view highlighting walking planes and potential navigation targets such as script
  triggers, treasure chests, doors, and load/zone transitions.

## First implementation milestone

The interactive prototype will:

1. Use a file picker to select an `.mld` file and remember the last directory.
2. Read and parse the selected file asynchronously through `SavorNavigation` and SpiceMLD.
3. Let SpiceMLD detect and decompress AKLZ data; SAVOR will not implement a second decompressor or MLD
   parser.
4. Parse a canonical SpiceMLD `MldFile`, then convert its ground resources plus a transient compatibility
   projection into an in-memory `NavigationAreaModel`. GRND and ground-role GOBJ data come directly from
   `MldFile.groundResources`; exact `fxn=wall` NJ object geometry is flattened through an in-memory
   `BlenderIrScene` and immediately converted to SAVOR-owned collision meshes. GOBJ blocks referenced only
   as objects are not walkable surfaces.
5. Render the model and later path overlays in a reusable Navigation widget hosted by `SavorQt3D`.

## Implemented first slice (2026-07-18)

- SPICE is pinned at `8ebdf50` under `third-party/SPICE`.
- `SavorNavigation` loads compressed MLD files, owns the public model and diagnostics, applies the
  centralized identity coordinate policy, and marks incomplete ground decoding as a partial model that
  is not pathfinding-ready.
- `SavorQt3D` loads through a file picker on a background worker, preserves the last directory, and
  renders GRND and ground-role GOBJ surfaces, real `fxn=wall` collision-boundary meshes, and the remaining
  collision, trigger, and unknown markers.
- `a101b.mld` is the first fixture contract: 6 GRND surfaces and 6 ground-role GOBJ surfaces (504 vertices
  and 401 triangles total), 59 collisions including 51 exact wall regions projected as 517 mesh
  instances (6,795 vertices and 8,347 triangles), 16 triggers, and 32 preserved unknown entries.
- Link rendering, start/target selection, and path search are intentionally deferred to the next slice.

The local US disc dump at
`D:\SoAGC\2002-12-19-gc-us-final_Skies_of_Arcadia_Legends` is a development fixture and convenient
initial directory. It must not become a hardcoded default or checked-in configuration value.

The first milestone does not require Dolphin/ISO file acquisition, automatic area lookup, a serialized
SPICE area-view artifact, or durable navigation persistence.

## Documents

- `01-scope-and-success-criteria.md`
  - Problem statement, non-goals, and measurable success criteria.
- `02-state-and-world-model.md`
  - Proposed navigation state representation and geometry/cutscene modeling.
- `03-search-and-optimization-strategy.md`
  - Multi-stage planning strategy (A* + simulator refinement).
- `04-phase-and-job-integration.md`
  - Proposed phase contracts, step kinds, and artifacts in existing system.
- `05-open-implementation-questions.md`
  - Unresolved decisions and experiments to de-risk implementation.

## Iteration approach

These are initial planning docs with open implementation details. We should expect to revise aggressively as we prototype extraction, heuristics, and determinism checks.

`SavorQt3D` is the standalone development and test host for the reusable widget. Its existing Quick 3D
renderer, file picker, visibility controls, and diagnostics are prototype assets to retain. Its dependency
on the removed `SavorMLD` project was obsolete and has been replaced by `SavorNavigation`. Once the widget
and model boundary are stable, the widget will be installed into `SavorQt`.

## Ownership Boundary

- SPICE owns SoA package/file parsing, including AKLZ decompression, MLD/SCT reads, low-level
  GRND/GOBJ model extraction, walking-plane candidate generation, and target discovery from
  script/object content.
- `SavorNavigation` owns the adapter from SpiceMLD results into SAVOR navigation semantics, coordinate
  policy, route/search types, and future persisted schema.
- `SavorQt3D` owns the prototype host and reusable Navigation widget; `SavorQt` is the eventual product
  host.
- SAVOR owns workflow launch, route/search/control-solver jobs, simulator validation, UI target selection,
  and future persistence.
- SA3DPort planning has been retired from this repo. Any parser/reference-comparison work belongs behind
  SPICE.
