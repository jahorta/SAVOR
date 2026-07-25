# Navigation Phase Planning

## Status

Mixed current/future plan.

SAVOR is the project owner for navigation semantics, route planning, eventual control solving and
simulator execution, UI selection, workflow orchestration, and result persistence. The current
implementation covers the in-memory `SavorNavigation` model/search boundary, the `SavorQt3D` prototype,
and a draft Navigation Context capture phase with an exported `a101b` `.sav`/`.nctx`. The Navmesh Survey,
broader workflow jobs, durable persistence, and installation in `SavorQt` remain planned work. SPICE
(Skies Package Interchange and Content Encoder) owns Skies of Arcadia filetype parsing and content
inspection. SAVOR vendors SPICE as a pinned git submodule at `third-party/SPICE`; the pin advances only
after required parser changes are regression-tested upstream. The implemented pathfinding slice uses
SpiceMLD and SpiceSCT at SPICE revision `0b82fe1`.

The dependency boundary is:

`third-party/SPICE -> SavorNavigation -> SavorQt3D prototype host -> eventual SavorQt integration`

The later predictor-backed flow is separate from that loading/widget dependency chain:

`Navigation Context export -> per-area Navmesh Survey/refinement -> nav.capture_prediction_start -> SavorPredict planning search -> NavigationControlSolveResult -> NavigationValidationResult`

`SavorPredict` is the existing predictor subsystem name. Its current exploratory executable/CLI shape is
not yet a SavorQt integration API; the navigation prediction surface and asynchronous invocation boundary
remain future work.

The normative lifecycle, content-provenance, context-capture, suppressed-exploration, prediction,
control-solving, validation, and workflow contracts are in
[`NavigationContextWorkflow/README.md`](NavigationContextWorkflow/README.md). This overview and documents
01 through 05 summarize that direction; the workflow package takes precedence for those contracts.
[`06-area-profiles-and-analysis-workstreams.md`](06-area-profiles-and-analysis-workstreams.md) remains the
normative source for area-profile classification and evidence levels.

`SavorNavigation` is a non-Qt C++20 static library. It is the only SAVOR project allowed to include
SpiceMLD or SpiceSCT types. It converts SPICE parse output into SAVOR-owned navigation types so the Qt
projects, planner, and future persistence code do not depend on either parser's internal model.

This folder tracks planning documents for a new **Navigation Phase** currently focused on start-to-end
movement within Skies of Arcadia Legends dungeons (for example, chest, door, or loading-zone objectives)
while minimizing total completion time in VI frames.

## Area scope and shared foundation

The common navigation foundation is **2.5D**: it preserves full XYZ mesh coordinates, vertically stacked
walkable surfaces, three-dimensional distance, and explicit directed links/portals, while constraining
player movement to authored walkable surfaces. It must never flatten overlapping Y layers into one X/Z
plane. Dungeon is the current target profile; the same foundation may support Safe areas later without
making their interaction or transition semantics part of the current dungeon milestone.

The planned `NavigationAreaProfile` classification is applied in this strict order:

| Priority | Profile | Classification evidence | Example |
|---:|---|---|---|
| 1 | Overworld | Area key begins with `099`, regardless of companion presence | `a099a/me099a/a099a.ect` and `a099b/me099b` |
| 2 | Dungeon | A non-099 MLD has a case-insensitively matched, same-key ECT | `a101b/me101b/a101b.ect` |
| 3 | Safe | A non-099 MLD has a matched SCT and no matched ECT | `a004a/me004a` is known traversable; `a201a/me201a` is unverified |
| 4 | Unknown/View-only | Neither a matched SCT nor ECT establishes a navigation profile | MLD without SCT or ECT |

ECT presence selects Dungeon before ECT I/O or parsing. An unreadable, undecompressible, or malformed ECT
makes encounter data incomplete but never reclassifies the area as Safe. Keys below `200a` are documented
as known-traversable Safe content; SCT-paired keys at or above `200a` remain Safe candidates until their
traversability is validated. Profile derivation treats the selected MLD directory as the complete companion
set. A strict manual SCT may restore script association, but this milestone has no manual ECT override.

