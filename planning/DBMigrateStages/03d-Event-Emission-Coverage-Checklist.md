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
- `SqliteAnalysisDb::RequestSeedProbeRun` -> `AnalysisSeedProbe.RunRequested.v1`
- `SqliteAnalysisDb::RecordSeedProbeNeutralSeed` -> `AnalysisSeedProbe.NeutralSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeGridSeed` -> `AnalysisSeedProbe.GridSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeUniqueSeed` -> `AnalysisSeedProbe.UniqueSeedRecorded.v1`
- `SqliteAnalysisDb::RecordSeedProbeEncounterProjection` -> `AnalysisSeedProbe.EncounterProjectionRecorded.v1`
- `SqliteAnalysisDb::CompleteSeedProbeRun` -> `AnalysisSeedProbe.RunCompleted.v1`

### AnalysisBattle
Deferred write points (schema complete; command services pending):
- `ab_battle_set`, `ab_seed_candidate`, `ab_turn_wave`, `ab_turn_job`, `ab_selection_pool`, `ab_selection_decision`, `ab_terminal_followup` writes should append to `ab_outbox_message` in the same transaction.

### State
Implemented write points emitting outbox rows inside the same DB transaction:
- `SqliteStateDb::StoreArtifact` -> `State.ArtifactStored.v1`
- `SqliteStateDb::CreateSavestate` -> `State.SavestateCreated.v1`
- `SqliteStateDb::DeriveSavestate` -> `State.SavestateDerived.v1`
- `SqliteStateDb::CreateTasVariant` -> `State.TasVariantCreated.v1`

### Authoring
Deferred write points (schema complete; command services pending):
- `au_*_spec`, `au_plan`, `au_predicate_spec`, `au_settings`, `au_template` writes should append to `au_outbox_message` in the same transaction.

### Archive
Deferred write points (schema complete; command services pending):
- `ar_archive_package`, `ar_archive_item`, `ar_rehydrate_request`, `ar_rehydrate_map` writes should append to `ar_outbox_message` in the same transaction.

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
| 23 | `AnalysisSeedProbe.SetCreated.v1` | Deferred | AnalysisSeedProbe command services pending; no transactional writer yet. |
| 24 | `AnalysisSeedProbe.RunRequested.v1` | Implemented | `SqliteAnalysisDb::RequestSeedProbeRun` inserts `sp_probe_run` + outbox row atomically (`payload_ref_kind=probe_run`). |
| 25 | `AnalysisSeedProbe.NeutralSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeNeutralSeed` inserts `sp_neutral_seed` + outbox row atomically (`payload_ref_kind=neutral_seed`). |
| 26 | `AnalysisSeedProbe.GridSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeGridSeed` inserts `sp_grid_seed` + outbox row atomically (`payload_ref_kind=grid_seed`). |
| 27 | `AnalysisSeedProbe.UniqueSeedRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeUniqueSeed` inserts `sp_unique_seed` + outbox row atomically (`payload_ref_kind=unique_seed`). |
| 28 | `AnalysisSeedProbe.EncounterProjectionRecorded.v1` | Implemented | `SqliteAnalysisDb::RecordSeedProbeEncounterProjection` inserts `sp_encounter_projection` + outbox row atomically (`payload_ref_kind=encounter_projection`). |
| 29 | `AnalysisSeedProbe.RunCompleted.v1` | Implemented | `SqliteAnalysisDb::CompleteSeedProbeRun` inserts `sp_probe_result`, finalizes run status, and appends outbox atomically (`payload_ref_kind=probe_result`). |
| 30 | `AnalysisBattle.BattleSetCreated.v1` | Deferred | AnalysisBattle command services pending; no transactional writer yet. |
| 31 | `AnalysisBattle.SeedCandidateAdded.v1` | Deferred | Same as #30. |
| 32 | `AnalysisBattle.TurnWaveCreated.v1` | Deferred | Same as #30. |
| 33 | `AnalysisBattle.TurnJobRecorded.v1` | Deferred | Same as #30. |
| 34 | `AnalysisBattle.SelectionPoolCreated.v1` | Deferred | Same as #30. |
| 35 | `AnalysisBattle.SelectionDecisionRecorded.v1` | Deferred | Same as #30. |
| 36 | `AnalysisBattle.TerminalFollowupUpdated.v1` | Deferred | Same as #30. |
| 37 | `Authoring.SeedProbeSpecSaved.v1` | Deferred | Authoring command services are stage placeholder; outbox writer pending. |
| 38 | `Authoring.TasSpecSaved.v1` | Deferred | Same as #37. |
| 39 | `Authoring.BattleRunSpecSaved.v1` | Deferred | Same as #37. |
| 40 | `Authoring.PlanSaved.v1` | Deferred | Same as #37. |
| 41 | `Authoring.PredicateSpecSaved.v1` | Deferred | Same as #37. |
| 42 | `Authoring.SettingsSaved.v1` | Deferred | Same as #37. |
| 43 | `Authoring.TemplateSaved.v1` | Deferred | Same as #37. |
| 44 | `Archive.PackageCreated.v1` | Deferred | Archive command services are stage placeholder; outbox writer pending. |
| 45 | `Archive.PackageIndexed.v1` | Deferred | Same as #44. |
| 46 | `Archive.RehydrateRequested.v1` | Deferred | Same as #44. |
| 47 | `Archive.RehydrateCompleted.v1` | Deferred | Same as #44. |
| 48 | `Archive.RehydrateFailed.v1` | Deferred | Same as #44. |

## Next implementation increments
1. Add transactional command services per non-Execution context and emit one event per domain write point in the same transaction.
2. Promote this checklist to “all implemented” gate in CI once remaining bounded-context command services exist.
