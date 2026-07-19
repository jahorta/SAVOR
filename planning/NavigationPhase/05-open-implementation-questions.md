# 05 - Open Implementation Questions

## Status

Future plan. File parsing questions are SPICE-owned. This document tracks resolved SAVOR integration
decisions and the remaining navigation-model, pathfinding, workflow, and UI questions.

## Resolved Decisions

### A. Dependency and ownership boundary (2026-07-18)

1. Vendor SPICE as a pinned git submodule under `third-party/SPICE`, currently at commit `8ebdf50`.
2. Add `SavorNavigation` as a non-Qt C++20 static library.
3. Only `SavorNavigation` may include SpiceMLD types. It converts the parser result into SAVOR-owned
   `NavigationAreaModel` and normalized diagnostics.
4. SAVOR owns navigation semantics, pathfinding, UI, workflow integration, and future persistence. SPICE
   owns AKLZ decompression, MLD/SCT parsing, and low-level content interpretation.
5. There is no SAVOR-side fallback parser or decompressor.

### B. First input path and model lifetime (2026-07-18)

1. The interactive prototype selects an AKLZ-compressed `.mld` with a file picker and remembers the last
   directory.
2. The US disc dump at
   `D:\SoAGC\2002-12-19-gc-us-final_Skies_of_Arcadia_Legends` is a local development fixture only. It is
   not a hardcoded default, portable configuration value, or durable area identity.
3. Loading and conversion run off the UI thread; the completed model is applied on the UI thread.
4. SpiceMLD automatically detects and decompresses AKLZ data before parsing.
5. The runtime path parses canonical `MldFile` once, then consumes its ground resources plus compatibility
   `world` and `searchWorld` evidence.
6. Exact `fxn=wall` NJ object geometry is flattened through a transient in-memory Blender IR projection
   inside `SavorNavigation`. JSON export, persistence, and exposure to Qt remain prohibited; Blender IR
   otherwise remains optional SPICE validation tooling.
7. `NavigationAreaModel` and path overlays remain in memory during the interactive prototype. A durable
   `nav_world_blob` is deferred to workflow integration.
8. Serialized SPICE area views are not a required runtime or first-pass artifact contract.

### C. Prototype host and widget rollout (2026-07-18)

1. Revive `SavorQt3D` as a separate development and test application before installing the widget in
   `SavorQt`.
2. Retain its useful Quick 3D renderer, file picker, layer controls, diagnostics, and last-directory
   behavior.
3. Its obsolete dependency on the removed `SavorMLD` project has been replaced with `SavorNavigation`.
4. Build the viewer as a reusable Navigation widget inside the prototype host so it can later move into
   `SavorQt` without exposing SpiceMLD types.

### D. Geometry semantics

1. Use true 3D search state; do not flatten overlapping Y layers to 2D.
2. Preserve SpiceMLD link/connectivity metadata while generating SAVOR navigation edges.
3. Keep coordinate conversion centralized in `SavorNavigation`; renderer-specific axis, winding, or scale
   patches are not allowed.
4. Start with coarse collision fidelity, then use worker probes to refine important bounds in later
   milestones.
5. A GOBJ block is part of the navigation surface set when referenced through an entry's
   `groundAddresses`. Consume it from canonical `MldFile.groundResources`, preserve its entry/block/node
   source identity, and exclude object-role-only GOBJ blocks.
6. A partially decoded ground set may be rendered for diagnosis, but it sets
   `hasCompleteGroundGeometry=false` and cannot enter path search.
7. Exact normalized `fxn=wall` entries use transiently projected object trees to create world-space
   `NavigationRegionMesh` instances. Missing wall geometry sets `hasCompleteWallGeometry=false`, makes the
   load partial, and suppresses the old cube fallback. `walluv` remains an ordinary marker in this slice.
8. Every SPICE-classified trigger receives the same projected object-tree treatment. Missing trigger
   geometry sets `hasCompleteTriggerGeometry=false` and increments `failedTriggerRegionCount`, but it is
   warning-only, does not alter load status, and retains an approximate red cube marker. The mesh is
   attached MLD geometry, not an inferred interaction radius or SCT/controller activation predicate.
9. Exact normalized `motscpt` entries are provisionally reclassified from preserved unknown entries as
   `MovingObject` regions and projected through the same path. They render in a separate orange
   `MovingObjects` layer. Missing geometry is warning-only with a cube fallback and does not affect load
   status. This name does not assert that every entry is a door or define runtime motion/collision behavior.

### E. Camera and controls

- Camera is not part of route-planning state.
- The later execution pipeline is route -> spline -> control-solver workers -> candidate selection.
- Add VM instructions for spline ingestion and iterative following with savestate checkpoints after the
  interactive pathfinding milestone.

### F. Cutscenes and optimization

- Use MLD trigger/flag combinations and later SCT analysis for likely cutscene starts and transitions.
- Treat cutscenes as non-branching for the current game scope.
- MVP uses baseline optimization and assumes deterministic replay from fixed savestate/input pairs.
- Add repeated validation only if failures are observed.

## Remaining Open Questions

1. Exact `CoordinatePolicy` for axes, signs, scale, matrix orientation, and triangle winding after visual
   calibration against known field areas.
2. Minimum durable schemas for SAVOR `nav_world_blob` and `nav_script_index`.
3. Automatic mapping from game/area identity to the required MLD, SCT, and related package files.
4. SCT/controller integration needed to turn first-pass MLD triggers into named objectives and transition
   outcomes.
5. Worker fanout strategy for control solving: segment, route candidate, or hybrid.
6. Heuristic for prioritizing collision regions for probe/refinement jobs.
7. Final start/target interaction model after basic model loading and layer inspection are usable.

## Implemented First Slice (2026-07-18)

- SPICE is pinned at `8ebdf50`; `SavorNavigation` is the sole SpiceMLD consumer in SAVOR.
- Direct AKLZ loading, normalized failure diagnostics, GRND plus ground-role GOBJ conversion, partial-model
  signaling, background Qt loading, and the retained viewer layers are implemented.
- The `a101b.mld` fixture contract is covered by an isolated navigation test target.
- Exact wall projection is covered by isolated hierarchy, repeated-instance, weighted-root, malformed
  triangle, and missing-reference tests plus the real `a101b.mld` count contract.
- Trigger projection reuses that tested projection path with explicit loader-supplied targets. The
  `a101b.mld` contract covers 16 complete triggers: 11 `goscript` meshes (88 vertices/132 triangles),
  20 meshes across 4 `treasure` entries (144 vertices/168 triangles), and 7 `wallmot` meshes
  (88 vertices/84 triangles), totaling 38 meshes, 320 vertices, and 384 triangles.
- The same fixture contains 11 exact `motscpt` MovingObjects, all complete, totaling 55 meshes,
  462 vertices, and 566 triangles; 21 entries remain in the generic Unknown category.

## Immediate Next Experiments

1. Calibrate the centralized coordinate policy against a known area and optional SPICE/Blender validation
   output.
2. Decide which provisional entry links and mesh-edge relationships constitute traversable adjacency,
   including transitions between GRND and ground-role GOBJ surfaces.
3. Select two positions and render an initial A* path overlay over the in-memory model.
4. After the prototype boundary is stable, design automatic area lookup, SCT integration, jobs, and the
   persisted `nav_world_blob` schema.
5. Correlate `motscpt` entries with controller/SCT behavior to determine which are doors and how their
   moving collision should affect navigation.
