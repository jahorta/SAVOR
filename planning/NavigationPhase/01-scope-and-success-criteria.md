# 01 - Scope and Success Criteria

## Status

Future plan. SPICE owns MLD/SCT and other Skies of Arcadia filetype parsing. `SavorNavigation` links the
vendored parser, converts its output into SAVOR-owned navigation models, and owns route-planning
semantics. `SavorQt3D` is the first standalone host for the reusable Navigation widget.

## Problem Statement

Given:
- An AKLZ-compressed MLD selected manually from a GameCube Legends disc dump.
- A start condition and target objective.

We need to produce an input strategy that reaches the objective in the fewest VI frames, with acceptable determinism/reliability.

For the first prototype, SpiceMLD supplies walkable geometry, collision geometry, link metadata, and
available trigger information from the selected MLD. Later milestones add automatic area identity,
related SCT content, workflow jobs, and durable artifacts.

## First Interactive Prototype

1. **Manual file acquisition**
   - Select one `.mld` with a file picker and remember the last directory.
   - Use the local US disc dump as a developer fixture, not a hardcoded runtime default.
   - Do not require Dolphin, ISO traversal, or automatic area-name resolution.

2. **Direct in-process parsing**
   - `SavorNavigation` reads the compressed bytes and calls SpiceMLD.
   - SpiceMLD owns AKLZ detection/decompression and MLD parsing; SAVOR does not duplicate either.
   - Runtime parsing starts from canonical `MldFile`. A compatibility projection supplies `world`,
     `searchWorld`, and a transient in-memory Blender IR scene used only inside `SavorNavigation` to
     flatten NJ object geometry for exact `fxn=wall` collision regions, every SPICE-classified trigger,
     and exact normalized `motscpt` entries; export remains disabled.
   - A GOBJ contributes navigation geometry only when an MLD entry references its block through
     `groundAddresses`. Object-role-only GOBJ blocks are counted for diagnostics and excluded from the
     navigation surface set.

3. **SAVOR-owned model**
   - Convert parser results into an in-memory `NavigationAreaModel` before applying data to Qt.
   - Do not expose SpiceMLD types to `SavorQt3D`, the widget, the planner, or future persistence code.

4. **Standalone widget host**
   - Revive `SavorQt3D` as the development/test application.
   - Retain its existing file picker, Quick 3D renderer, layer visibility controls, and diagnostics where
     useful; the obsolete `SavorMLD` dependency path has been removed.
   - Load and convert the selected file off the UI thread, then apply the completed model on the UI thread.

## In-Scope (MVP)

1. **Static world navigation**
   - Consume walking-plane and target-discovery data exposed through `NavigationAreaModel`.
   - Include both native GRND meshes and GOBJ meshes used in the ground role, while preserving their
     source kind and entry/block/node identity.
   - Represent non-walkable obstacles and trigger volumes in SAVOR navigation types, including their
     attached projected object meshes when available.
   - Preserve exact `motscpt` entries as provisional `MovingObject` regions for inspection without yet
     asserting that every entry is a door or modeling its runtime motion/controller behavior.

2. **Objective-based routing**
   - Route between named objectives:
     - chest/interaction
     - doorway/zone transition
     - map exit/load trigger

3. **Cutscene-aware progression**
   - Detect and model cutscene-triggered interruptions.
   - Continue planning from post-cutscene state.

4. **Time-optimal baseline**
   - Optimize for completion time in VI frames, but keep optimization strategy simple in MVP.

5. **Simulator-backed route execution**
   - Produce a route/spline and solve for executable inputs in workers.

6. **UI-first workflow**
   - Navigation phase is UI-driven (no CLI workflow for objective specification in MVP).

## Non-Goals (MVP)

- Full global route planning across multiple maps with long-term resource constraints.
- Dolphin/ISO-backed MLD discovery or automatic area-ID lookup in the first prototype.
- A SAVOR-side MLD parser or AKLZ decompressor.
- Durable `nav_world_blob` persistence or a required serialized SPICE area-view artifact in the first
  prototype.
- Blender IR as a runtime data contract, persisted artifact, or Qt-facing type. A transient internal
  projection is permitted solely to convert NJ object geometry into SAVOR-owned meshes.
- Combat strategy co-optimization.
- Heavy optimization/meta-optimization for search budgets in first pass.
- Mandatory repeat-run validation gates in first pass (add if deterministic assumptions fail in practice).

## Success Criteria (MVP)

1. **Functional completion**
   - Can produce a successful route between at least two objective pairs in one dungeon.

2. **Cutscene continuity**
   - If a mandatory cutscene triggers, planner resumes from post-cutscene location and still reaches objective.

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

## Deliverables

- Pinned SPICE submodule integration and a `SavorNavigation` adapter contract for navigation-relevant
  world data.
- Reusable Navigation widget running in the standalone `SavorQt3D` host.
- Planner/refiner artifact format for routes and candidate telemetry.
- Phase integration contract (job payload/result schema and step kinds).
- 3D world-model viewer and path overlay support in UI.
- Benchmark set + reporting template.

## Acceptance Benchmarks (Draft)

- Benchmark A: simple straight traversal with 0 cutscenes.
- Benchmark B: traversal requiring 1 mandatory interaction cutscene.
- Benchmark C: traversal with overlapping walk mesh elevation and tight collision corners.

## Risks

- Geometry mismatch between extracted data and runtime collision behavior.
- Incorrect coordinate conversion, matrix interpretation, or triangle winding. The first prototype must
  calibrate a centralized `SavorNavigation` coordinate policy against known areas instead of distributing
  renderer-specific fixes.
- Attached trigger meshes show the MLD object geometry, but trigger/script coupling, player interaction
  radius, and SCT/controller activation predicates are not yet represented.
- Moving platform/controller rules requiring a second modeling pass.
- `motscpt` may include doors, but entry meaning and motion/activation behavior require controller/SCT
  evidence before becoming navigation semantics.
