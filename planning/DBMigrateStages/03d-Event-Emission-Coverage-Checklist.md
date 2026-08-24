# Stage 3d - Event Emission Coverage Checklist (v1 Catalog)

## Scope
This checklist maps each catalog event to a concrete transactional emission path (implemented) or marks it **Deferred** with rationale.

Atomicity rule for all implemented emitters:
- Domain row mutation + source payload row insert + outbox insert run under one `BEGIN IMMEDIATE ... COMMIT` unit.
- Outbox envelope writes all required fields: `event_id`, `event_type`, `event_version`, `occurred_at_utc` (Unix ms), `context_name`, `aggregate_kind`, `aggregate_id`, `correlation_id`, `causation_id`, `payload_ref_kind`, `payload_ref_id`.

## Bounded-context transactional write points

### Execution
Implemented write points emitting outbox rows inside the same DB transaction:
- `AppendLifecycleEvent(JobSetCreated/JobQueued/JobClaimed/JobLeaseRenewed/JobProgressed/JobCompleted/JobEventArchived/JobRestored)` -> matching `Execution.Job*.v1`
- `RetryFailedStep` -> `Execution.WorkflowStepReady.v1`
- `SkipStep` -> `Execution.WorkflowStepCompleted.v1`
- `CancelWorkflowInstance` -> `Execution.WorkflowInstanceCompleted.v1`
- `ResumeWorkflowInstance` -> `Execution.WorkflowInstanceCreated.v1`
- `MarkStepMaterialized` -> `Execution.WorkflowStepMaterialized.v1`
- `MarkStepTerminal` -> `Execution.WorkflowStepCompleted.v1` / `Execution.WorkflowStepFailed.v1`
- `WorkflowRecoveryService::ReconcileInFlightInstances` -> `Execution.WorkflowStepCompleted.v1` / `Execution.WorkflowStepFailed.v1`

### AnalysisSeedProbe
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteAnalysisDb::CreateSeedProbeSet` -> `AnalysisSeedProbe.SetCreated.v1`
- `SqliteAnalysisDb::RequestSeedProbeRun` -> `AnalysisSeedProbe.RunRequested.v1`
- `SqliteAnalysisDb::RecordSeedProbeNeutralSeed` -> `AnalysisSeedProbe.NeutralSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeGridSeed` -> `AnalysisSeedProbe.GridSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeUniqueSeed` -> `AnalysisSeedProbe.UniqueSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeEncounterProjection` -> `AnalysisSeedProbe.EncounterProjectionRecorded.v1`
- `SqliteAnalysisDb::CompleteSeedProbeRun` -> `AnalysisSeedProbe.RunCompleted.v1`

### AnalysisBattle
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteAnalysisDb::CreateBattleSet` -> `AnalysisBattle.BattleSetCreated.v1`
- `SqliteAnalysisDb::AddBattleSeedCandidate` -> `AnalysisBattle.SeedCandidateAdded.v1`
- `SqliteAnalysisDb::CreateBattleTurnWave` -> `AnalysisBattle.TurnWaveCreated.v1`
- `SqliteAnalysisDb::RecordBattleTurnJob` -> `AnalysisBattle.TurnJobRecorded.v1`
- `SqliteAnalysisDb::CreateBattleSelectionPool` -> `AnalysisBattle.SelectionPoolCreated.v1`
- `SqliteAnalysisDb::RecordBattleSelectionDecision` -> `AnalysisBattle.SelectionDecisionRecorded.v1`
- `SqliteAnalysisDb::UpsertBattleTerminalFollowup` -> `AnalysisBattle.TerminalFollowupUpdated.v1`

