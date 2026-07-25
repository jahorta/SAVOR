# Planning Document Status

This index marks active planning documents as current, historical, or future-facing. Current code remains
the source of truth when a plan conflicts with implementation.

## Current

- `planning/DBMigrateStages/` - current SavorDb bounded-context migration direction, with older stage notes
  retained as historical implementation context.
- `planning/DBMigrateWorkflows/` - current hybrid workflow direction, with historical phase-completion notes.
- `planning/DBMigrateQueues/` - current facade-first queued DB direction. Full command/query bus remains a
  future opt-in path for operations that outgrow the facade model.
- `planning/VISUAL_DEBUGGER_ARCHITECTURE_PLAN.md` - current visual debug replay architecture.
- `SavorDb/README.md` - current SavorDb workflow/authored-graph direction.

## Future Plans

- `planning/ExecutionRuntime/` - authoritative clean-slate execution-runtime refactor direction. It defines
  one typed `ProgramRuntime`/`ProgramExecutor`, explicit emulation-session services, scoped native actions
  and effects, immutable invocation/result/artifact contracts, current-phase migration, and the boundary
  between bounded worker programs and durable workflow/frontier orchestration. It supersedes target-runtime
  assumptions that serialize the current `PhaseScriptVM`, preserve `PSContext` as the public ABI, add
  controller-per-phase factories, or retain separate built-in and authored-program execution paths.
  Current code remains authoritative for implemented behavior.
- `planning/SavorPredict_NavigationModels/` - future SavorPredict field-navigation model direction,
  including reusable pathfinding, encounter-aware joint search, predicted navigation trajectories, and
  worker-driven live trajectory testing.
