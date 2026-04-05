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
- `occurred_at_utc` is an integer Unix epoch timestamp in **milliseconds**; emitters and relays must preserve this value without seconds/milliseconds conversion.

Field usage expectations:
- `context_name` identifies the producing bounded context and supports projector routing/diagnostics.
- `correlation_id` links all events for a single workflow/request across contexts.
- `causation_id` links an emitted event to the immediate event that triggered it, preserving causal chains.

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
9. `Execution.WorkflowInstanceCreated.v1`
10. `Execution.WorkflowStepReady.v1`
11. `Execution.WorkflowStepMaterialized.v1`
12. `Execution.WorkflowStepCompleted.v1`
13. `Execution.WorkflowStepFailed.v1`
14. `Execution.WorkflowInstanceCompleted.v1`

## State events
15. `State.ArtifactStored.v1`
16. `State.SavestateCreated.v1`
17. `State.SavestateDerived.v1`
18. `State.TasVariantCreated.v1`

## Analysis spine events
19. `AnalysisSpine.RunCreated.v1`
20. `AnalysisSpine.StateRefRegistered.v1`
21. `AnalysisSpine.LineageEdgeAdded.v1`
22. `AnalysisSpine.ArtifactLinked.v1`

## Analysis.SeedProbe events
23. `AnalysisSeedProbe.SetCreated.v1`
24. `AnalysisSeedProbe.RunRequested.v1`
25. `AnalysisSeedProbe.NeutralSeedRecorded.v1`
26. `AnalysisSeedProbe.GridSeedRecorded.v1`
27. `AnalysisSeedProbe.UniqueSeedRecorded.v1`
28. `AnalysisSeedProbe.EncounterProjectionRecorded.v1`
29. `AnalysisSeedProbe.RunCompleted.v1`

## Analysis.Battle events
30. `AnalysisBattle.BattleSetCreated.v1`
31. `AnalysisBattle.SeedCandidateAdded.v1`
32. `AnalysisBattle.TurnWaveCreated.v1`
33. `AnalysisBattle.TurnJobRecorded.v1`
34. `AnalysisBattle.SelectionPoolCreated.v1`
35. `AnalysisBattle.SelectionDecisionRecorded.v1`
36. `AnalysisBattle.TerminalFollowupUpdated.v1`

## Authoring events
37. `Authoring.SeedProbeSpecSaved.v1`
38. `Authoring.TasSpecSaved.v1`
39. `Authoring.BattleRunSpecSaved.v1`
40. `Authoring.PlanSaved.v1`
41. `Authoring.PredicateSpecSaved.v1`
42. `Authoring.SettingsSaved.v1`
43. `Authoring.TemplateSaved.v1`

## Archive events
44. `Archive.PackageCreated.v1`
45. `Archive.PackageIndexed.v1`
46. `Archive.RehydrateRequested.v1`
47. `Archive.RehydrateCompleted.v1`
48. `Archive.RehydrateFailed.v1`

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
- `recorded_dtmini_artifact_id` (optional)
- `recorded_sav_artifact_id` (optional)

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

### `WorkflowProjector`
Consumes:
- `Execution.Workflow*`
Outputs:
- `ui_workflow_instance`
- `ui_workflow_step`
- `ui_workflow_edge`
- `ui_workflow_alert`

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
