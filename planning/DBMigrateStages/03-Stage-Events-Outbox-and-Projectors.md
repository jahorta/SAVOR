# Stage 3 - Event Contracts, Outbox, and Projectors

## Objective
Implement v1 event catalog with per-event schema versioning and projector pipelines for UIRead and cross-context propagation.

## Exit Criteria
- Outbox tables active in all write contexts.
- Relay can publish events in order and mark published.
- UIRead projectors can build job, seed probe, battle, artifact, and archive read models defined in Stage 2.

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
- `payload_ref_kind`
- `payload_ref_id`

Rules:
- `event_type + event_version` uniquely identifies payload schema.
- Breaking changes require a new event version.
- Projectors must be idempotent by `event_id`.
- Payloads should be read from typed source tables using (`payload_ref_kind`, `payload_ref_id`) rather than embedding large JSON blobs.

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
9. `State.ArtifactStored.v1`
10. `State.SavestateCreated.v1`
11. `State.SavestateDerived.v1`
12. `State.TasVariantCreated.v1`

## Analysis spine events
13. `AnalysisSpine.RunCreated.v1`
14. `AnalysisSpine.StateRefRegistered.v1`
15. `AnalysisSpine.LineageEdgeAdded.v1`
16. `AnalysisSpine.ArtifactLinked.v1`

## Analysis.SeedProbe events
17. `AnalysisSeedProbe.SetCreated.v1`
18. `AnalysisSeedProbe.RunRequested.v1`
19. `AnalysisSeedProbe.NeutralSeedRecorded.v1`
20. `AnalysisSeedProbe.GridSeedRecorded.v1`
21. `AnalysisSeedProbe.UniqueSeedRecorded.v1`
22. `AnalysisSeedProbe.EncounterProjectionRecorded.v1`
23. `AnalysisSeedProbe.RunCompleted.v1`

## Analysis.Battle events
24. `AnalysisBattle.BattleSetCreated.v1`
25. `AnalysisBattle.SeedCandidateAdded.v1`
26. `AnalysisBattle.TurnWaveCreated.v1`
27. `AnalysisBattle.TurnJobRecorded.v1`
28. `AnalysisBattle.SelectionPoolCreated.v1`
29. `AnalysisBattle.SelectionDecisionRecorded.v1`
30. `AnalysisBattle.TerminalFollowupUpdated.v1`

## Authoring events
31. `Authoring.SeedProbeSpecSaved.v1`
32. `Authoring.TasSpecSaved.v1`
33. `Authoring.BattleRunSpecSaved.v1`
34. `Authoring.PlanSaved.v1`
35. `Authoring.PredicateSpecSaved.v1`
36. `Authoring.SettingsSaved.v1`
37. `Authoring.TemplateSaved.v1`

## Archive events
38. `Archive.PackageCreated.v1`
39. `Archive.PackageIndexed.v1`
40. `Archive.RehydrateRequested.v1`
41. `Archive.RehydrateCompleted.v1`
42. `Archive.RehydrateFailed.v1`

---

## 3.3 SeedProbe Event Payload Requirements (v1)

### `AnalysisSeedProbe.NeutralSeedRecorded.v1`
- `probe_run_id`
- `probe_result_id`
- `neutral_seed_id`

### `AnalysisSeedProbe.GridSeedRecorded.v1`
- `probe_run_id`
- `probe_result_id`
- `grid_seed_id`
- `source_family`
- `axis_xy_id`
- `seed_value`
- `seed_delta`

### `AnalysisSeedProbe.UniqueSeedRecorded.v1`
- `probe_run_id`
- `probe_result_id`
- `unique_seed_id`
- `input_frame_id`
- `seed_value`
- `seed_delta`

### `AnalysisSeedProbe.RunCompleted.v1`
- `probe_run_id`
- `probe_result_id`
- `neutral_seed_value`
- `grid_count`
- `unique_count`
- `result_status`
- `completed_at_utc`

### `AnalysisSeedProbe.EncounterProjectionRecorded.v1`
- `probe_run_id`
- `encounter_projection_id`
- `seed_value`
- `option_ordinal`

---

## 3.4 Battle Event Payload Requirements (v1)

### `AnalysisBattle.SeedCandidateAdded.v1`
- `battle_set_id`
- `seed_candidate_id`
- `source_kind`
- `seed_value`

### `AnalysisBattle.TurnWaveCreated.v1`
- `battle_set_id`
- `wave_id`
- `turn_index`
- `parent_wave_id` (optional)
- `seed_candidate_id`

### `AnalysisBattle.TurnJobRecorded.v1`
- `wave_id`
- `turn_job_id`
- `exec_job_id` (optional)
- `job_state`
- result columns if `has_results=true`:
  - `fake_attacks_this_turn`
  - `fake_attacks_used_before`
  - `vi_start`, `vi_end`, `delta_vi`
  - `rng_seed`
  - `battle_outcome`
  - `plan_materialize_err`
  - `pred_passed`, `pred_total`, `pred_abort_run`

### `AnalysisBattle.SelectionDecisionRecorded.v1`
- `selection_pool_id`
- `turn_job_id`
- `decision_kind`
- `decision_reason` (optional)

### `AnalysisBattle.TerminalFollowupUpdated.v1`
- `turn_job_id`
- `is_victory`
- `manual_followup_status` (`UNREVIEWED`/`RECORDED`)
- `recorded_dtm_artifact_id` (required for `RECORDED`)

---

## 3.5 Projectors

## UIRead projectors

### `JobProjector`
Consumes:
- `Execution.*`
Outputs:
- `ui_job_summary`
- `ui_job_detail`
- `ui_job_artifact`

### `SeedProbeProjector`
Consumes:
- `AnalysisSeedProbe.*`
Outputs:
- `ui_seed_probe_summary`
- `ui_seed_probe_delta_point`
- `ui_seed_probe_unique_value`

### `BattleProjector`
Consumes:
- `AnalysisBattle.*`
- `Execution.*` (for state rollups)
Outputs:
- `ui_battle_group`
- `ui_battle_wave`
- `ui_battle_turn_job`
- `ui_battle_followup`

### `ArtifactProjector`
Consumes:
- `State.ArtifactStored.v1`
Outputs:
- `ui_artifact_browser`

### `ArchiveCatalogProjector`
Consumes:
- `Archive.*`
Outputs:
- `ui_archive_catalog`

## Projector Idempotency
- Maintain per-projector checkpoints in `ui_projection_checkpoint`.
- Use upsert semantics keyed by stable IDs.

---

## 3.6 Outbox Relay Behavior

1. Read unpublished outbox messages ordered by `outbox_id`.
2. Deserialize envelope and route by (`event_type`,`event_version`).
3. Resolve typed payload rows from (`payload_ref_kind`,`payload_ref_id`).
4. Dispatch to projector handlers.
5. On success, mark `published_at_utc`.
6. On failure, increment `attempt_count`, store `last_error`.
7. Dead-letter after configurable max attempts.

---

## 3.7 Test Plan

- Event serialization round-trip tests per event type.
- Version router tests (`v1` dispatch).
- Projector idempotency tests (same event twice).
- Relay failure/retry tests.
- SeedProbe projector tests for neutral/grid/unique counters and delta-point rendering feeds.
- Battle projector tests for wave hierarchy, per-turn metrics, and terminal follow-up status transitions.
