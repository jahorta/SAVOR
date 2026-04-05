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
Deferred write points (schema complete; command services pending):
- `sp_probe_set`, `sp_probe_run`, `sp_probe_result`, `sp_neutral_seed`, `sp_grid_seed`, `sp_unique_seed`, `sp_encounter_projection` writes should append to `sp_outbox_message` in the same transaction.

### AnalysisBattle
Deferred write points (schema complete; command services pending):
- `ab_battle_set`, `ab_seed_candidate`, `ab_turn_wave`, `ab_turn_job`, `ab_selection_pool`, `ab_selection_decision`, `ab_terminal_followup` writes should append to `ab_outbox_message` in the same transaction.

### State
Deferred write points (schema complete; command services pending):
- `state_artifact`, `state_savestate`, `state_savestate_derivation`, `state_tas_movie_variant` writes should append to `state_outbox_message` in the same transaction.

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
| 15 | `State.ArtifactStored.v1` | Deferred | State command services are stage placeholder; outbox table exists but no writer yet. |
| 16 | `State.SavestateCreated.v1` | Deferred | Same as #15. |
| 17 | `State.SavestateDerived.v1` | Deferred | Same as #15. |
| 18 | `State.TasVariantCreated.v1` | Deferred | Same as #15. |
| 19 | `AnalysisSpine.RunCreated.v1` | Deferred | AnalysisSpine bounded-context command services pending. |
| 20 | `AnalysisSpine.StateRefRegistered.v1` | Deferred | Same as #19. |
| 21 | `AnalysisSpine.LineageEdgeAdded.v1` | Deferred | Same as #19. |
| 22 | `AnalysisSpine.ArtifactLinked.v1` | Deferred | Same as #19. |
| 23 | `AnalysisSeedProbe.SetCreated.v1` | Deferred | AnalysisSeedProbe command services pending; no transactional writer yet. |
| 24 | `AnalysisSeedProbe.RunRequested.v1` | Deferred | Same as #23. |
| 25 | `AnalysisSeedProbe.NeutralSeedRecorded.v1` | Deferred | Same as #23. |
| 26 | `AnalysisSeedProbe.GridSeedRecorded.v1` | Deferred | Same as #23. |
| 27 | `AnalysisSeedProbe.UniqueSeedRecorded.v1` | Deferred | Same as #23. |
| 28 | `AnalysisSeedProbe.EncounterProjectionRecorded.v1` | Deferred | Same as #23. |
| 29 | `AnalysisSeedProbe.RunCompleted.v1` | Deferred | Same as #23. |
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
