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

- `planning/NavigationPhase/` - future navigation phase with its first interactive loading/visualization
  slice implemented. A pinned SPICE submodule feeds the non-Qt `SavorNavigation` library, which converts
  canonical MLD data, native GRND and ground-role GOBJ meshes, and transiently projected `fxn=wall`
  object meshes into a SAVOR-owned model. `SavorQt3D` loads manually selected AKLZ-compressed MLD files
  asynchronously and hosts the prototype viewer. Coordinate calibration, adjacency/path search, workflow
  jobs, and persistence remain future work.
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