This classification and its ECT discovery are approved planning decisions, not current prototype
behavior. The existing file picker may still load and inspect any MLD that SpiceMLD can parse. A future
private `SpiceEct` dependency in `SavorNavigation` will parse related ECT content; SPICE will own ECT bytes,
AKLZ handling, and file-format diagnostics, while SAVOR will own companion association, ordered profile
classification, dungeon encounter semantics, and conversion into SAVOR-owned types. SpiceEct types must
not cross the `SavorNavigation` boundary.

Area 99/Overworld navigation is explicitly deferred. Its free-flight movement, altitude-dependent state,
and special encounter lookup are not extensions of the current dungeon rules and must receive a separate
design before being admitted to planning or workflow jobs.

The authoritative SavorPredict model design for reusable field navigation, encounter-aware search,
predicted navigation trajectories, and live worker trajectory testing is maintained separately in
`planning/SavorPredict_NavigationModels/`. This folder remains responsible for the broader phase,
workflow, content-integration, UI, and persistence direction.

## Goals

- Establish the shared surface-constrained 2.5D model/search foundation, with Dungeon as the current
  supported navigation target and Safe reuse reserved for a later milestone.
- Load a manually selected, AKLZ-compressed MLD through SpiceMLD and construct an in-memory,
  SAVOR-owned representation of the area's walkable space.
- Discover and parse the MLD's strictly matched SCT through SpiceSCT, retain it in memory, and expose
  statically resolvable opcode-77 placements as optional route starts with their condition provenance.
- Plan objective-to-objective movement with a frame-time cost function.
- Run a per-area Navmesh Survey from one explicitly named Navigation Context `.sav`/`.nctx` bootstrap to
  establish spatial passability, replay-verified positional anchors, collision boundaries, door portals,
  and initial lock constraints.
- Account for interruptions without inventing reset boundaries: a same-script cutscene remains inside one
  navigation epoch, while entering battle terminates the epoch and a reset-qualified field return starts a
  new context.
- Refine candidate input tapes in simulator for time-optimal results.
- Add a later `SavorQt` outcome-planning module that invokes `SavorPredict` to search paths and
  movement/no-movement/interruption schedules for a requested result such as no encounter or a specific
  encounter.
- Project a user-selected prediction result onto the Navigation map as a provenance-checked reachable
  route prefix or 2.5D frontier without making the widget run the predictor search.
- Integrate into existing SAVOR job/phase infrastructure.
- Render a 3D area view highlighting walking planes and potential navigation targets such as script
  triggers, treasure chests, doors, and load/zone transitions.

## First implementation milestone

The interactive prototype will:

1. Use a file picker to select an `.mld` file and remember the last directory.
2. Derive the case-normalized area key from a conforming `aNNNC.mld` name and look only beside that MLD
   for `meNNNC.sct`. Parse a matching sibling automatically; if it is absent, warn and offer a manual SCT
   picker. A manual SCT may come from another directory but must carry the same prefix-stripped area key.
3. Read and parse the selected MLD and any matched SCT asynchronously through `SavorNavigation`,
   SpiceMLD, and SpiceSCT.
4. Let SPICE detect and decompress AKLZ data; SAVOR will not implement a second decompressor, MLD parser,
   or SCT parser.
5. Parse a canonical SpiceMLD `MldFile`, then convert its ground resources plus a transient compatibility
   projection into an in-memory `NavigationAreaModel`. GRND and ground-role GOBJ data come directly from
   `MldFile.groundResources`; exact `fxn=wall` NJ object geometry and every SPICE-classified trigger's
   attached object geometry, plus exact normalized `motscpt` object geometry, are flattened through an
   in-memory `BlenderIrScene` and immediately converted to SAVOR-owned region meshes. GOBJ blocks referenced
   only as objects are not walkable surfaces.
6. Retain normalized SCT source/status/section summaries and a SAVOR-owned opcode-77 start catalog in
   `NavigationScriptModel`, with the full parse result behind an internal opaque boundary. Preserve branch,
   switch, nested-condition, and resolved-call provenance without exposing SpiceSCT types.
7. Derive the walkable triangle/portal graph. Preserve each source entry's ordered linked-EntryID fallback
   chain, test its complete GRND/GOBJ collision bundle first, and create a directed handoff only where the
   current bundle stops accepting movement and the first accepting linked bundle continues it. Keep manual
   point selection as the default, optionally resolve a catalogued opcode-77 placement to the graph start,
   let the user pick a ground or projected-trigger goal, run deterministic A*, and render handoff and route
   overlays in the reusable Navigation widget hosted by `SavorQt3D`.

