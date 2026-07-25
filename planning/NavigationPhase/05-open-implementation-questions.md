# 05 - Open Implementation Questions

## Status

Future plan. File parsing questions are SPICE-owned. This document tracks resolved SAVOR integration
decisions and the remaining navigation-model, pathfinding, workflow, and UI questions.
Workflow-specific decisions and research questions are normative in
[`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md); this file is the cross-cutting index.
The draft Navigation Context capture exists. Navmesh Survey decisions below are resolved planning, not an
implemented worker phase.

## Resolved Decisions

### A. Dependency and ownership boundary (2026-07-18)

1. Vendor SPICE as a pinned git submodule under `third-party/SPICE`. The implemented rendering slice used
   `8ebdf50`; the implemented pathfinding slice advances the pin to tested SpiceMLD/SpiceSCT revision `0b82fe1`.
2. Add `SavorNavigation` as a non-Qt C++20 static library.
3. Only `SavorNavigation` may include SpiceMLD, SpiceSCT, or SpiceEct types. It converts parser results into
   SAVOR-owned scenario/area/script/encounter models, search and analysis types, and normalized diagnostics.
4. SPICE owns AKLZ decompression, MLD/SCT/ECT parsing, and low-level content interpretation.
   `SavorNavigation` owns CPU-domain world adaptation, 2.5D analysis, pathfinding, collision/anomaly
   analysis, and normalized dungeon encounter analysis. `SavorQt3D`/later `SavorQt` own UI;
   `SavorWorkflow` coordinates generic claiming, dispatch, fan-out, and transition invocation through
   `SavorDb` services; `SavorDb` owns workflow lifecycle, authored specifications, artifacts, analysis
   results, transition services, and UI projections; `SavorCore` owns simulator-program/VM contracts; and
   `SavorWorker` executes Dolphin-backed programs.
5. There is no SAVOR-side fallback parser or decompressor.

### B. First input path and model lifetime (2026-07-18)

1. The interactive prototype selects an AKLZ-compressed `.mld` with a file picker and remembers the last
   directory.
2. The US disc dump at
   `D:\SoAGC\2002-12-19-gc-us-final_Skies_of_Arcadia_Legends` is a local development fixture only. It is
   not a hardcoded default, portable configuration value, or durable area identity.
3. Loading and conversion run off the UI thread; the completed model is applied on the UI thread.
4. SpiceMLD automatically detects and decompresses AKLZ data before parsing.
5. The runtime path parses canonical `MldFile` once, then consumes its ground resources plus compatibility
   `world` and `searchWorld` evidence.
6. Exact `fxn=wall` NJ object geometry is flattened through a transient in-memory Blender IR projection
   inside `SavorNavigation`. JSON export, persistence, and exposure to Qt remain prohibited; Blender IR
   otherwise remains optional SPICE validation tooling.
7. `NavigationAreaModel` and path overlays remain in memory during the interactive prototype. A durable
   `nav_world_blob` is deferred to workflow integration.
8. Serialized SPICE area views are not a required runtime or first-pass artifact contract.
9. SpiceSCT is a private dependency for the implemented pathfinding slice. SPICE retains ownership of generic
   relative `CallSubscript` target resolution and typed numeric-literal access; regression-test those
   fixes upstream before advancing SAVOR's pinned revision.
10. SpiceEct is the private parser dependency for the planned dungeon encounter slice. The complete
    companion set is inspected only beside the selected MLD, and same-key ECT presence establishes a
    non-099 Dungeon before parsing. There is no manual ECT override. Safe/no-encounter profiles treat ECT
    as not applicable. Area 99/indexed ECT is explicitly deferred rather than decoded as a dungeon.

### C. Prototype host and widget rollout (2026-07-18)

1. Revive `SavorQt3D` as a separate development and test application before installing the widget in
   `SavorQt`.
2. Retain its useful Quick 3D renderer, file picker, layer controls, diagnostics, and last-directory
   behavior.
3. Its obsolete dependency on the removed `SavorMLD` project has been replaced with `SavorNavigation`.
4. Build the viewer as a reusable Navigation widget inside the prototype host so it can later move into
   `SavorQt` without exposing SpiceMLD, SpiceSCT, or SpiceEct types.

### D. Geometry semantics

1. Use true 3D search state; do not flatten overlapping Y layers to 2D. A shared 2.5D analysis field may
   sample X/Z per resolved layer only when it retains Y plus stable surface/triangle identity and does not
   replace the true-3D graph.
2. Preserve SpiceMLD link/connectivity metadata while generating SAVOR navigation edges.
3. Keep coordinate conversion centralized in `SavorNavigation`; renderer-specific axis, winding, or scale
   patches are not allowed.
4. Start with coarse collision fidelity, then use worker probes to refine important bounds in later
   milestones.
5. A GOBJ block is part of the navigation surface set when referenced through an entry's
   `groundAddresses`. Consume it from canonical `MldFile.groundResources`, preserve its entry/block/node
   source identity, and exclude object-role-only GOBJ blocks.
6. A partially decoded ground set may be rendered for diagnosis, but it sets
   `hasCompleteGroundGeometry=false` and cannot enter path search.
7. Exact normalized `fxn=wall` entries use transiently projected object trees to create world-space
   `NavigationRegionMesh` instances. Missing wall geometry sets `hasCompleteWallGeometry=false`, makes the
   load partial, and suppresses the old cube fallback. `walluv` remains an ordinary marker in this slice.
8. Every SPICE-classified trigger receives the same projected object-tree treatment. Missing trigger
   geometry sets `hasCompleteTriggerGeometry=false` and increments `failedTriggerRegionCount`, but it is
   warning-only, does not alter load status, and retains an approximate red cube marker. The mesh is
   attached MLD geometry, not an inferred interaction radius or SCT/controller activation predicate.
9. Exact normalized `motscpt` entries are provisionally reclassified from preserved unknown entries as
   `MovingObject` regions and projected through the same path. They render in a separate orange
   `MovingObjects` layer. Missing geometry is warning-only with a cube fallback and does not affect load
   status. This name does not assert that every entry is a door or define runtime motion/collision behavior.

### E. Camera and controls

- Camera is not part of route-planning state.
- The later execution pipeline is route -> spline -> control-solver workers -> candidate selection.
- Add VM instructions for spline ingestion and iterative following with savestate checkpoints after the
  interactive pathfinding milestone.
- Static world adaptation, 2.5D construction, A*, collision diagnostics, anomaly candidate generation and
  result interpretation, and marginal dungeon encounter analysis are CPU-domain work. Dolphin workers are
  reserved for input execution, runtime collision validation, anomaly discovery/reproducibility probes,
  exact encounter cadence/shared-RNG order, and route validation.

### F. Cutscenes and optimization

- Use MLD trigger/flag combinations and later SCT analysis for likely cutscene starts and transitions.
- Treat cutscenes as non-branching for the current game scope.
- MVP uses baseline optimization and assumes deterministic replay from fixed savestate/input pairs.
- Add repeated validation only if failures are observed.

### G. Matched SCT loading (2026-07-19)

1. Recognize a conforming `aNNNC.mld` by its case-normalized, prefix-stripped `NNNC` area key and derive
   the sole expected script name `meNNNC.sct`.
2. On MLD load, search only its directory for the expected SCT. Do not perform a disc-wide search, infer
   alternative companions, or hardcode the local dump path.
3. Load a matching sibling automatically. If absent, keep the MLD usable, warn with the expected name,
   and offer `Load Related SCT...` or continuation without it.
4. A manual SCT may come from another directory but must have the same prefix-stripped key
   case-insensitively. Reject a mismatch before parsing, show expected and selected identities, and retain
   any current valid SCT.
5. Opening another MLD clears the previous SCT association. A nonconforming MLD remains viewable but has
   no SCT association.
6. Retain normalized SCT source/status/section/instruction summaries and a SAVOR-owned opcode-77 start
   catalog in the scenario, with the full `SctParseResult` behind an opaque private boundary. Missing or
   failed SCT parsing is advisory and never disables manual pathfinding.
7. Loading alone does not evaluate current game state, select an incoming transition, handle opcode-156
   restoration, or apply SCT/controller activation semantics.

### H. Traversal graph, ordered fallback chains, and route (updated 2026-07-21)

1. Create one stable node per valid walkable triangle; omit and diagnose degenerate, malformed, and
   non-manifold geometry.
2. Add bidirectional shared/tolerance-welded intramesh adjacency. For cross-resource traversal, preserve
   each source entry's ordered linked-EntryID fallback chain, including EntryID `0`, instead of flattening
   it into unordered source/target surface pairs.
3. Treat every GRND/GOBJ surface owned by the current entry as one collision bundle. At each source
   boundary interval, continue within that bundle when possible; only a complete miss enables the first
   linked bundle in authored order whose triangles provide height-continuous coverage.
4. Resolve linked EntryIDs with indexed lookup followed by the first linear EntryID match. A missing entry
   truncates the effective chain; a resolved entry with missing geometry remains diagnostic and does not
   erase later authored targets.
5. Test coverage against target triangle footprints rather than requiring coincident external boundaries.
   Choose a unique height-nearest target, keep tied stacked heights unresolved, and derive reverse traversal
   only from independently valid reverse evidence.
6. Manual Set Start picks use ground; Set Goal accepts ground or a real projected trigger mesh. Both resolve
   to `NavigationGraphAnchor`s with exact/snapped positions and snap distance. A selected resolvable
   opcode-77 catalog entry may instead set the start.
7. Run one deterministic A* route with stable tie-breaking, 3D Euclidean heuristic, and geometric
   centroid/portal cost plus a nonnegative coarse slope multiplier. Return start, portal midpoints, and
   goal; defer funnel smoothing, top-K alternatives, splines, and frame-time ranking.
8. Render active static handoffs in yellow and runtime-dependent bind-pose candidates in muted orange in
   Links, plus the path and green start/magenta goal markers in Route. Exact normalized `ground` entries
   without motion resources are static; motion-bearing or non-`ground` entries are excluded from A*.
9. Preserve short-click versus drag-to-orbit behavior and report graph, handoff, anchor, route, and
   unreachable diagnostics.

### I. Selectable opcode-77 starts (2026-07-20)

1. Analyze every opcode-77 occurrence after SpiceSCT parsing. An option is resolvable only when its ground
   selector and XYZ reduce to finite constants; retain yaw when known but do not require it for A*.
2. Preserve true/false branch polarity, switch choice/case values, nested predicates, resolved subscript
   calls, and instruction/section provenance. Retain unsupported or cyclic flow as opaque diagnostic
   conditions rather than dropping the occurrence.
3. Classify exact `init` placements, placements reached from `init`, and `loop` placements under
   `BitVar 1910 == 0` as `AuthoredArrival`; classify other occurrences as lower-confidence
   `ScriptedReposition`. Group only bit-exact placements of the same kind and retain each path as a variant.
4. Preserve raw `IntVar 15` values. Label `10000` as Battle Return and `20000` as Save Load; show a derived
   previous-area stem only when sibling filenames corroborate it.
5. Exclude opcode 156 completely because its saved transform provenance is unresolved. Battle-return
   navigation remains manual until runtime restore data is understood.
6. Resolve a selected available placement through `NavigationCoordinatePolicy`, match its selector to
   surface `tblId`, and choose the unique nearest matching triangle. Tied stacked surfaces are ambiguous;
   failed resolution preserves the current valid start.
7. The grouped Path-toolbar `Start:` selector has `Manual point` first and selected by default. It shows
   authored arrivals, scripted repositions, and disabled incomplete placements with condition/provenance
   tooltips. Manual picks return the selector to `Manual point`.

### J. Endpoint facing and projected-trigger goals (2026-07-20)

1. Start and ground-goal markers use the same compact fixed-size scale as the `man` fallback marker.
2. Manual start facing is settable and renders as a short ray; known opcode-77 yaw supplies scripted facing.
3. Set Goal accepts ground or a real projected trigger mesh. Approximate trigger fallback cubes and every
   other non-ground layer remain excluded from endpoint picking.
4. Trigger goal resolution chooses the largest usable projected mesh deterministically by world-space AABB
   size with stable ties, anchors its nearest unambiguous point to walkable graph geometry, and renders the
   chosen bounds as a magenta AABB.
5. Projected trigger selection defines only a route target and visualization. Runtime activation predicates,
   interaction radii, and transition execution remain deferred.

### K. Shared 2.5D, collision, and anomaly analysis (2026-07-21)

1. Build one shared layer-preserving 2.5D analysis field from the SAVOR-owned walkable surfaces and stable
   triangle keys. Each sample/partition retains X/Z, resolved Y, normal, surface/triangle identity,
   GRND/GOBJ provenance, collision evidence, and raw triangle metadata.
2. Stacked surfaces stay distinct. The field is an analysis projection and never replaces the true-3D
   graph, graph anchor, or portal model used by A*.
3. Static collision analysis and anomaly analysis are separate outputs over the same field. Anomalies may
   include missing/conflicting metadata, invalid seams, suspect portals, stacked-layer ambiguity, and
   later observed-versus-predicted simulator discrepancies.
4. Static diagnostics or simulator discrepancies do not silently mutate A*. A correction reaches the
   routing model only through an explicit, validated refinement rule with retained provenance.
5. The Navmesh Survey order is resolved: one common Navigation Context bootstrap, a finite in-area
   door-anchor establishment wave, clean-baseline teleport-and-settle verification, a parallel spatial
   probe wave, independent reproduction, and deterministic refinement. Timing-dependent oddities and
   broad trigger characterization are separate later work. Exact sampling, adaptive refinement,
   promotion thresholds, and durable schemas remain open.

### L. Dungeon-only encounter analysis (2026-07-21)

1. The first encounter profile is direct-table dungeon analysis. Inspect only the selected MLD directory
   for an exact case-insensitive same-key `aNNNC.ect`; its presence establishes a non-099 Dungeon before
   parsing. No manual ECT replacement is accepted. Opening a new MLD rebuilds the complete companion set.
2. SpiceEct owns AKLZ and flat-table parsing. `SavorNavigation` converts it into SAVOR-owned table,
   selector, route-exposure, source-status, and diagnostic types; no SpiceEct type crosses into Qt.
3. The first-pass authored-selector hypothesis masks `(triangleMetadata.rawU16[2] & 0x7fff)` and interprets
   its decimal tens digit. Preserve confidence/provenance for this interpretation independently from
   confidence in active runtime resource/triangle selection; parser success alone proves neither.
   Selector zero means no encounter-distance accumulation. Currently validated selectors 1 through 7
   resolve to 1-based flat ECT tables. Missing metadata is unknown rather than zero; selectors 8/9 and
   missing tables are unsupported/incomplete rather than guessed.
4. Apply the selector to both GRND and ground-role GOBJ triangles. `NavigationSurface::tblId` selects a
   collision resource and is never an encounter table ID. Trigger and collision-region meshes do not
   define encounter geography.
5. Preserve selector zero, missing metadata, unsupported selector, missing table, zero stage, zero overall
   rate, and malformed row pools as distinct results. Do not silently normalize malformed or ineligible
   formation rows.
6. The whole-area static selector/table field is independent of the selected start. Start-relative output
   segments only the current deterministic route and reports active/no-encounter geometric distance and
   table transitions. A destination is not assigned a unique cumulative risk without an explicit route
   policy.
7. Marginal or seeded analysis requires an explicitly referenced ready `NavigationPredictionStart` captured
   from a reset-qualified transition. It preserves measured `stepCount`, zero-velocity readiness, and
   independently captured encounter fields. A manual or opcode-77 spatial start does not prove this state.
8. A per-active-check probability can be exact under known S/W/M inputs. A cumulative survival product is
   a marginal independence approximation, not an exact seeded result. Exact check cadence, shared-RNG
   order, encounter outcome, and sequential formation selection require captured state and simulator
   execution unless the entire field update is later emulated.
9. Dungeon encounter analysis is visualization/diagnostics only in this slice. It does not alter graph
   readiness, A* edge costs, route ranking, trigger semantics, control solving, or failure of an otherwise
   usable navigation scenario.
10. Safe/no-encounter areas reuse the shared field, collision/anomaly, graph, route, and control pipeline
    with encounter status not applicable; missing ECT is expected for that profile.
11. Area 99 is explicitly deferred. Its selector is a local lane requiring pointwise position buckets,
    altitude, scenario page, `fldEfcontrol` payload, encounter-zone/table resolution, and runtime table-set
    context. Indexed ECT or `a099*` content never falls back to the dungeon direct-table analyzer.

### M. Explorer workflows and execution domains (2026-07-21)

1. Reuse the existing hidden `dungeon_explorer` workflow-unit contract in `SavorDb` for the first integrated
   Navigation workflow. Reuse the existing hidden `overworld_explorer` only after its overworld/Area-99
   profile is designed. Add a future `safe_explorer` that shares the base pipeline with encounters not
   applicable; do not create a parallel generic `navigation_phase` workflow unit.
2. The normative integrated sequence is `nav.capture_context`, `nav.materialize_content`,
   `nav.build_world`, `nav.explore_geometry`, `nav.build_refinement`,
   `nav.capture_prediction_start`, `nav.search_predicted_outcomes`, explicit result selection, `nav.solve_controls`,
   `nav.validate_route`, and `nav.publish_result`. The existing static A* operation remains a CPU-domain
   route planner, not a Dolphin program.
3. CPU-domain steps use `SavorNavigation` logic and are not Dolphin jobs. Until `SavorWorkflow` has a
   local CPU execution lane, these remain in-process domain operations outside worker dispatch. Later,
   `SavorWorkflow` may schedule them locally. Simulator programs such as `nav.solve_controls` and
   `nav.validate_route` are `SavorCore` contracts executed by `SavorWorker` from savestates/input tapes.
   `SavorDb` owns authored workflow specifications, lifecycle state, durable references, and publication
   through `nav.publish_result`.
4. Only simulator work fans out for Navmesh Survey anchor establishment, spatial collision probes,
   reproduction attempts, control attempts, or empirical validation. Later oddity and generalized trigger
   work may also use workers but is not part of the Survey contract. CPU analysis may use normal CPU
   parallelism but does not require PhaseScript or a Dolphin worker to build the world, run A*, reduce a
   refinement, or calculate assumption-labeled marginal dungeon encounter results.

### N. Area profiles and evidence boundaries (2026-07-21)

1. Profile classification uses the complete companion set beside the selected MLD and is ordered: every
   `099*` key is Overworld; otherwise a same-key ECT makes a non-099 area Dungeon; otherwise a matched SCT
   with no ECT makes it Safe; all other inputs are Unknown/View-only. ECT presence, not parse success,
   selects Dungeon, and there is no manual ECT override.
2. Area 99 precedence is absolute even when an ECT is present. It never falls through to the direct-table
   Dungeon analyzer.
3. Future Safe Navigation reuses the shared 2.5D geometry, collision/anomaly, graph, route, control, and
   event-continuation pipeline with encounter state explicitly not applicable.
4. Dungeon encounter exposure is analysis-only in the first slice and cannot alter A*, route ranking,
   triggers, control solving, or ordinary pathfinding readiness.
5. Marginal exposure requires an explicitly selected ready `NavigationPredictionStart`. A manual or
   opcode-77 spatial start never implies runtime encounter state or a reset boundary.
6. Parse status, geometry completeness, known traversability, and runtime collision confidence are
   independent evidence dimensions. A complete mesh conversion is not proof of runtime behavior.

### O. Predictor planning and Navigation-map boundary (2026-07-21)

1. Predictor-backed encounter-outcome search belongs to a separate future planning module in `SavorQt`,
   not to the reusable Navigation widget or `SavorQt3D`. The planning module authors the objective and
   bounds, selects a compatible world and explicit ready prediction-start ID, invokes the existing
   `SavorPredict` predictor subsystem through a future asynchronous boundary, compares results, and chooses
   one for display.
2. `SavorPredict` owns planning-level modeled field/RNG/movement-state evolution, path and
   movement/no-movement/interruption schedule search, objective evaluation, witness selection, and frontier
   computation. Its versioned model bundle declares completeness for required field RNG sources; unfinished
   Moonfish/field research is not used as behavioral input. Its current exploratory executable/CLI is not
   yet a reusable Navigation API; the integration mechanism remains open without changing this ownership
   decision.
3. The Navigation widget consumes one selected immutable result. It validates compatibility and renders an
   already-computed route prefix/cutoff or layer-preserving `NavigationTriangleKey`-keyed reachable set and
   frontier. It never enumerates, extends, or ranks movement schedules.
4. The static selector/table overlay remains independent from Prediction Reachability. A predictor frontier
   is qualified by objective, explicit prediction start, navigation-world/model fingerprints, and search bounds. It
   is not a probability heatmap or universal safety guarantee, and sampled-schedule frequency is not natural
   encounter probability.
5. A fixed-path result uses a route prefix and terminal cutoff. A branching search may use multiple 2.5D
   frontier edges; a single furthest point exists only under a declared progress/optimization metric. A
   search-budget, prediction-horizon, schedule-exhaustion, event-interruption, or model-incomplete frontier
   retains that terminal reason.
6. Immutable fingerprints derive compatibility rather than mutating the artifact with a stale flag. Spatial
   mismatch rejects display; matching geometry with older start/model/objective may be shown as historical;
   missing fingerprints are unverifiable; and an exact match is current.
7. Static/marginal, modeled predicted/seeded, and simulator-observed/validated encounter evidence remain
   separate. A selected predictor witness may show a path different from ordinary A*, but does not mutate
   A* costs or replace the static route result.

### P. Future prediction lifecycle and evidence layers (revised 2026-07-24)

The implemented Navigation Context `.nctx` plus savestate exists only to bootstrap the Navmesh Survey.
The reset/readiness decisions below describe a separate future `NavigationPredictionStart`.

1. A `NavigationEpoch` starts only after a completed field script/context switch or battle return restores
   stable control and passes measured readiness. Entering battle terminates it. Save load qualifies only
   when runtime evidence confirms a reset-producing script switch.
2. Same-script cutscenes remain inside one epoch and do not reset `stepCount`. A predictor models the
   interruption or returns `ModelIncomplete`; it never creates a clean-entry context at cutscene return.
3. Context readiness requires a clean/unpatched source state, settled placement/ground, measured zero
   velocity, neutral authored input, no pending forced action, and consistent reset-state capture. Camera/
   control orientation is excluded from `NavigationPredictionStart` and predictor input.
4. Static world, patched survey observations/refinement, prediction, control realization, and clean
   validation are separate immutable evidence layers. Encounter suppression is independent from trigger
   control. The first Survey slice uses the exact reversible callsite toggle documented below for door
   interaction; broader trigger allowlists and `eventhook` remain future work. Modified live state or a
   modified savestate is never promoted. A position discovered there becomes an anchor only after the
   common-baseline teleport/settle replay succeeds.
5. Disc identity is a streaming SHA-256 plus size, Game ID, region, and revision. Internal-file hashes and
   parser/model/schema/coordinate-policy versions key content bundles; a machine path is only a locator.
6. Every downstream prediction explicitly references a prediction-start ID. `SavorPredict` emits a
   world-space planning route and movement/no-movement/interruption
   schedule; the control solver owns stick/camera realization and clean validation owns runtime comparison.
7. The normative details and remaining workflow research questions live in
   [`NavigationContextWorkflow/`](NavigationContextWorkflow/README.md).

### Q. Navmesh Survey direction (revised 2026-07-24)

1. The Navmesh Survey spans Dolphin-backed `nav.explore_geometry` plus deterministic
   `nav.build_refinement`. It produces one runtime-refined spatial navmesh per loaded area; area loads are
   separate files.
2. The current `a101b` bootstrap is the explicitly named `navigation-context-41.sav` and adjacent
   `navigation-context-41.nctx`. Every worker reloads that same baseline.
3. Encounter suppression writes one zero byte to `0x8030b7ad`. Trigger commit is normally suppressed by writing
   `0x48000018` at `0x80117e8c`; a door worker briefly restores original word `0x480F86C5`, performs the
   intended interaction, and immediately suppresses again. The first slice uses no breakpoint, code cave,
   permanent hook, or `eventhook`.
4. A door activation must match the expected TBLID—currently witnessed through `0x8034744c` and
   `[object + 8]`—and must physically cross and settle on the far side. Generic trigger activity is not
   sufficient.
5. A disposable anchor-establishment job may read-modify-write a known lock BitVar. It records the
   original/override values and `initially_locked` on the portal. For `a101b` door `4101`, BitVar `2556`
   controls the lock at word `0x80310c78`, mask `0x10000000`; BitVar `1555` is the opening-completion
   witness at word `0x80310bfc`, mask `0x00080000`.
6. Wave 1 publishes positional anchors only after reloading the common baseline, teleporting to the
   proposed position, letting normal game updates reconstruct ground/collision state, and observing a
   usable settle. Anchors do not own savestates, full ground-selector records, or live pointers.
7. Wave 2 reloads the common baseline, teleports parallel workers to verified anchors, and records
   requested/resulting/settled positions plus pass/block/fall/correction and connectivity evidence.
8. Persistent Survey evidence is spatial. Runner safeguards do not become probe clocks, frame costs, or
   movement-optimization evidence. Timing-dependent anomalies and broad trigger-envelope characterization
   are separate later analyses.
9. The normative detailed contract lives in
   [`NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md`](NavigationContextWorkflow/04-suppressed-exploration-and-world-refinement.md).

## Remaining Open Questions

1. Exact `CoordinatePolicy` for axes, signs, scale, matrix orientation, and triangle winding after visual
   calibration against known field areas.
2. Minimum durable schemas for context, disc/content manifests, SAVOR world/refinement, optional encounter,
   prediction, control, validation, trace, witness, and reachability artifacts. The required lineage is
   resolved; concrete encodings remain open.
3. Automatic mapping from game/area identity to the full related package set beyond the resolved strict
   sibling MLD/SCT/ECT profile-classification contract.
4. Runtime state needed to evaluate opcode-77 condition paths automatically and select the correct arrival,
   plus the source/lifetime of the transform restored by opcode 156.
5. Simulator-worker batch sizing and retry strategy for the resolved two-wave Survey, plus context
   capture, control solving, and validation;
   plus the separate future `SavorPredict` process/library boundary, asynchronous invocation,
   cancellation/progress, CPU search fanout, and model-bundle versioning.
6. 2.5D sampling/refinement policy, collision-probe priority, settle tolerance, and evidence threshold for
   promoting a spatial correction into the route model.
7. Exact reset-state field inventory, capture breakpoints, save-load qualification, state-signature policy,
   dungeon check cadence, and the completed/versioned field-RNG model required for evidence-qualified
   `SavorPredict` results. The unfinished Moonfish/field analysis remains excluded until accepted.
8. Prediction search bounds/completeness, the progress metric for a single furthest point across multiple
   paths, partial-triangle coverage encoding, and the boundary between a modeled movement schedule and its
   executable controller-input realization.
9. Whether and how battle-time cost and post-battle resume behavior may eventually influence predictor or
   route ranking. No-encounter and specific-encounter objectives may drive the separate predictor search,
   but they do not affect A* in the planned first encounter slice.
10. Area-99-specific encounter profile and `overworld_explorer` behavior. The dungeon direct-table rule is
   not a provisional substitute.
11. Final named-objective/start interaction model after the opcode-77/manual point-to-point prototype is
    usable.
12. Runtime calibration for wall blocking, triangle-flag filtering, player step-up limits, animation/
    script-driven ground state, and collision-selector modes. The static collision-handoff graph deliberately
    does not guess these rules.
13. Generalized trigger identity and lifecycle beyond the resolved first-slice door toggle, including
    `eventhook`, initialization/environmental-controller preservation, automatic versus interactable
    behavior, and broad activation-envelope characterization.

The workflow-specific research list is maintained normatively in
[`NavigationContextWorkflow/07-open-research-questions.md`](NavigationContextWorkflow/07-open-research-questions.md);
this list retains only cross-cutting Navigation questions.

## Implemented First Slice (2026-07-18)

- The geometry slice was validated at SPICE revision `8ebdf50`; the implemented pathfinding slice uses the
  tested SpiceMLD/SpiceSCT revision `0b82fe1`. `SavorNavigation` remains the sole SAVOR consumer of those
  parser types.
- Direct AKLZ loading, normalized failure diagnostics, GRND plus ground-role GOBJ conversion, partial-model
  signaling, background Qt loading, and the retained viewer layers are implemented.
- The `a101b.mld` fixture contract is covered by an isolated navigation test target.
- Exact wall projection is covered by isolated hierarchy, repeated-instance, weighted-root, malformed
  triangle, and missing-reference tests plus the real `a101b.mld` count contract.
- Trigger projection reuses that tested projection path with explicit loader-supplied targets. The
  `a101b.mld` contract covers 16 complete triggers: 11 `goscript` meshes (88 vertices/132 triangles),
  20 meshes across 4 `treasure` entries (144 vertices/168 triangles), and 7 `wallmot` meshes
  (88 vertices/84 triangles), totaling 38 meshes, 320 vertices, and 384 triangles.
- The same fixture contains 11 exact `motscpt` MovingObjects, all complete, totaling 55 meshes,
  462 vertices, and 566 triangles; 21 entries remain in the generic Unknown category.

## Implemented Pathfinding Slice

1. The centralized coordinate policy remains identity for the slice, with one consistent graph,
   anchor, route, and future-SCT-coordinate boundary; calibration remains an explicit follow-up.
2. Strict sibling SCT discovery/matching and retention provide recoverable diagnostics and expose
   a condition-preserving catalog of opcode-77 authored arrivals and scripted repositions.
3. Traversable adjacency preserves ordered EntryID fallback chains and derives current-bundle-first
   collision handoffs from source-boundary continuation into target triangle coverage. The host accepts a
   manual or resolvable opcode-77 start and a manual ground or projected-trigger goal, and renders the first
   deterministic A* path overlay with start facing and endpoint-specific markers.
4. After this boundary is stable, add automatic condition/state evaluation, investigate opcode-156 runtime
   restore data, expand transition behavior, design full automatic content lookup, add jobs, and define the
   persisted `nav_world_blob`/`nav_script_index` schemas.
5. Correlate `motscpt` entries with controller/SCT behavior to determine which are doors and how their
   moving collision should affect navigation.
