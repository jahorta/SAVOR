# 04 - Navmesh Survey and World Refinement

## Status

Future plan. The **Navmesh Survey** establishes state-qualified effective passability, movement response,
collision oddities, and trigger-activation evidence over the static SAVOR-owned world. Static MLD geometry
is necessary but insufficient because projected walls may intersect ground triangles, runtime collision
selection may differ from extraction assumptions, and authored events may both obstruct surveying and
provide required transitions such as doors.

The Navmesh Survey spans the Dolphin-backed `nav.explore_geometry` work and deterministic
`nav.build_refinement` reduction. It consumes the output savestate of a ready `NavigationContextResult` as
its clean bootstrap, creates only disposable exploration descendants, and never contaminates prediction or
clean-validation evidence.

SAVOR has no general VM runtime patch/write API for this work today. Patch addresses, application timing,
restoration, and safety gates require implementation and research before these jobs can run.

This direction supersedes earlier planning that prescribed one fixed, job-wide
`TriggerSuppressedSelective` profile. Survey jobs need research-gated trigger control that may suppress or
permit activations dynamically while the job runs, especially while establishing anchors through doors.
The concrete trigger-control mechanism and modes are deliberately unresolved.

## Evidence-Layer Separation

The workflow keeps four relevant state classes distinct:

| State class | Runtime modifications | Permitted use |
|---|---|---|
| Clean context state | None | Navmesh Survey bootstrap, prediction start, and clean validation lineage |
| Static content/world | None; parsed from disc content | Candidate geometry, graph, authored metadata |
| Patched survey state | Named runtime modifications and recorded trigger-control history | Navmesh Survey evidence only |
| Clean validation state | None; restored from clean lineage | Validate selected controls and outcomes |

A patched savestate, RAM snapshot, or successor state is never promoted to `NavigationContextResult`,
never supplied to `SavorPredict` as a clean start, and never used for clean validation.

## Runtime Modification and Trigger-Control Direction

Encounter suppression and trigger control are independent capabilities.

### `EncounterSuppressed`

Prevents random battle entry during ordinary geometry probes while preserving movement and collision
behavior as closely as research supports. The profile records every address/instruction/value changed,
activation and restoration points, runtime build compatibility, and validation evidence.

### Dynamic trigger control (research-gated)

Initial collision probes need a way to prevent unknown trigger volumes from interrupting ordinary
measurement. Anchor expansion may simultaneously require the same job to activate a door or another
scripted transition, observe its forced movement or collision-resource change, establish a stable successor
anchor, and then resume isolated probing.

The workflow therefore requires auditable dynamic trigger activation/deactivation control within a job:
the job must be able to change whether a detected trigger effect may execute. It does **not** yet prescribe
a global disable, allowlist format, dispatcher hook, fixed set of gate modes, or causal-script boundary.
Research must determine how to preserve required field initialization, doors, platforms, MovingObjects,
and collision-resource changes while preventing unrelated activations.

Changing trigger permission does not undo an in-game state change. Branching back to a pre-trigger state
requires restoration of an earlier disposable checkpoint. Trigger control is never implied by
`EncounterSuppressed`.

### Combined runtime modifications

A job may combine encounter suppression with a versioned trigger-control policy. Its evidence records the
full runtime-modification identity plus every trigger-control change, activation window, observed trigger,
and restoration boundary. The shorthand "suppressed exploration" is not an artifact identity.

## Patch Provenance and Safety

Every runtime modification or trigger-control contract records:

- modification/control ID, schema, and digest;
- exact game executable/disc/runtime build compatibility;
- each target address or symbolic locator and expected original bytes/value;
- written bytes/value and application frame;
- each dynamic trigger-control change, reason, target evidence, and effective frame range;
- verification that the original state matched before writing;
- restoration procedure and post-restoration verification;
- worker/process identity; and
- diagnostics for partial application or cleanup failure.

If any expected original value does not match, the worker rejects the patch rather than applying a nearby
or guessed edit. Worker process isolation remains the final containment boundary.

## Exploration Inputs

`NavigationExplorationRequest` references:

- one static `NavigationWorldModel`/graph identity;
- one ready `NavigationContextResult` and its output savestate as clean bootstrap lineage;
- exact runtime-modification and trigger-control policy identities;
- the survey wave, candidate partition, and any verified survey-anchor identities;
- probe partition and search bounds;
- surface/boundary candidates;
- allowed trigger/door/platform state signature dimensions;
- telemetry contract and versions; and
- runtime, time, and retry budgets.

The request does not mutate the clean context. Any probe savestate derived for convenience remains a
patched exploration artifact.

## Survey Waves and Anchors

The waves express dependencies and scheduling priority, not mandatory whole-field barriers. A verified
result may release its dependent jobs immediately.

1. **Bootstrap** consumes the output savestate of `nav.capture_context`. It does not repeat or weaken the
   Navigation Context readiness contract.
2. **Survey-anchor expansion** moves outward from the bootstrap or an already verified anchor and creates
   shorter-lived starting states near candidate clusters. A validated reposition procedure may place the
   actor closer to a target only when all required placement, ground/resource, velocity, collision-state,
   and settle checks succeed.
3. **Ordinary collision survey** probes passability, walls, external edges, corners, ramps, stairs,
   GRND/GOBJ handoffs, and uncertain portals in parallel.
4. **Collision-oddity survey** follows up on telemetry candidates such as sticky-corner positional jumps,
   wall-contact ramp-speed behavior, slides, snags, and unexpected displacement.
5. **Trigger survey** measures automatic-trigger boundaries and interactable-trigger activation envelopes
   after trigger identity and control behavior are understood well enough to run safely.