## Implemented prototype slices (2026-07-18 through 2026-07-19)

- The geometry-projection slice was validated at SPICE revision `8ebdf50`; the current pathfinding work
  pins the tested SpiceMLD/SpiceSCT revision `0b82fe1` under `third-party/SPICE`.
- `SavorNavigation` loads compressed MLD files, owns the public model and diagnostics, applies the
  centralized identity coordinate policy, and marks incomplete ground decoding as a partial model that
  is not pathfinding-ready.
- `SavorQt3D` loads through a file picker on a background worker, preserves the last directory, and
  renders GRND and ground-role GOBJ surfaces, real `fxn=wall` collision-boundary meshes, projected trigger
  meshes, provisionally classified `motscpt` meshes in a separate orange `MovingObjects` layer, and the
  remaining collision and unknown markers. A trigger or MovingObject whose attached geometry cannot be
  projected keeps a warning-backed approximate cube; an exact wall never uses that fallback.
- `a101b.mld` is the first fixture contract: 6 GRND surfaces and 6 ground-role GOBJ surfaces (504 vertices
  and 401 triangles total), 59 collisions including 51 exact wall regions projected as 517 mesh
  instances (6,795 vertices and 8,347 triangles), 16 fully projected triggers totaling 38 meshes,
  320 vertices, and 384 triangles, 11 fully projected `motscpt` MovingObjects totaling 55 meshes,
  462 vertices, and 566 triangles, and 21 remaining unknown entries.
- The geometry-only slices originally deferred link derivation, endpoint selection, and path search; the
  implemented pathfinding slice below now supplies those capabilities.

## Implemented pathfinding slice (2026-07-19 through 2026-07-20)

- SpiceSCT is a private `SavorNavigation` dependency, and the prototype loads at most one SCT associated
  with the current MLD. A missing, unreadable, or malformed SCT is recoverable and never disables manual
  pathfinding; a mismatched manual replacement is rejected before parsing and leaves the current valid
  SCT unchanged.
- `File -> Load Related SCT...` provides the strict manual replacement path. Opening another MLD clears the
  current SCT association and repeats sibling discovery; the recent-files submenu remains MLD-only.
- Generic relative `CallSubscript` target resolution and typed numeric literals are corrected and covered
  upstream at the pinned SPICE revision. `SavorNavigation` uses those foundations to catalog opcode-77
  placements without exposing SPICE control-flow types.
- Statically readable opcode-77 placements reached from `init` or the `BitVar 1910 == 0` loop
  initialization path are classified as authored arrivals. Other opcode-77 occurrences remain
  lower-confidence scripted repositions. Every condition and call path is preserved; incomplete placements
  remain visible but unavailable for anchoring.
- A grouped `Start:` selector is present in the Path toolbar. `Manual point` remains the default; choosing a
  resolvable catalog option anchors it to the uniquely nearest triangle on the selected ground `tblId`.
  Ambiguous, missing-ground, or otherwise unresolvable options never replace a valid current start. Manual
  start facing is settable; a catalogued option's opcode-77 yaw supplies its scripted facing when known.
- The traversal graph has one stable node per valid walkable triangle and bidirectional shared-edge
  adjacency. Cross-resource traversal follows the runtime-backed MLD contract: treat all GRND/GOBJ
  surfaces owned by one entry as a collision bundle, preserve linked EntryIDs including `0` and their
  authored order, query the current bundle first, and only on a miss accept the first linked bundle whose
  geometry continues the motion. The derived handoff need not join coincident external mesh boundaries;
  this allows a GRND edge to continue onto an interior landing of a ground-role GOBJ staircase.
- Exact normalized `ground` entries without motion resources participate in static A*. Motion-bearing and
  non-`ground` entries retain visible bind-pose geometry and conditional handoff diagnostics but are
  excluded from traversal until animation/script state is modeled. The Links layer distinguishes active
  static handoffs from muted-orange runtime-dependent candidates.
- Manual starts and ordinary goals can be picked on ground. Goals can also select real projected trigger
  meshes; warning-backed trigger fallback cubes are never selectable. Trigger selection deterministically
  chooses the largest projected mesh by its world-space bounds, resolves it to walkable graph geometry, and
  renders the chosen bounds as a magenta AABB.
