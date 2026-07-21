# 04 - Suppressed Exploration and World Refinement

## Status

Future plan. Static MLD geometry is necessary but insufficient for trusted routing because projected
walls may intersect ground triangles, runtime collision selection may differ from extraction assumptions,
and authored events can prevent full-area probing. This document specifies a separate patched exploration
lane that refines the world without contaminating prediction or validation evidence.

SAVOR has no general VM runtime patch/write API for this work today. Patch addresses, application timing,
restoration, and safety gates require implementation and research before these jobs can run.

## Evidence-Layer Separation

The workflow keeps four relevant state classes distinct:

| State class | Runtime modifications | Permitted use |
|---|---|---|
| Clean context state | None | Prediction start and clean validation lineage |
| Static content/world | None; parsed from disc content | Candidate geometry, graph, authored metadata |
| Patched exploration state | Named suppression profile | Collision/passability discovery only |
| Clean validation state | None; restored from clean lineage | Validate selected controls and outcomes |

A patched savestate, RAM snapshot, or successor state is never promoted to `NavigationContextResult`,
never supplied to `SavorPredict` as a clean start, and never used for clean validation.

## Named Patch Profiles

Encounter and event suppression are independent capabilities.

### `EncounterSuppressed`

Prevents random battle entry during ordinary geometry probes while preserving movement and collision
behavior as closely as research supports. The profile records every address/instruction/value changed,
activation and restoration points, runtime build compatibility, and validation evidence.

### `TriggerSuppressedSelective`

Suppresses only specifically approved script/event triggers so a probe can traverse otherwise interrupted
areas. It is research-gated because indiscriminate trigger suppression may prevent doors, platforms,
collision-resource changes, or scripts required to make geometry meaningful.

The profile therefore contains an allowlist of trigger/function identities and explicit exclusions. It is
never implied by `EncounterSuppressed`.

### Composite profiles

A job may request both profiles, but the resulting composite identity contains both full definitions and
an ordered application recipe. The shorthand "suppressed exploration" is not an artifact identity.

## Patch Provenance and Safety

Every applied profile records:

- patch profile ID, schema, and digest;
- exact game executable/disc/runtime build compatibility;
- each target address or symbolic locator and expected original bytes/value;
- written bytes/value and application frame;
- verification that the original state matched before writing;
- restoration procedure and post-restoration verification;
- worker/process identity; and
- diagnostics for partial application or cleanup failure.

If any expected original value does not match, the worker rejects the patch rather than applying a nearby
or guessed edit. Worker process isolation remains the final containment boundary.

## Exploration Inputs

`NavigationExplorationRequest` references:

- one static `NavigationWorldModel`/graph identity;
- one clean context only as source lineage and a way to derive disposable probe starts;
- exact patch profile IDs;
- probe partition and search bounds;
- surface/boundary candidates;
- allowed trigger/door/platform state signature dimensions;
- telemetry contract and versions; and
- runtime, time, and retry budgets.

The request does not mutate the clean context. Any probe savestate derived for convenience remains a
patched exploration artifact.

## Probe Generation

`SavorNavigation` generates candidates from:

- ground-triangle interiors and edges;
- projected wall intersections and near-boundary offsets;
- GRND/GOBJ overlaps and stacked surfaces;
- derived portals and uncertain links;
- trigger, MovingObject, door, and collision-resource boundaries;
- parser or topology diagnostics; and
- coverage gaps from earlier observations.

Candidate ordering may prioritize route-relevant regions, but a local route sample does not prove global
passability.

## NavigationGeometryObservation

Each immutable runtime observation records:

- world/graph and source geometry identity;
- exact patch profile identity;
- probe start state and derivation lineage;
- state signature for relevant collision/script/object modes;
- requested path/approach and executed input/camera telemetry;
- per-frame position, velocity, facing, active ground/collision resource, and contact evidence;
- trigger/event/battle/control transitions;
- observed pass, block, slide, fall, warp, interruption, or divergence outcome;
- end state and coverage contribution; and
- runtime build, worker, attempts, confidence, and diagnostics.

Camera and controller orientation belong here because they are empirical execution evidence. They are not
added to `NavigationContextResult` or made a prerequisite for planning-level route search.

## State Signatures

Geometry may vary with door state, platform state, switches, script branches, or collision-resource
selection. A refinement is therefore keyed by an explicit state signature rather than treated as one
universal map.

The first implementation may support a deliberately small signature vocabulary. Unknown state
dimensions lower confidence or produce separate `UnknownState` observations. Merging two signatures is
allowed only when an explicit equivalence rule is validated.

## Sub-Triangle Refinement

Runtime passability cannot be stored only as one boolean per `NavigationTriangleKey`. A projected wall
may cut across the middle of a walkable triangle, and only part of a triangle may be reachable under one
state signature.

`NavigationWorldRefinement` therefore supports:

- validated passable and blocked local polygons/segments within a source triangle;
- boundary contact intervals and approach direction;
- refined adjacency and portal spans;
- state-qualified transition edges;
- uncertainty/untested regions; and
- links back to every supporting or contradicting observation.

The representation may later choose a constrained subdivision, overlay mesh, or sampled field. The
semantic requirement is stable local coordinates plus source-triangle provenance; it must not silently
replace the authored triangle with an untraceable mesh.

## Coverage and Confidence

Coverage is measured separately for:

- triangle interiors;
- wall/edge approaches and directions;
- cross-resource portals;
- relevant state signatures;
- trigger/interruption boundaries; and
- route-specific corridors.

Evidence levels are:

1. `StaticOnly` - parser/projector evidence with no runtime probe.
2. `Observed` - at least one runtime observation under an exact patch/state signature.
3. `Reproduced` - compatible independent attempts agree.
4. `Contradictory` - observations disagree or conflict with static geometry.

Encounter or trigger suppression makes the evidence suitable for geometry discovery, not for claiming
natural event/encounter behavior.

## Building a Refinement

`nav.build_refinement` is deterministic over:

- static world identity;
- ordered observation artifact identities;
- patch/state-signature schemas;
- refinement algorithm and parameters; and
- coordinate-policy version.

It emits a new immutable refinement. New observations produce a descendant refinement; they do not edit
the old artifact in place. Contradictions remain visible and lower confidence instead of being resolved by
last-write-wins.

## Relationship to Movement-Anomaly Research

The same probe framework may record sticky corners, speed changes, slides, or other movement anomalies.
Those observations remain a separate analysis result and require reproducibility before becoming planning
actions. A geometry refinement may mark an area traversable or blocked without labeling it a speedup.

## Failure Semantics

- Patch precondition mismatch fails the probe before execution.
- Partial patch application quarantines the worker/process result and produces no geometry claim.
- Encounter interruption under an encounter-suppressed profile is a profile failure, not a passability
  observation.
- An unsuppressed required script trigger is an interruption outcome, not proof of blocked collision.
- Trigger suppression that invalidates required world state marks the observation unusable for that state.
- Timeout/divergence preserves telemetry but contributes no positive passability claim beyond the last
  confirmed observation.

## Acceptance Rules

- Clean, patched, and validation lineages are queryably distinct.
- Patch profiles are named, versioned, and content-addressed.
- Encounter suppression never implies trigger suppression.
- Refinement can represent a wall cutting through one source triangle.
- Every refined fact links to static geometry and runtime evidence.
- No patched state is accepted as a prediction context or clean validation start.
