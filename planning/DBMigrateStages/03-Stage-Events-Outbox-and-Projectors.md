# Stage 3 - Event Contracts, Outbox, and Projectors

## Objective
Implement v1 event catalog with per-event schema versioning and projector pipelines for UIRead and cross-context propagation.

## Exit Criteria
- Outbox tables active in all write contexts.
- Relay can publish events in order and mark published.
- UIRead projectors can build run summaries, seed probe views, and battle tree views.

---

## 3.1 Event Envelope Standard (All Contexts)

Required envelope fields:
- `event_id`
- `event_type`
- `event_version`
- `occurred_at_utc`
- `context_name`
- `aggregate_kind`
- `aggregate_id`
- `correlation_id` (optional)
- `causation_id` (optional)
- `payload_json`

Rules:
- `event_type + event_version` uniquely identifies payload schema.
- Breaking changes require a new event version.
- Projectors must be idempotent by `event_id`.

---

## 3.2 Event Catalog v1

## Execution events
1. `Execution.JobSetCreated.v1`
2. `Execution.JobQueued.v1`
3. `Execution.JobClaimed.v1`
4. `Execution.JobLeaseRenewed.v1`
5. `Execution.JobProgressed.v1`
6. `Execution.JobCompleted.v1`
7. `Execution.JobEventArchived.v1`
8. `Execution.JobRestored.v1`

## State events
9. `State.ObjectStored.v1`
10. `State.SavestateCreated.v1`
11. `State.SavestateDerived.v1`

## Analysis spine events
12. `AnalysisSpine.RunCreated.v1`
13. `AnalysisSpine.StateRefRegistered.v1`
14. `AnalysisSpine.LineageEdgeAdded.v1`
15. `AnalysisSpine.ArtifactLinked.v1`

## Analysis.SeedProbe events
16. `AnalysisSeedProbe.SetCreated.v1`
17. `AnalysisSeedProbe.RunRequested.v1`
18. `AnalysisSeedProbe.NeutralSeedRecorded.v1`
19. `AnalysisSeedProbe.GridSeedRecorded.v1`
20. `AnalysisSeedProbe.UniqueSeedRecorded.v1`
21. `AnalysisSeedProbe.RunCompleted.v1`

## Analysis.Battle events
22. `AnalysisBattle.BattleSetCreated.v1`
23. `AnalysisBattle.SeedCandidateDiscovered.v1`
24. `AnalysisBattle.TurnNodeCreated.v1`
25. `AnalysisBattle.TurnOutcomeRecorded.v1`
26. `AnalysisBattle.BranchSelectionRecorded.v1`

## Authoring events
27. `Authoring.PlanSaved.v1`
28. `Authoring.TemplateSaved.v1`
29. `Authoring.PredicateSpecSaved.v1`
30. `Authoring.SettingsSaved.v1`

## Archive events
31. `Archive.PackageCreated.v1`
32. `Archive.PackageIndexed.v1`
33. `Archive.RehydrateRequested.v1`
34. `Archive.RehydrateCompleted.v1`
35. `Archive.RehydrateFailed.v1`

---

## 3.3 SeedProbe Event Payload Requirements (v1)

### `AnalysisSeedProbe.NeutralSeedRecorded.v1`
- `seed_probe_run_id`
- `seed_probe_result_id`
- `neutral_seed_value`
- `source_kind`
- `recorded_at`

### `AnalysisSeedProbe.GridSeedRecorded.v1`
- `seed_probe_run_id`
- `seed_probe_result_id`
- `grid_seed_id`
- `seed_value`
- `seed_delta`
- `input_frame_ref` (optional)
- `recorded_at`

### `AnalysisSeedProbe.UniqueSeedRecorded.v1`
- `seed_probe_run_id`
- `seed_probe_result_id`
- `unique_seed_id`
- `seed_value`
- `seed_delta`
- `input_frame_ref` (optional)
- `recorded_at`

### `AnalysisSeedProbe.RunCompleted.v1`
- `seed_probe_run_id`
- `seed_probe_result_id`
- `neutral_seed_value`
- `grid_count`
- `unique_count`
- `result_status`
- `completed_at`

---

## 3.4 Projectors

## UIRead projectors

### `RunSummaryProjector`
Consumes:
- execution lifecycle events
- analysis run events
Outputs:
- `ui_run_summary`

### `SeedProbeProjector`
Consumes:
- all `AnalysisSeedProbe.*` events
Outputs:
- `ui_seed_probe_summary`
- `ui_seed_probe_values`

### `BattleTreeProjector`
Consumes:
- `AnalysisBattle.*`
Outputs:
- `ui_battle_tree_node`

### `ArchiveCatalogProjector`
Consumes:
- `Archive.*`
Outputs:
- `ui_archive_catalog`

## Projector Idempotency
- Maintain per-projector checkpoints in `ui_projection_checkpoint`.
- Use upsert semantics keyed by stable IDs.

---

## 3.5 Outbox Relay Behavior

1. Read unpublished outbox messages ordered by `outbox_id`.
2. Deserialize envelope.
3. Dispatch to projector handlers.
4. On success, mark `published_at`.
5. On failure, increment `attempt_count`, store `last_error`.
6. Dead-letter after configurable max attempts.

---

## 3.6 Test Plan

- Event serialization round-trip tests per event type.
- Version router tests (`v1` dispatch).
- Projector idempotency tests (same event twice).
- Relay failure/retry tests.
- SeedProbe projector tests for neutral/grid/unique counters and value lists.