- Start and ground-goal markers use the same compact fixed-size scale as the `man` fallback marker. Start
  facing is rendered as a short ray. Opcode 156 remains excluded because its transform comes from unresolved
  runtime restore state; trigger activation semantics, automatic condition evaluation, funnel smoothing,
  alternative routes, and other SCT-derived endpoints remain later work. Wall blocking, decoded triangle
  flags, player step-up rules, moving-ground state, and exact runtime collision-selector calibration are
  also deferred; the static handoff graph is candidate topology rather than complete runtime proof.

## Planned Navigation workflow

The eventual `SavorQt` product integration will contain a planning module separate from the reusable
Navigation map. The planning module authors an objective and search bounds, selects a compatible per-area
world/refinement and an explicitly defined prediction-start state, invokes `SavorPredict`, monitors
candidate results, and lets the user select one result for display. Initial Dungeon objectives include
reaching as far as possible without an encounter and obtaining a particular encounter. The widget does
not simulate, enumerate, or rank movement schedules.

The Navmesh Survey spans `nav.explore_geometry` and `nav.build_refinement`. Its first wave starts from one
common Navigation Context `.sav`/`.nctx`, crosses required in-area doors, and publishes positional anchors
only after replaying each position from the common baseline and observing a usable settle. Its second wave
reloads that same baseline, teleports parallel workers to those anchors, and records spatial
passability/collision observations for deterministic reduction.

Encounter suppression writes one zero byte to `0x8030b7ad`. Trigger commit is normally bypassed at `0x80117e8c`;
a door worker briefly restores the original instruction for the intended interaction, immediately
reinstalls suppression, verifies the expected TBLID, and proves physical crossing. A known lock BitVar may
be overridden inside the disposable job when the portal retains its initial lock constraint. The normative
direction in
[`NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`](NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md)
supersedes older fixed job-wide trigger-suppression, per-anchor-savestate, timing-based Survey, and
collision/anomaly-only sketches in the active Navigation documents.

A later prediction-only `NavigationEpoch` starts only after a reset-qualified field script/context switch or battle return has
restored stable player control. Readiness is measured from a clean, unpatched source state: placement and
ground are settled, velocity is zero, authored input is neutral, no forced action is pending, and required
reset-state capture matches the current contract. Entering battle ends the epoch. A same-script cutscene
that returns control does not reset `stepCount`, does not create a new context, and must remain within the
same prediction; an unsupported interruption returns `ModelIncomplete` instead of restarting from a clean
entry assumption. Save load qualifies only when runtime evidence confirms a reset-producing script switch.

The selected immutable prediction result carries the complete provenance needed to interpret its spatial
projection: `NavigationPredictionStart` identity, disc/content/world/graph and coordinate-policy identity,
predictor/model versions, objective and search bounds, witness path and planning-level
movement/no-movement/interruption schedule, predicted trace, terminal reason, and search completeness.
`SavorPredict` does not choose raw stick or camera inputs. `NavigationControlSolveResult` owns controller
tape and camera realization; `NavigationValidationResult` records comparison against a clean runtime. A
fixed-path result supplies a reachable route prefix and cutoff. A branching path search may supply a
`NavigationTriangleKey`-keyed 2.5D reachable set with one or more frontier edges. A search-budget or
prediction-horizon boundary must be labeled as such; it is not proof that gameplay cannot continue beyond
it.

The static encounter selector/table overlay remains separate. A predictor result is an objective- and
state-qualified **Prediction Reachability** overlay, not a probability heatmap. The widget derives
compatibility from immutable fingerprints: spatial incompatibility rejects the overlay; matching geometry
with an older start/model/objective may be shown as historical/stale; missing fingerprints are
unverifiable; and an exact match is current. `SavorNavigation` owns the Qt-neutral spatial compatibility
and projection boundary, while `SavorPredict` owns temporal state evolution, outcome search, witness
selection, and frontier computation.

The local US disc dump at
`D:\SoAGC\2002-12-19-gc-us-final_Skies_of_Arcadia_Legends` is a development fixture and convenient
initial directory. It must not become a hardcoded default or checked-in configuration value.