A survey anchor records its parent lineage, world/state signature, position and facing, active
ground/resource, settle evidence, runtime modifications, and disposable savestate. Local placement
validity and proven reachability from the clean field-entry context are separate facts. A worker may
establish a post-door anchor inside one job, but downstream fan-out uses the checkpointed anchor rather
than depending on one long-lived worker process.

## Probe Generation

`SavorNavigation` generates candidates from:

- ground-triangle interiors and edges;
- projected wall intersections and near-boundary offsets;
- GRND/GOBJ overlaps and stacked surfaces;
- derived portals and uncertain links;
- trigger, MovingObject, door, and collision-resource boundaries;
- ordinary slopes and wall-adjacent ramp/stair approaches;
- parser or topology diagnostics; and
- coverage gaps from earlier observations.

Candidate ordering may prioritize route-relevant regions, but a local route sample does not prove global
passability. Ordinary probes capture movement-response and prospective trigger-hit telemetry so later
oddity and trigger waves can target evidence rather than rescan the field blindly.

## NavigationGeometryObservation

Each immutable runtime observation records:

- world/graph and source geometry identity;
- exact runtime-modification and trigger-control-history identity;
- probe start state and derivation lineage;
- survey-anchor identity, positioning method, and settle verification;
- state signature for relevant collision/script/object modes;
- ordered trigger-control changes and trigger activations observed during the probe;
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
- trigger/interruption boundaries;
- collision-oddity approach, contact, timing, and release variants; and
- route-specific corridors.

Evidence levels are:

1. `StaticOnly` - parser/projector evidence with no runtime probe.
2. `Observed` - at least one runtime observation under exact runtime-modification,
   trigger-control-history, and state-signature identities.
3. `Reproduced` - compatible independent attempts agree.
4. `Contradictory` - observations disagree or conflict with static geometry.

When used, encounter suppression or any trigger-control modification makes the evidence suitable for
survey discovery, not for claiming natural event/encounter behavior.

## Building a Refinement

`nav.build_refinement` is deterministic over:

- static world identity;
- ordered observation artifact identities;
- runtime-modification, trigger-control-history, and state-signature schemas;
- refinement algorithm and parameters; and
- coordinate-policy version.

It emits a new immutable refinement. New observations produce a descendant refinement; they do not edit
the old artifact in place. Contradictions remain visible and lower confidence instead of being resolved by
last-write-wins.

## Movement Response and Collision Oddities

The Navmesh Survey records the directional movement response needed by later optimization without
performing that optimization. Normal probes establish baseline displacement, velocity, vertical gain,
surface/resource changes, and collision response. Targeted oddity probes vary approach angle and speed,
contact point and duration, input direction, and release timing.

The first named oddity families include:

- sticky corners that hold or redirect motion and may later produce a discontinuous player-position jump;
- ramp-speed behavior where wall contact while ascending a ramp or staircase produces a different ascent
  rate from ordinary travel;
- slides, snags, step-up behavior, speed loss/retention, and other unexpected displacement.

Each observation retains the exact entry and release conditions, frame-by-frame position/velocity,
contact and active-resource evidence, net progress, VI-frame cost, repetition, and variance. Oddity
evidence remains separate from ordinary passability. Only a reproducible measured behavior may later be
offered to a planner as an available movement primitive; the survey does not decide whether to use it.

## Trigger Survey Direction

Trigger research must precede a concrete gate contract, but the survey output requirement is established:

- Automatic script triggers are tested from every available/reachable approach side to refine their
  positional crossing boundaries and state conditions.
- Interactable triggers are tested for the position, distance, facing, input, and any occlusion/state
  envelope that still activates them; distance alone is not assumed sufficient.
- A door or trigger that changes collision, forces movement, or transfers the actor creates a
  state-qualified transition between pre- and post-activation survey anchors.
- If research establishes a suppression point that still exposes trigger detection, hits recorded before
  their effects are suppressed seed later targeted work. They are not blocked collision evidence.

If an activation changes door, platform, MovingObject, switch, or collision-resource state, the affected
geometry belongs to a new state signature and may require a localized resurvey.

## Failure Semantics

- Patch precondition mismatch fails the probe before execution.
- Partial patch application quarantines the worker/process result and produces no geometry claim.
- Encounter interruption under an encounter-suppressed profile is a profile failure, not a passability
  observation.
- A suppressed or unexpected trigger hit is a trigger observation or interruption, not proof of blocked
  collision.
- Trigger control that invalidates required initialization or world state marks the observation unusable
  for that state.
- A repositioned anchor that does not settle on the expected ground/resource produces no anchor or
  passability claim.
- An allowed door/trigger transition that does not reach a stable, attributable successor state produces
  no post-transition anchor.
- Timeout/divergence preserves telemetry but contributes no positive passability claim beyond the last
  confirmed observation.

## Acceptance Rules

- Clean, patched, and validation lineages are queryably distinct.
- The Navmesh Survey bootstraps from the exact ready context output savestate without mutating it.
- Runtime modifications and trigger-control histories are named, versioned, and content-addressed.
- Encounter suppression never implies a trigger-control decision.
- No fixed trigger-gate modes are treated as resolved before the trigger research contract is accepted.
- Survey-anchor local validity and entry reachability remain distinct.
- Refinement can represent a wall cutting through one source triangle.
- Collision-oddity output can represent sticky-corner positional jumps and wall-contact ramp-speed
  conditions without making an optimization decision.
- Trigger output can represent automatic crossing boundaries, interactable activation envelopes, and
  stateful pre/post-anchor transitions.
- Every refined fact links to static geometry and runtime evidence.
- No patched state is accepted as a prediction context or clean validation start.
