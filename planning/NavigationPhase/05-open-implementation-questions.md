# 05 - Open Implementation Questions

This document now captures both resolved decisions and remaining open items.

## Resolved Decisions (2026-04-09)

## A. Data & Extraction

1. **Source of truth**
   - Use ISO path from UI settings and read data directly through Dolphin APIs with this chain:
     1. `UICommon::GameFile(path_to.iso)` to open game metadata.
     2. `DiscIO::CreateVolume(game.GetFilePath())` to create `DiscIO::Volume` (`m_volume`).
     3. `m_volume->GetPartitions()` to enumerate partitions.
     4. For each partition, `m_volume->GetFileSystem(partition)` to get a filesystem handle (`m_file_system`).
     5. `m_file_system->GetRoot()` to get root `DiscIO::FileInfo&`.
     6. Walk directories recursively from root and locate required files (MLD/SCT and related assets).
   - No fallback source required in MVP.

2. **Versioning**
   - Use Game ID as extraction/version key.
   - Only US game version is in MVP support scope for memory/breakpoint integration.

3. **Coverage direction**
   - Decode `.SCT` scripts as part of world/trigger modeling.
   - Include MLD controller behavior in model roadmap.
   - Moving platforms are acknowledged as later-pass behavior modeling.

## B. Geometry Semantics

1. **Overlapping Y layers**
   - Use true 3D search state (do not flatten to 2D).

2. **Portal/transition inference**
   - Use GRND_Link system for connectivity and transition generation.

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

1. Exact GRND_Link decode details and edge semantics (all link types/flags).
2. Initial minimum schema for `nav_world_blob` and `nav_script_index`.
3. Worker fanout strategy for control solving (by segment vs by candidate).
4. Minimal viable 3D viewer interaction model for selecting start/target descriptors.
5. Heuristic to prioritize which collision regions get probe/refinement jobs first.

## Immediate Next Experiments

1. Build ISO filesystem walker for target files (MLD/SCT) on US disc.
2. Implement first-pass GRND + GRND_Link decode and inspect graph connectivity in viewer.
3. Prototype spline-follow VM instructions and a single-worker control solve loop.
4. Instrument one dungeon objective pair with a known cutscene and verify post-cutscene re-anchor.