The first milestone does not require Dolphin/ISO file acquisition, disc-wide area lookup, automatic
`NavigationAreaProfile` enforcement, SpiceEct integration, automatic SCT state evaluation, opcode-156
runtime restoration, a serialized SPICE area-view artifact, or durable navigation persistence. Area 99
and other Overworld planning remain outside this milestone regardless of what the viewer can display. It
also does not embed `SavorPredict` execution or movement-schedule search in `SavorQt3D` or the Navigation
widget.

## Documents

- `01-scope-and-success-criteria.md`
  - Problem statement, non-goals, and measurable success criteria.
- `02-state-and-world-model.md`
  - Proposed navigation state representation and geometry/cutscene modeling.
- `03-search-and-optimization-strategy.md`
  - Multi-stage planning strategy (A* + simulator refinement).
- `04-phase-and-job-integration.md`
  - Proposed phase contracts, step kinds, and artifacts in existing system.
- `05-open-implementation-questions.md`
  - Unresolved decisions and experiments to de-risk implementation.
- `06-area-profiles-and-analysis-workstreams.md`
  - Normative profile precedence, shared component boundaries, and collision, anomaly, and encounter
    workstreams.
- `NavigationContextWorkflow/`
  - Implemented Navigation Context bootstrap contract, planned per-area Navmesh Survey, and separate
    future prediction lifecycle, control/validation, phase/job, artifact-lineage, and research contracts.

## Iteration approach

These are initial planning docs with open implementation details. We should expect to revise aggressively as we prototype extraction, heuristics, and determinism checks.

`SavorQt3D` is the standalone development and test host for the reusable widget. Its existing Quick 3D
renderer, file picker, visibility controls, and diagnostics are prototype assets to retain. Its dependency
on the removed `SavorMLD` project was obsolete and has been replaced by `SavorNavigation`. Once the widget
and model boundary are stable, the widget will be installed into `SavorQt`.

## Ownership boundary and current reality

- SPICE currently owns AKLZ handling, MLD/SCT parsing, low-level GRND/GOBJ extraction, and parser
  diagnostics through SpiceMLD and SpiceSCT. SpiceEct will own ECT parsing when that future dependency is
  introduced; SPICE does not own SAVOR area-profile or navigation semantics.
- `SavorNavigation` currently owns the adapters from SpiceMLD/SpiceSCT results into SAVOR models, strict
  MLD/SCT area-key association, opcode-77 start cataloguing/resolution, coordinate policy, traversal graph,
  deterministic A*, and geometry-completeness diagnostics. It will later own related-ECT association,
  ordered `NavigationAreaProfile` classification, dungeon encounter interpretation, and SAVOR-owned
  navigation serialization/domain contracts; `SavorDb` owns durable storage.
- `SavorQt3D` currently owns the standalone prototype host and reusable Navigation widget. `SavorQt` is the
  eventual product host; that integration is not implemented yet. Its planned outcome-planning module will
  launch and select `SavorPredict` navigation results, while the widget remains a result renderer rather
  than a schedule-search surface.
- `SavorPredict` is the intended non-Qt owner of modeled field-update/RNG evolution, path and
  movement/no-movement/interruption schedule search, requested-outcome evaluation, witness selection, and reachability
  frontier computation at the planning level. It will consume an explicitly defined prediction-start
  state separate from the Survey-bootstrap `.nctx`; it does not own raw controller/camera realization. Its existing exploratory
  executable/CLI must gain a deliberate navigation prediction and asynchronous integration boundary; no
  such SavorQt-facing API is implemented here.
- `SavorCore` already owns runtime memory, breakpoints, inputs, savestates, telemetry, the PhaseScript VM
  boundary, and the draft Navigation Context PhaseScript. Navmesh Survey patch, teleport/settle, and probe
  operations remain future. `SavorWorker` already provides Dolphin-backed execution and will later run
  Navigation probes.
- `SavorWorkflow` already coordinates generic claiming, dispatch, fan-out, and transition invocation
  through `SavorDb` services; Navigation integration remains future. A local CPU execution lane does not
  exist yet, so deterministic navigation analysis remains in-process domain work until that lane is
  designed.
- `SavorDb` owns workflow lifecycle, authored specifications, transition services, artifacts, analysis
  results, and UI projections.
- SA3DPort planning has been retired from this repo. Any parser/reference-comparison work belongs behind
  SPICE.
