# 04 - Phase and Job Integration

## Goal

Define how Navigation Phase fits into existing SAVOR phase/job infrastructure and UI workflow.

## Runtime Data Source Decision

- No fallback source required for MVP.
- Use ISO path configured in UI settings.
- Read game content through Dolphin stack:
  - `UICommon::GameFile(path)`
  - `DiscIO::CreateVolume(game.GetFilePath())`
  - partition discovery and filesystem walk
- Use Game ID (`GetGameID`) as extraction/version key.

## Versioning Decision

- GameCube Skies of Arcadia has 3 versions; only US is currently supported by addresses/breakpoints.
- Asset/version keys should include Game ID, and MVP scope targets US build.

## Proposed Program Kind

Draft identifiers:
- `program_kind`: `navigation_phase`
- Step kinds (initial draft):
  - `nav.build_world`
  - `nav.plan_global`
  - `nav.solve_controls`
  - `nav.publish_result`

## Step Responsibilities

## 1) `nav.build_world`
Input:
- ISO path
- map/dungeon ID

Output artifacts:
- `nav_world_blob`
- `nav_script_index`
- extraction diagnostics

## 2) `nav.plan_global`
Input:
- start descriptor
- target descriptor
- world blob
- planning config

Output artifacts:
- top-K route skeletons
- route splines

## 3) `nav.solve_controls`
Input:
- route spline(s)
- solver config

Output artifacts:
- executable input tape candidate(s)
- solver telemetry (deviation/frame counts/trigger hit info)

Notes:
- naturally parallelizable by route candidate and/or segment.

## 4) `nav.publish_result`
Input:
- selected successful candidate

Output:
- canonical route artifact
- summary metrics
- optional replay package

## Draft Job Payload Fields

- `seed_snapshot_path`
- `iso_path`
- `game_id`
- `map_id`
- `start_descriptor`
- `target_descriptor`
- `planner_config_json`
- `solver_config_json`

## Draft Job Result Fields

- `status_code`
- `objective_reached` (bool)
- `total_vi_frames`
- `route_artifact_id`
- `input_tape_artifact_id`
- `diagnostics_artifact_id`

## Failure Classes (Draft)

- ISO missing/unreadable
- Unsupported game ID
- World extraction/decode failure (MLD/SCT)
- Start/target resolution failed
- No feasible route in graph
- Control solver failed to realize spline

## Artifact Inventory (Draft)

- `nav_world_blob.bin`
- `nav_script_index.bin`
- `nav_route_candidates.jsonl`
- `nav_splines.bin`
- `nav_best_input_tape.bin`
- `nav_run_report.json`

## UI/Visualization Integration

- Add service to produce visual world-model reconstruction.
- Add 3D viewer pane for:
  - world geometry/triggers visualization
  - start/target specification
  - selected path/spline overlay
- Phase is UI-first; no CLI objective-entry path planned for MVP.

## Open Integration Questions

1. Should control-solver workers fan out by route candidate, by segment, or hybrid?
2. Minimum viewer capabilities needed for first internal usability pass?
3. How should route artifacts evolve as extraction decoders improve?