### State
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteStateDb::StoreArtifact` -> `State.ArtifactStored.v1`
- `SqliteStateDb::CreateSavestate` -> `State.SavestateCreated.v1`
- `SqliteStateDb::DeriveSavestate` -> `State.SavestateDerived.v1`
- `SqliteStateDb::CreateTasVariant` -> `State.TasVariantCreated.v1`

### Authoring
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteAuthoringDb::SaveSeedProbeSpec` -> `Authoring.SeedProbeSpecSaved.v1`
- `SqliteAuthoringDb::SaveBattleRunSpec` -> `Authoring.BattleRunSpecSaved.v1`
- `SqliteAuthoringDb::SavePlan` -> `Authoring.PlanSaved.v1`
- `SqliteAuthoringDb::SavePredicateSpec` -> `Authoring.PredicateSpecSaved.v1`
- `SqliteAuthoringDb::SaveExplorerSettings` -> `Authoring.SettingsSaved.v1`
- `SqliteAuthoringDb::SaveTemplate` -> `Authoring.TemplateSaved.v1`

### Archive
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteArchiveDb::CreateArchivePackage` -> `Archive.PackageCreated.v1`
- `SqliteArchiveDb::AddArchiveItem` -> `Archive.PackageIndexed.v1`
- `SqliteArchiveDb::RequestRehydrate` -> `Archive.RehydrateRequested.v1`
- `SqliteArchiveDb::CompleteRehydrate` -> `Archive.RehydrateCompleted.v1`
- `SqliteArchiveDb::FailRehydrate` -> `Archive.RehydrateFailed.v1`

## Catalog-to-code coverage (48 events)

| # | Event | Status | Emission path / rationale |
|---:|---|---|---|
| 1 | `Execution.JobSetCreated.v1` | Implemented | `SqliteJobEventCommandService::AppendLifecycleEvent(JobSetCreated)` inserts outbox with `payload_ref_kind=job_set` in the same transaction. |
| 2 | `Execution.JobQueued.v1` | Implemented | `AppendLifecycleEvent(JobQueued)` updates job state and appends outbox atomically (`payload_ref_kind=job`). |
| 3 | `Execution.JobClaimed.v1` | Implemented | `AppendLifecycleEvent(JobClaimed)` updates claim/lease/start columns and appends outbox atomically (`payload_ref_kind=job`). |
| 4 | `Execution.JobLeaseRenewed.v1` | Implemented | `AppendLifecycleEvent(JobLeaseRenewed)` updates lease and appends outbox atomically (`payload_ref_kind=job`). |
| 5 | `Execution.JobProgressed.v1` | Implemented | `AppendLifecycleEvent(JobProgressed)` updates job row and appends outbox atomically (`payload_ref_kind=job`). |
| 6 | `Execution.JobCompleted.v1` | Implemented | `AppendLifecycleEvent(JobCompleted)` marks terminal job state and appends outbox atomically (`payload_ref_kind=job`). |
| 7 | `Execution.JobEventArchived.v1` | Implemented | `AppendLifecycleEvent(JobEventArchived)` appends outbox row for archive bridge handoff (`payload_ref_kind=job`). |
| 8 | `Execution.JobRestored.v1` | Implemented | `AppendLifecycleEvent(JobRestored)` appends outbox row for rehydrate restore handoff (`payload_ref_kind=job`). |
| 9 | `Execution.WorkflowInstanceCreated.v1` | Implemented | `SqliteWorkflowOrchestrationCommandService::ResumeWorkflowInstance` -> `EmitLifecycleEvent`. |
| 10 | `Execution.WorkflowStepReady.v1` | Implemented | `SqliteWorkflowOrchestrationCommandService::RetryFailedStep` -> `EmitLifecycleEvent`. |
| 11 | `Execution.WorkflowStepMaterialized.v1` | Implemented | `SqliteWorkflowOrchestrationCommandService::MarkStepMaterialized` -> `EmitLifecycleEvent`. |
| 12 | `Execution.WorkflowStepCompleted.v1` | Implemented | `SkipStep`/`MarkStepTerminal` and recovery reconciliation emit via workflow event + outbox insert. |
| 13 | `Execution.WorkflowStepFailed.v1` | Implemented | `MarkStepTerminal` and recovery reconciliation emit via workflow event + outbox insert. |
| 14 | `Execution.WorkflowInstanceCompleted.v1` | Implemented | `SqliteWorkflowOrchestrationCommandService::CancelWorkflowInstance` -> `EmitLifecycleEvent`. |
| 15 | `State.ArtifactStored.v1` | Implemented | `SqliteStateDb::StoreArtifact` inserts `state_artifact` + outbox row atomically (`payload_ref_kind=artifact`). |
| 16 | `State.SavestateCreated.v1` | Implemented | `SqliteStateDb::CreateSavestate` inserts `state_savestate` + outbox row atomically (`payload_ref_kind=savestate`). |
| 17 | `State.SavestateDerived.v1` | Implemented | `SqliteStateDb::DeriveSavestate` inserts `state_savestate_derivation` + outbox row atomically (`payload_ref_kind=savestate_derivation`). |
| 18 | `State.TasVariantCreated.v1` | Implemented | `SqliteStateDb::CreateTasVariant` inserts `state_tas_movie_variant` + outbox row atomically (`payload_ref_kind=tas_variant`). |
| 19 | `AnalysisSpine.RunCreated.v1` | Implemented | `SqliteAnalysisDb::ResolveSpinePayload` + `SqliteAnalysisSpinePayloadRowResolver::ResolveSpineRunCreated` resolve v1 payloads from `asp_run`. |
| 20 | `AnalysisSpine.StateRefRegistered.v1` | Implemented | `ResolveSpinePayload` + `ResolveSpineStateRefRegistered` resolve v1 payloads from `asp_state_ref`. |
| 21 | `AnalysisSpine.LineageEdgeAdded.v1` | Implemented | `ResolveSpinePayload` + `ResolveSpineLineageEdgeAdded` resolve v1 payloads from `asp_lineage_edge`. |
| 22 | `AnalysisSpine.ArtifactLinked.v1` | Implemented | `ResolveSpinePayload` + `ResolveSpineArtifactLinked` resolve v1 payloads from `asp_artifact_ref`. |
| 23 | `AnalysisSeedProbe.SetCreated.v1` | Implemented | `SqliteAnalysisDb::CreateSeedProbeSet` inserts `sp_probe_set` + outbox row atomically (`payload_ref_kind=probe_set`). |
| 24 | `AnalysisSeedProbe.RunRequested.v1` | Implemented | `SqliteAnalysisDb::RequestSeedProbeRun` inserts `sp_probe_run` + outbox row atomically (`payload_ref_kind=probe_run`). |
| 25 | `AnalysisSeedProbe.NeutralSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeNeutralSeed` inserts `sp_neutral_seed` + outbox row atomically (`payload_ref_kind=neutral_seed`). |
| 26 | `AnalysisSeedProbe.GridSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeGridSeed` inserts `sp_grid_seed` + outbox row atomically (`payload_ref_kind=grid_seed`). |
| 27 | `AnalysisSeedProbe.UniqueSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeUniqueSeed` inserts `sp_unique_seed` + outbox row atomically (`payload_ref_kind=unique_seed`). |
| 28 | `AnalysisSeedProbe.EncounterProjectionRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeEncounterProjection` inserts `sp_encounter_projection` + outbox row atomically (`payload_ref_kind=encounter_projection`). |
| 29 | `AnalysisSeedProbe.RunCompleted.v1` | Implemented | `SqliteAnalysisDb::CompleteSeedProbeRun` inserts `sp_probe_result`, finalizes run status, and appends outbox atomically (`payload_ref_kind=probe_result`). |
| 30 | `AnalysisBattle.BattleSetCreated.v1` | Implemented | `SqliteAnalysisDb::CreateBattleSet` inserts `ab_battle_set` + outbox row atomically (`payload_ref_kind=battle_set`). |
| 31 | `AnalysisBattle.SeedCandidateAdded.v1` | Implemented | `SqliteAnalysisDb::AddBattleSeedCandidate` inserts `ab_seed_candidate` + outbox row atomically (`payload_ref_kind=seed_candidate`). |
| 32 | `AnalysisBattle.TurnWaveCreated.v1` | Implemented | `SqliteAnalysisDb::CreateBattleTurnWave` inserts `ab_turn_wave` + outbox row atomically (`payload_ref_kind=turn_wave`). |
| 33 | `AnalysisBattle.TurnJobRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordBattleTurnJob` inserts `ab_turn_job` + outbox row atomically (`payload_ref_kind=turn_job`). |
| 34 | `AnalysisBattle.SelectionPoolCreated.v1` | Implemented | `SqliteAnalysisDb::CreateBattleSelectionPool` inserts `ab_selection_pool` + outbox row atomically (`payload_ref_kind=selection_pool`). |
| 35 | `AnalysisBattle.SelectionDecisionRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordBattleSelectionDecision` inserts `ab_selection_decision` + outbox row atomically (`payload_ref_kind=selection_decision`). |
| 36 | `AnalysisBattle.TerminalFollowupUpdated.v1` | Implemented | `SqliteAnalysisDb::UpsertBattleTerminalFollowup` upserts `ab_terminal_followup` + outbox row atomically (`payload_ref_kind=terminal_followup`). |
| 37 | `Authoring.SeedProbeSpecSaved.v1` | Implemented | `SqliteAuthoringDb::SaveSeedProbeSpec` inserts `au_seed_probe_spec` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 39 | `Authoring.BattleRunSpecSaved.v1` | Implemented | `SqliteAuthoringDb::SaveBattleRunSpec` inserts `au_battle_run_spec` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 40 | `Authoring.PlanSaved.v1` | Implemented | `SqliteAuthoringDb::SavePlan` inserts `au_battle_plan` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 41 | `Authoring.PredicateSpecSaved.v1` | Implemented | `SqliteAuthoringDb::SavePredicateSpec` inserts `au_predicate_spec` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 42 | `Authoring.SettingsSaved.v1` | Implemented | `SqliteAuthoringDb::SaveExplorerSettings` inserts `au_explorer_settings` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 43 | `Authoring.TemplateSaved.v1` | Implemented | `SqliteAuthoringDb::SaveTemplate` inserts `au_template` + outbox row atomically (`payload_ref_kind=authoring_event`). |
| 44 | `Archive.PackageCreated.v1` | Implemented | `SqliteArchiveDb::CreateArchivePackage` inserts `ar_archive_package` + outbox row atomically (`payload_ref_kind=archive_package`). |
| 45 | `Archive.PackageIndexed.v1` | Implemented | `SqliteArchiveDb::AddArchiveItem` inserts `ar_archive_item` + outbox row atomically (`payload_ref_kind=archive_item`). |
| 46 | `Archive.RehydrateRequested.v1` | Implemented | `SqliteArchiveDb::RequestRehydrate` inserts `ar_rehydrate_request` + outbox row atomically (`payload_ref_kind=rehydrate_request`). |
| 47 | `Archive.RehydrateCompleted.v1` | Implemented | `SqliteArchiveDb::CompleteRehydrate` updates `ar_rehydrate_request`, inserts `ar_rehydrate_map` rows, and appends outbox atomically (`payload_ref_kind=rehydrate_request`). |
| 48 | `Archive.RehydrateFailed.v1` | Implemented | `SqliteArchiveDb::FailRehydrate` updates `ar_rehydrate_request` with failure metadata + outbox row atomically (`payload_ref_kind=rehydrate_request`). |

## Next implementation increments
1. Promote this checklist to “all implemented” gate in CI now that all catalog events have transactional emitters.
2. Add cross-context integration tests that replay all outbox streams through UIRead projector checkpoints in one pass.
