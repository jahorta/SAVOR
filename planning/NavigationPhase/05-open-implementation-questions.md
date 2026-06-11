# 05 - Open Implementation Questions

## Status

Future plan. File parsing questions are SPICE-owned; this document tracks SAVOR-side integration,
navigation model, route planning, control solving, and UI decisions.

This document now captures both resolved decisions and remaining open items.

## Resolved Decisions (2026-04-09)

## A. Data & Extraction

1. **Source of truth**
   - Use SPICE as the parsing/content source for MLD/SCT and other SoA filetypes.
   - SAVOR should request or consume SPICE area-content artifacts rather than walking the ISO filesystem
     directly for navigation parsing.
   - No SAVOR-side fallback parser is required in MVP.

2. **Versioning**
   - Use Game ID as extraction/version key.
   - Only US game version is in MVP support scope for memory/breakpoint integration.

3. **Coverage direction**
   - SPICE decodes `.SCT` scripts as part of world/trigger modeling inputs.
   - SPICE exposes MLD controller behavior and target discovery data for SAVOR route planning.
   - Moving platforms are acknowledged as later-pass behavior modeling.

## B. Geometry Semantics

1. **Overlapping Y layers**
   - Use true 3D search state (do not flatten to 2D).

2. **Portal/transition inference**
   - Use SPICE-provided link/connectivity metadata for transition generation.

3. **Collision fidelity**
   - Start coarse.
   - Launch worker probes on important collisions to refine effective bounds, then rebake.

## C. Camera & Controls

- Camera is not part of route planning state.
- Pipeline is:
  1) plan route,
  2) produce spline,
  3) send to control-solver workers,
  4) solve camera+player manipulation needed to follow spline,
  5) report/select best candidate.
- Add VM instructions for spline ingestion and iterative follow loop with savestate checkpoints.

## D. Cutscene Modeling

1. **Detection direction**
   - Use MLD trigger + flag combinations for likely cutscene starts.
   - Decode scripts, identify section starts, and use runtime breakpoint heuristics to locate boundaries.

2. **Identity/versioning**
   - Use `FILENUM_FILELETTER_SECTIONNAME`-style key for unique cutscene section identity.
   - Parse section for player-position set instructions to map post-cutscene transforms.

3. **Branching**
   - Treat cutscenes as non-branching for this game scope.

## E/F. Optimization + Validation policy

- MVP focuses on baseline optimization only.
- Do not add heavy optimization stack in first pass.
- Assume savestate determinism with fixed inputs; only add repeated validation if failures are observed.

## G. Product/Workflow

- Add a service for visual world-model reconstruction.
- Add 3D viewer to inspect model/path and support start/target selection.
- Navigation phase is UI-first; CLI objective specification is out of scope.

---

## Remaining Open Questions

1. Minimum SPICE area-content contract for walking planes, links, collision hints, and target discovery.
2. Initial minimum schema for SAVOR `nav_world_blob` and `nav_script_index`.
3. Worker fanout strategy for control solving (by segment vs by candidate).
4. Minimal viable 3D viewer interaction model for selecting start/target descriptors.
5. Heuristic to prioritize which collision regions get probe/refinement jobs first.

## Immediate Next Experiments

1. Add SPICE submodule integration and define the SAVOR request/response artifact contract.
2. Consume a SPICE-generated 3D area view with walking planes and target candidates in a SAVOR viewer.
3. Prototype spline-follow VM instructions and a single-worker control solve loop.
4. Instrument one dungeon objective pair with a known cutscene and verify post-cutscene re-anchor.
