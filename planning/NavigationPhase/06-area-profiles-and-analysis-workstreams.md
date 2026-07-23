# 06 - Area Profiles and Analysis Workstreams

## Status

Future plan. This document defines the content-derived profiles and analysis workstreams that sit on top
of the implemented `SavorNavigation` loading, visualization, start-selection, graph, and A* prototype.
The current implementation target is Dungeon Navigation. Safe Navigation is a later sibling that reuses
the same surface-constrained 2.5D foundation without random-encounter behavior. Area 99 remains a separate
Overworld Navigation problem.

This file remains normative for profile precedence, shared/profile-specific capabilities, encounter
geography, and evidence levels. The reset-qualified runtime, disc/content, exploration, prediction,
control, validation, workflow, and artifact contracts are normative in
[`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md).

## Area Profile Classification

`NavigationAreaProfile` is a SAVOR-owned classification. It is derived from the selected MLD's normalized
area key and its complete companion-file set, in this order:

1. **Overworld**
   - Any area key beginning with `099` is Overworld, regardless of which companion files exist.
   - Every `me099*.sct` belongs to the Overworld profile.
   - This rule takes precedence over ECT presence. For example, `a099a.mld`, `me099a.sct`, and
     `a099a.ect` still form an Overworld scenario.
2. **Dungeon**
   - A non-099 MLD with a case-insensitively matched, same-key ECT is Dungeon.
   - ECT presence defines the profile; successful ECT parsing does not.
   - A missing or failed SCT makes the scenario script-incomplete without reclassifying it.
3. **Safe**
   - A non-099 MLD with a matched SCT and no matched ECT is a Safe Navigation candidate.
   - Keys below `200a` are known traversable. Keys at or above `200a` retain candidate status until their
     traversability is validated.
   - Safe means that random encounters are absent. Scripted triggers, events, cutscenes, doors, and other
     interruptions may still exist.
4. **Unknown / View-only**
   - Nonconforming names and MLDs with neither an associated SCT nor ECT remain available for inspection,
     but receive no navigation-phase guarantee.

For a conforming key `NNNC`, the expected names are `aNNNC.mld`, `meNNNC.sct`, and `aNNNC.ect`.
Comparison is case-insensitive and preserves the original paths for provenance. Automatic companion
discovery is limited to the MLD directory. A manually supplied matching SCT may restore script association;
there is no manual ECT override in this milestone.

Classification, parser status, and traversability evidence are independent:

- An unreadable, undecompressible, malformed, or partial ECT leaves a non-099 area classified as Dungeon
  and marks encounter content incomplete.
- A missing ECT classifies an SCT-associated non-099 area as Safe only when the selected directory is
  treated as the complete companion set.
- Successful mesh extraction establishes geometry completeness, not known traversability or runtime
  collision accuracy.

## Shared and Profile-Specific Capabilities

| Capability | Shared 2.5D foundation | Dungeon only | Safe only |
|---|---:|---:|---:|
| MLD/SCT loading and area identity | Yes |  |  |
| Ground-role GRND/GOBJ geometry | Yes |  |  |
| Walls, triggers, and MovingObjects | Yes |  |  |
| Opcode-77 starts and manual endpoints | Yes |  |  |
| Triangle graph, A*, spline, and control solver | Yes |  |  |
| Navmesh Survey anchors and refinement | Yes |  |  |
| Collision validation | Yes |  |  |
| Movement-response and collision-oddity discovery | Yes |  |  |
| Dynamic trigger control and activation survey | Planned |  |  |
| Event/cutscene interruption handling | Yes |  |  |
| SpiceEct and encounter-table regions |  | Yes |  |
| Encounter suppression during ordinary probes |  | Yes | Unnecessary |
| Route-relative encounter exposure |  | Yes |  |
| `SavorPredict` encounter-outcome search and reachability frontiers |  | Planned | Not applicable |
| Explicit no-encounter profile |  |  | Yes |
| Free-altitude movement and Overworld RNG | Deferred | Deferred | Deferred |

Dungeon and Safe profiles use the same three-dimensional surface geometry while constraining player motion
to walkable ground. This is distinct from Overworld movement, where altitude is an independent control
dimension and encounter selection has Area-99-specific spatial and RNG semantics.

## Shared Scenario Components

The implemented `NavigationScenarioModel`, `NavigationAreaModel`, `NavigationScriptModel`,
`NavigationTraversalGraph`, endpoint resolvers, and deterministic A* remain the shared foundation. Planned
SAVOR-owned additions are optional analysis data associated with the scenario:

- profile and companion-set provenance;
- Navmesh Survey anchors with separate local-validity and clean-entry-reachability evidence;
- collision-validation observations keyed to source geometry and runtime probe position;
- movement-response/oddity candidates with approach, input/camera context, sticky-jump or ramp-speed
  measurements, speed/displacement delta, and reproducibility;
- automatic/interactable trigger activation evidence and state-qualified pre/post door transitions;
- for Dungeon only, encounter source/table profiles, per-triangle selectors, explicit encounter start
  state, selected-route exposure, and diagnostics.
- for a later selected predictor result, a separate Qt-neutral compatibility/projection attachment that
  references immutable prediction artifacts without changing static scenario state.
- for workflow execution, explicit immutable context, content, patched-observation/refinement, prediction,
  control-solve, and clean-validation layers rather than one mutable scenario record.

Encounter geography is keyed by the existing `NavigationTriangleKey`. It is not a
`NavigationRegion`, a trigger volume, or a `NavigationSurface::tblId`. Ground-entry `tblId` identifies
collision resources; it is not an encounter-table ID.

## Shared Ground Handoff Contract

Dungeon and Safe Navigation use the same data- and disassembly-backed ground-selection contract. An MLD
entry's `ground_links` are ordered target EntryIDs, not unordered adjacency hints:

1. Preserve every raw linked EntryID in authored order, including EntryID `0` and duplicates. Resolve a
   target through indexed lookup followed by the first linear EntryID match. A missing EntryID truncates
   the effective runtime chain; retain later values as suppressed provenance. A resolved target lacking
   usable geometry is diagnosed without erasing later authored evidence.
2. Treat all GRND and ground-role GOBJ surfaces owned by one entry as its collision bundle. Resource kind
   selects a collision backend in the game; it does not create a special navigation rule or make a GOBJ
   inherently moving.
3. For a candidate source-boundary interval, query the rest of the current entry bundle first. Continued
   coverage creates a same-entry handoff and suppresses linked-target evaluation.
4. Only when the whole current bundle misses, query linked bundles in authored order. The first accepting
   bundle owns the directed interval and shadows later targets. A first hit whose height is discontinuous
   remains unresolved rather than falling through to a later target.
5. Determine coverage against target triangle footprints, so the transition may land inside a larger mesh;
   coincident external boundaries are not required. Select the height-nearest result, resolve coincident
   duplicates with stable triangle keys, and leave effectively tied stacked heights ambiguous.
6. Reverse traversal is not synthesized. It exists only when independently derived from the reverse
   source's current-bundle coverage and ordered fallback evidence.

Exact normalized `ground` entries without nonzero motion resources are statically traversable. A
motion-bearing or non-`ground` entry is `RequiresRuntimeState`: retain its transformed bind-pose geometry
and muted-orange conditional handoff evidence for inspection, but omit it from active A*. Static handoffs
render yellow. Both retain source/target EntryIDs, triangle keys, same-entry versus authored-fallback kind,
authored ordinal when present, and availability.

Initial converted-scene tolerances are a 0.001 vertex weld, 0.001 planar containment tolerance, 0.01
outward probe capped at one quarter of its interval, 0.01 height-continuity tolerance, 0.001 distinct-height
tie tolerance, 0.001 minimum portal length, and 0.0001 minimum absolute up-normal component for height
solving. They are centralized graph-build configuration and remain subject to runtime calibration.

This contract supplies static candidate topology, not complete collision proof. Projected wall blocking,
decoded triangle-flag filtering, player step-up behavior, animation/script-driven surface state, and exact
runtime collision-selector modes remain deferred. Collision-validation work must refine confidence without
silently rewriting source geometry or authored fallback provenance.

The planned **Navmesh Survey** is the shared runtime workstream that supplies verified survey anchors,
passability refinement, directional movement response, collision oddities, and trigger activation evidence.
The workstreams below remain separate evidence products even though one dependency-driven worker survey
schedules them together.

## Workstream 1 - Collision Validation

Collision validation compares extracted GRND/GOBJ and projected wall geometry with runtime behavior. It
must remain separate from parser completeness:

- `hasCompleteGroundGeometry` and `hasCompleteWallGeometry` say that expected source geometry was
  converted successfully.
- Validation coverage says which surfaces, boundaries, and probe approaches have runtime evidence.
- Validation observations retain expected contact, observed contact/response, position, ground/resource
  identity, input/camera context, and source provenance.
- Discrepancies remain visible and lower confidence for affected route segments; they do not rewrite the
  source geometry silently.

`SavorNavigation` generates probe candidates and interprets results. Dolphin-backed execution reads the
active ground/resource and player/collision state through `SavorCore` and `SavorWorker`. Probe fan-out may
partition by boundary or region, start pose, approach direction, and input/camera context.

The survey bootstraps from the exact output savestate of a ready `NavigationContextResult`. It expands
disposable anchors outward before releasing nearby work, including intentional door/script transitions
when required. Local anchor placement validity and reachability from the clean entry remain separate.

## Workstream 2 - Movement-Anomaly Discovery

Movement-anomaly discovery searches collision corners, seams, slopes, and boundary interactions for
measurable changes in player motion. The plan does not assume that every sticky or unusual interaction is
a useful speedup.

Each candidate records:

- world and source-geometry identity;
- approach pose, facing, camera, and input sequence;
- baseline and observed displacement or speed;
- sticky held interval and any later positional discontinuity, or ordinary versus wall-contact ramp/stair
  ascent response when applicable;
- VI-frame cost and resulting route position;
- repeat count, reproducibility, and variance;
- whether the outcome is beneficial, neutral, harmful, or unresolved.

Candidates are generated from validated or explicitly low-confidence collision regions. Simulator jobs
fan out by site, approach, input/timing, and camera variant. Only measured, reproducible positive results
may later become route-planning actions or edge-cost adjustments.

## Workstream 3 - Trigger Activation Survey

Trigger research must establish identities, safe control points, and completion boundaries before this
workstream runs. Its required outputs are nevertheless defined:

- automatic-trigger positional boundaries from every available/reachable approach side;
- interactable-trigger position, distance, facing, input, occlusion, and state activation envelopes; and
- state-qualified pre/post survey anchors for doors, forced movement, collision-resource changes, and
  other trigger-driven transitions.

Ordinary collision jobs should prevent unrelated trigger effects once a safe mechanism is proven, while
anchor expansion may need to permit a required door activation and then resume isolation in the same job.
The exact modes, allowlist or identity scheme, runtime hook, and causal-session boundary remain unresolved.
Trigger evidence never substitutes for blocked-collision evidence.

## Workstream 4 - Dungeon Encounter Analysis

### Static encounter field

The first-pass authored-selector hypothesis for each Dungeon GRND or ground-role GOBJ triangle is:

```text
rawSelector = triangleMetadata.rawU16[2] & 0x7fff
encounterTableId = (rawSelector / 10) % 10
```

The formula is the current static-analysis hypothesis, not yet a universal field rule. Preserve evidence
and confidence for the selector-field interpretation separately from evidence about which collision
resource and winning triangle the runtime selects. Both require validation across additional dungeons;
ECT parse success proves neither.

- The mask removes the strip-winding bit before semantic decoding.
- Selector `0` means no encounter-distance accumulation.
- Currently validated selectors `1..7` resolve to the corresponding one-based flat ECT table, represented
  in zero-based storage as `tables[selector - 1]`.
- Selectors `8` and `9` are preserved as unsupported, never coerced to no-encounter.
- Missing triangle metadata is Unknown, not selector zero.
- A resolved table with zero stage or zero overall rate is valid data, not a parser failure.
- Both GRND and ground-role GOBJ participate. Overlapping resources are not unioned; exact runtime
  interpretation depends on the active collision resource and winning triangle.

SpiceEct privately supplies AKLZ handling and flat ECT parsing. `SavorNavigation` converts its tables,
rates, encounter rows, source identity, and diagnostics into SAVOR-owned types. No SpiceEct type crosses
into Qt, workflow payloads, or persistence contracts.

### Static and marginal selected-route exposure

The whole scene may display a categorical selector/table overlay. Start-relative exposure is calculated
only along the currently selected route; a destination has no unique cumulative risk without a route or
route policy.

Marginal or seeded analysis requires an explicitly selected ready `NavigationContextResult` from a
reset-qualified field entry. It records measured `stepCount`, measured zero velocity, stable placement and
control, and independently captured encounter fields rather than fabricating a generic fresh-entry state.
A manual or opcode-77 spatial start never silently implies encounter state. The exposure result references
the context-result ID and preserves provenance for each supplied field, eligible-check cadence, and model
bundle.

Evidence levels are explicit:

1. **Structural** - selector, resolved table, stage/rate/rows, active versus inactive route distance, and
   table sequence. No probability claim is made.
2. **Assumed / marginal** - the recovered per-check formula is applied only when an explicit ready context
   and eligible-check sequence are supplied. Cumulative risk is labeled marginal, not seeded truth.
3. **Predicted / seeded** - a complete evidence-qualified `SavorPredict` model reproduces the relevant
   field-update/RNG schedule from an exact start state and supplies deterministic modeled outcomes.
4. **Observed / validated** - Dolphin replay supplies runtime encounter position, formation, and RNG
   telemetry. Agreement or divergence with a prediction is retained explicitly.

If check cadence is unavailable, the UI reports structural distance and table segments rather than
converting geometry distance into encounter checks. Encounter occurrence and formation selection remain
separate calculations. Encounter exposure is analysis output in this phase and does not modify the current
geometric/slope A* cost.

### Predictor-result reachability and frontier

The later predictor-backed slice uses a separate planning module in `SavorQt`. That module authors a
no-encounter or specific-encounter objective and search bounds, selects a compatible navigation world and
explicitly references a ready `NavigationContextResult`, invokes the existing `SavorPredict` subsystem through a future asynchronous boundary,
compares candidates, and selects one result for display. `SavorPredict` is currently an exploratory
application/CLI; this document does not assume that a reusable navigation API or direct Qt linkage already
exists.

The reusable Navigation widget and `SavorQt3D` do not simulate, enumerate, or rank paths or
movement/no-movement schedules. They consume only the selected immutable prediction result after
`SavorNavigation` validates its spatial compatibility and converts its stable geometry references into a
Qt-neutral overlay. The static selector/table field remains independently visible.

Each result is qualified by:

- objective and requested encounter identity when applicable;
- game/area/profile and MLD/SCT/ECT content identity;
- navigation-world, geometry/graph, and coordinate-policy fingerprints;
- exact context-result identity, reset-boundary/anchor provenance, and captured fields required by the
  selected versioned model bundle;
- predictor build and model-bundle identity with evidence/completeness;
- explicit update/pause/path/node/time search bounds and completion status;
- selected witness path plus complete movement/no-movement/interruption schedule and ordered trace
  references; and
- terminal reason, predicted encounter/formation, metrics, warnings, and diagnostics.

A fixed-path result provides a reachable route prefix and cutoff. A branching search provides a
layer-preserving set of reachable triangle/local-coordinate states and may produce multiple frontier edges
keyed by `NavigationTriangleKey`; a single furthest point exists only under a declared progress or
optimization metric. Crossing one point in a triangle does not prove the entire triangle reachable. For a
no-encounter objective, retain the last confirmed safe point and the subsequent predicted encounter when
known. For a specific-encounter objective, retain target-hit locations rather than presenting a misleading
safety frontier.

Prediction Reachability is not a probability heatmap or universal boundary. Search-budget,
prediction-horizon, schedule-exhaustion, event-interruption, model-incomplete, and control-divergence
frontiers remain labeled by their terminal reason. A fraction of sampled schedules is search evidence, not
natural encounter probability.

The predictor output is planning-level world-space behavior, not a controller tape or camera solution.
One prediction spans one `NavigationEpoch`: a same-script cutscene that returns control stays in the epoch
and is modeled or produces `ModelIncomplete`, while battle entry terminates the epoch. A post-battle plan
references a separately captured ready context. The unfinished Moonfish/field analysis is not used here;
required field-RNG behavior remains a versioned model-completeness dependency.

Compatibility is derived from immutable fingerprints:

- spatial mismatch or unsupported schema is `Incompatible` and is not overlaid;
- matching spatial data with an older start/model/objective is `StaleContext` and may be shown only as
  historical with an explicit treatment;
- missing legacy fingerprints is `Unverifiable`, never assumed compatible; and
- an exact match is `Current`.

Safe profiles have no encounter model, encounter cost, or encounter-suppression requirement.

## Runtime and Workflow Boundaries

| Project | Navigation responsibility |
|---|---|
| SPICE | AKLZ and MLD/SCT/ECT parsing |
| `SavorNavigation` | Profiles, scenarios, graph/search, static analysis, exploration/refinement interpretation, and predictor-result spatial compatibility/projection |
| `SavorPredict` | Planned planning-level field/RNG/movement evolution, outcome search, witness selection, and frontier computation; no raw controller/camera realization; current executable/CLI requires a navigation boundary |
| `SavorQt3D` | Prototype loading, selections, visualization, diagnostics, and optional predictor-overlay fixture rendering; no predictor search |
| `SavorCore` | Runtime memory/breakpoints, inputs, checkpoints, telemetry, and future patch-control/context-capture Navigation PhaseScripts |
| `SavorWorker` | Isolated Dolphin-backed context, exploration, control-solve, and validation execution |
| `SavorWorkflow` | Coordinates generic claiming, dispatch, fan-out, and transition invocation |
| `SavorDb` | Workflow lifecycle, authored specifications, transition services, artifacts, analysis results, and UI projections |
| `SavorQt` | Later product host plus a separate outcome-planning module for authoring, launching/monitoring, comparing, and selecting predictor results |

The existing hidden `dungeon_explorer` workflow unit is the future Dungeon entry point. A future
`safe_explorer` sibling reuses shared execution contracts without dungeon encounter work. The existing
`overworld_explorer` placeholder remains independent.

Static classification, world building, route planning, deterministic survey reduction, and analytic encounter exposure are CPU/domain
operations. The present worker protocol executes fixed `PhaseScriptVM` programs, so those CPU operations
must not be disguised as simulator jobs until SAVOR has a local CPU-executor lane. Simulator programs are
reserved for Navmesh Survey anchor expansion, collision/oddity/trigger probes, reproduction attempts,
control solving, and encounter-enabled runtime validation. Modeled seeded path/schedule search belongs to
the future `SavorPredict` boundary; the Navigation widget only renders its selected result.

Encounter suppression is independent from trigger control. Trigger suppression/permission may change
dynamically during one survey job, especially around required doors, but the mechanism and modes remain
research-gated. Every derived anchor, savestate, control change, and observation is patched survey evidence
only: it may refine local/sub-triangle passability or stateful transitions but never becomes a
`NavigationContextResult` input or clean validation state.

## Failure Semantics

- MLD failure follows the existing scenario-load failure policy.
- Missing or failed SCT parsing preserves manual geometry/pathfinding but marks script/start/transition
  information incomplete.
- ECT I/O, AKLZ, or parse failure preserves Dungeon classification and ordinary pathfinding while disabling
  table-backed encounter results. Raw triangle-selector visualization may remain available.
- Out-of-range tables, missing metadata, unsupported selectors, and ambiguous overlapping resources retain
  explicit incomplete diagnostics; none are treated as no-encounter.
- Unknown start state or check cadence blocks marginal exposure, not static encounter-field inspection.
- `NonResetContinuation`, `VelocityNonZero`, `ControlNotStable`, `ResetStateMismatch`, and
  `IncompleteCapture` block predictor use while preserving static inspection and diagnostics.
- An unsupported same-script interruption yields `ModelIncomplete`; it never restarts prediction from an
  assumed clean entry. Battle entry terminates the epoch and requires a separate post-battle context.
- Worker timeout, divergence, or missing telemetry produces no empirical claim and does not overwrite
  structural or marginal results.
- Predictor schema/fingerprint mismatch, missing coverage/witness artifacts, incomplete models, or
  budget/horizon termination never invalidates the underlying scenario or static encounter layer. Reject
  spatially incompatible overlays; mark historical/unverifiable context explicitly; preserve the previous
  compatible selected result when a replacement cannot be applied.
- Area 99 returns the Overworld/deferred profile and never falls through to Dungeon selector decoding.

## Reference Classification Examples

| Companion set | Profile and evidence |
|---|---|
| `a099a.mld`, `me099a.sct`, `a099a.ect` | Overworld; Area 99 precedence |
| `a099b.mld`, `me099b.sct`, no same-stem ECT | Overworld; Area 99 precedence |
| `a101b.mld`, `me101b.sct`, `a101b.ect` | Dungeon |
| `a004a.mld`, `me004a.sct`, no ECT | Safe; known traversable below `200a` |
| `a201a.mld`, `me201a.sct`, no ECT | Safe candidate; traversability unverified |
| MLD with neither matched SCT nor ECT | Unknown / View-only |