- `planning/NavigationPhase/` - future dungeon-navigation phase built on a shared, surface-constrained
  2.5D foundation. The foundation preserves full XYZ geometry, vertically stacked walkable surfaces, and
  explicit 3D links/portals; "2.5D" means movement is constrained to authored surfaces, not that geometry
  is flattened to X/Z. SAVOR owns navigation orchestration, planning, solver execution, UI, and persistence;
  SPICE owns MLD/SCT and other SoA filetype parsing plus area-content generation. A pinned SPICE submodule
  feeds the non-Qt `SavorNavigation` library, which converts
  canonical MLD data, native GRND and ground-role GOBJ meshes, and transiently projected `fxn=wall`
  collision meshes, SPICE-classified trigger object meshes, and provisionally classified `motscpt`
  MovingObjects into a SAVOR-owned model. `SavorQt3D` loads manually selected AKLZ-compressed MLD files
  asynchronously and hosts the prototype viewer. The current prototype also includes same-directory
  `aNNNC.mld` -> `meNNNC.sct` discovery with strict area-key matching, in-memory SCT retention, an
  opcode-77 start catalog with condition/call provenance, a triangle/portal traversal graph, manual or
  catalogued start selection with facing, manual ground or projected-trigger goal selection, deterministic
  A*, and route/link/endpoint overlays. Trigger goals resolve to walkable graph geometry and display their
  projected bounds. Ground handoffs now preserve each MLD entry's ordered EntryID fallback chain. Static
  graph construction tests the complete current-entry GRND/GOBJ collision bundle first; only a current
  bundle miss may transfer to the first linked bundle that accepts the continuation. This replaces the
  earlier coincident-external-boundary heuristic and permits natural GRND-to-GOBJ staircase transitions.
  EntryID `0`, authored order, missing-entry truncation, and same-entry multi-resource continuity remain
  explicit SAVOR-owned evidence. Motion-bearing or non-`ground` entries are rendered as runtime-dependent
  bind-pose candidates but are excluded from A* until their runtime state is modeled.

  The approved future `NavigationAreaProfile` classification is ordered:

  | Priority | Profile | Classification evidence |
  |---:|---|---|
  | 1 | Overworld | Area key begins with `099`, regardless of SCT or ECT presence |
  | 2 | Dungeon | Non-099 MLD has a same-key ECT; ECT presence, not parse success, selects the profile |
  | 3 | Safe | Non-099 MLD has a matched SCT and no same-key ECT |
  | 4 | Unknown/View-only | Neither a matched SCT nor ECT establishes an earlier profile |

  Profile derivation treats the selected MLD directory as the complete companion set. A strict manual SCT
  may restore script association, but this milestone has no manual ECT override.

  Therefore `a099a/me099a/a099a.ect` and `a099b/me099b` are both Overworld,
  `a101b/me101b/a101b.ect` is Dungeon, `a004a/me004a` without ECT is known-traversable Safe, and
  `a201a/me201a` without ECT is an unverified Safe candidate. An MLD without SCT or ECT remains
  Unknown/View-only. Dungeon is the current phase target. Safe areas may reuse the same 2.5D foundation
  later, while Area 99/Overworld movement and encounter semantics are explicitly deferred to a separate
  design. `SpiceEct` is a future private parsing dependency behind `SavorNavigation`; it is not integrated
  into the current prototype, and no SpiceEct type will cross into Qt. Automatic profile enforcement,
  game-state evaluation, opcode-156 runtime restoration, transition semantics, moving-object/door
  semantics, trigger activation semantics, workflow jobs, and persistence remain future work.
  Runtime-perfect wall blocking, triangle-flag filtering, step-up limits, animation/script-driven ground
  state, and calibration against the game's active collision-selector modes also remain future work.

  A later predictor-backed slice adds a separate outcome-planning module in `SavorQt`. That module will
  consume the surveyed per-area world/refinement plus an explicitly defined prediction-start state and
  invoke the existing `SavorPredict` predictor subsystem through a future asynchronous boundary to search paths and
  movement/no-movement/interruption schedules for objectives such as no encounter or a specific encounter.
  The reusable Navigation widget will not run or rank those searches. It will validate and render a
  selected immutable prediction result as either a route prefix/cutoff or a `NavigationTriangleKey`-keyed
  2.5D reachable set and frontier. The static selector/table overlay remains an independent spatial layer;
  a predictor frontier is objective-, start-state-, model-, and search-bound-qualified rather than a
  probability heatmap or universal safety guarantee. `SavorPredict` is currently an exploratory
  application/CLI, so its navigation API, process boundary, and artifact schema remain planned work.

  `planning/NavigationPhase/NavigationContextWorkflow/` is the normative package for the draft
  Navigation Context capture, the planned Navmesh Survey, later predictor/control/validation work,
  workflow steps, and immutable artifact lineage. The first Survey slice uses the concrete
  `navigation-context-41.sav` and adjacent `.nctx` as one common per-area bootstrap. A finite first wave
  establishes in-area door anchors, briefly restores trigger commit only for an intended interaction,
  verifies TBLID/opening/crossing, and records any temporary BitVar unlock plus `initially_locked`
  constraint. Each anchor is replayed from the common baseline by teleport and settle. A second wave
  reloads that baseline and fans parallel spatial probes out from the verified positions. Area loads are
  separate files; anchors do not own savestates or serialized ground-selector state; persistent Survey
  evidence contains no probe timing. This direction supersedes fixed job-wide trigger-suppression,
  per-anchor-checkpoint, timing-based, and collision/anomaly-only Navmesh Survey sketches elsewhere in
  active Navigation planning.
  `planning/NavigationPhase/06-area-profiles-and-analysis-workstreams.md` remains normative for profile
  precedence and evidence levels.
- `planning/DBMigrateQueues/09-Phase-4-Implementation-Plan.md` - future hardening/tuning work, including
  stress-load analysis for current queued database facades.

## Historical / Reference

- `planning/SAVOR-architecture-research-report.md` - historical research context; do not treat exact service,
  schema, or citation details as current implementation truth.
- `planning/SAVOR-database-choices-research-report.md` - historical storage research context; current SavorDb
  code and DBMigrate docs supersede exact names/details.
- `planning/NavigationPhase/Resources/` and `planning/NavigationPhase/MLDParseLogs/` - historical and
  reference material for parser/viewer investigation. New parser implementation work belongs in SPICE.
- `planning/DTM-file-deep-research-report.md` - reference research.

## Retired

- `planning/NavigationPhase/SA3DPort/` - removed. Parser/reference-comparison work moved behind SPICE.
- `phase0/` SA3D parity/mapping docs - removed. Parser/reference-comparison work moved behind SPICE.
