# 02 - State and World Model

## Objectives

Define a planning state representation that supports:
- Overlapping walk surfaces in Y.
- Obstacle and trigger interactions.
- Cutscene interrupts and repositioning.
- Route-planning independent from camera implementation details.

## World Model Components

## 1) Walkable Surface Model

- Source: GRND polygon collections (directly from ISO via Dolphin volume/filesystem APIs).
- Representation options:
  - Polygon adjacency graph (coarse).
  - 3D navmesh with portal transitions (preferred).
- Requirements:
  - Preserve vertical layering where projections overlap in X/Z.
  - Explicitly represent legal transitions between layers.
  - Use GRND_Link data to define inter-polygon connectivity.

### Current direction
- Use a true 3D graph/navmesh search (A* in 3D state space) rather than flattening to 2D.
- Decode GRND_Link semantics and generate explicit edge metadata.

## 2) Collision Model

- Source: MLD collision objects (prisms + additional models).
- MVP usage:
  - Coarse collision boundaries for global feasibility.
  - Follow-up worker jobs probe important collision regions and refine effective bounds using observed player coordinates.

### Current direction
- Maintain a rebake step that updates collision bounds from empirical probe results.

## 3) Interaction/Trigger Model

- Source: interaction volumes + trigger objects + script coupling.
- Types:
  - interaction prompts (chest/door/etc.)
  - load/zone transitions
  - cutscene start triggers
- Each trigger stores:
  - activation predicate
  - required flags
  - resulting state transitions

## 4) Script/Controller Model

- Decode `.SCT` scripts to model trigger and cutscene behavior.
- Map MLD object controllers to runtime behavior (including moving platforms as future expansion).
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
