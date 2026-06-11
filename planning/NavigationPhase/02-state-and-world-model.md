# 02 - State and World Model

## Status

Future plan. Low-level SoA file parsing is delegated to the SPICE submodule. This document describes the
SAVOR planning model built from SPICE-provided area content.

## Objectives

Define a planning state representation that supports:
- Overlapping walk surfaces in Y.
- Obstacle and trigger interactions.
- Cutscene interrupts and repositioning.
- Route-planning independent from camera implementation details.

## World Model Components

## 1) Walkable Surface Model

- Source: SPICE area content output derived from the current area's MLD/SCT and related package data.
- Representation options:
  - Polygon adjacency graph (coarse).
  - 3D navmesh with portal transitions (preferred).
- Requirements:
  - Preserve vertical layering where projections overlap in X/Z.
  - Explicitly represent legal transitions between layers.
  - Preserve SPICE-provided link/connectivity metadata for inter-polygon movement.

### Current direction
- Use a true 3D graph/navmesh search (A* in 3D state space) rather than flattening to 2D.
- Consume SPICE walking-plane/link metadata and generate SAVOR navigation edge metadata from it.

## 2) Collision Model

- Source: SPICE collision/walk-plane output for the active area.
- MVP usage:
  - Coarse collision boundaries for global feasibility.
  - Follow-up worker jobs probe important collision regions and refine effective bounds using observed player coordinates.

### Current direction
- Maintain a rebake step that updates collision bounds from empirical probe results.

## 3) Interaction/Trigger Model

- Source: SPICE target-discovery output from script triggers, object content, treasure chests, doors,
  load zones, and related area metadata.
- Types:
  - interaction prompts (chest/door/etc.)
  - load/zone transitions
  - cutscene start triggers
- Each trigger stores:
  - activation predicate
  - required flags
  - resulting state transitions

## 4) Script/Controller Model

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

- `spice_area_view`:
  - walking planes, collision hints, renderable area geometry, potential targets, and parse diagnostics
- `nav_world_blob`:
  - surfaces, links, obstacles, triggers, controller metadata
- `nav_script_index`:
  - decoded script section map and transition hints
- `nav_transition_catalog`:
  - cutscene/interaction transition outcomes
- `nav_route_candidate`:
  - graph path + spline + metadata

## Validation Strategy

- 3D viewer overlay of reconstructed world model.
- Overlay selected route spline and objectives.
- Replay sampled route segments and compare predicted vs observed transitions.
- Flag mismatches for model correction/rebake.
