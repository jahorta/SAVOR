# Stage 5 - UI Cutover, Backfill, and Validation

## Objective
Move `SavorQt` to the new architecture safely with measurable correctness and performance gates.

## Exit Criteria
- SavorQt reads are served from UIRead models.
- Command paths write to new context DBs.
- Critical screens validated against expected data.
- Rollback and fallback documented.

---

## Current Cutover Slice Order

The first UI cutover pass is intentionally read-focused and follows this order:

1. Seed probe views.
   - Current status: `SeedProbeController` already reads directly from `IUiReadDb` for run list, summary, delta points, and unique values.
   - Remaining work: backfill/projection validation, parity checks against expected seed probe data, and removal of any dead legacy seed-probe list/detail fallbacks.
2. Jobs detail view.
   - Current status: job list and artifact references are backed by UIRead; job detail summary fields are projected through `ui_job_summary` and `ui_job_detail`.
   - Remaining work: expose an explicit UIRead detail contract, wire the detail pane to those projected fields, remove legacy `vm_kv`/raw-input parsing from Qt2 detail paths, and project any non-execution/non-workflow detail from the owning durable context instead.
3. Job sets detail views.
   - Current status: job-set summaries are derived from `ui_job_summary` aggregation. There is not yet a dedicated job-set read model for parent, purpose, expected total, or full family tree semantics.
   - Remaining work: expose a current-capability detail contract, stop treating fabricated parent/purpose fields as authoritative, then add a typed job-set projection when hierarchy and expected-total parity become required.

This order supersedes any older implication that all Stage 5 UI surfaces must cut over at once.

## Current Implementation Notes

- `DBService` now starts an attached-source UIRead projection service that opens the UIRead database, attaches the source context databases, and replays source outbox streams through the existing projectors.
- Qt2 should treat UIRead as read-only. UIRead writes remain internal projector operations; Qt2 command paths should write to their owning context services and rely on outbox projection to refresh read models.
- State artifact creation now updates UIRead through the State outbox and artifact projector. The artifact browser cutover is therefore a command-to-State plus projection-to-UIRead flow, not a synchronous UIRead update from the Qt2 artifact service.
- The first attached-source projection coverage is validated by focused SQLite fixture tests for artifact summary projection across separate State and UIRead database files.
- Qt2 and UIRead projections should not derive domain detail by parsing raw legacy `vm_kv`, raw `input_ini`, results INI, or job-event payload text. Execution-owned fields can come from Execution/UI workflow tables; authored/analysis/state details must come from Authoring, Analysis, and State respectively.
- Qt2 authoring uses modeless top-level editor windows for reusable authoring records. The main panes are browsers/libraries; `New`, `Edit`, and `Duplicate` actions open independent editor windows for predicates, battle plans, and workflow graphs. These windows must not block the rest of Qt2, and dirty-close handling should prompt before discarding unsaved edits.
- Qt2 Workflow Builder is an authoring surface only. It saves reusable workflow graphs to Authoring as logical graphs with immutable revisions containing required input port definitions, possible output port definitions, edges, guards, and optional refs to other authored records. Workflow submission/instancing belongs to a separate launch pane where external inputs are selected.
- Qt2 must not store external input values in authored workflow graphs. Source artifacts, savestates, prior analysis output refs, and other run-specific bindings are instance-specific submission data.
- Qt2 must not launch workflows from authoring editors or the workflow builder. A separate Workflow Launcher owns instance creation, external input selection, and scalar instance arguments.
- Scalar launch choices that vary per run belong to Execution workflow instance arguments. Current examples are TAS RTC value and battle fake-attack min/max overrides.
- TAS RTC ranges are launcher input, not authored TAS spec state. Launching a range creates one workflow instance per RTC value, each with one concrete RTC argument.
- Qt2 may bind authored plans, predicates, and specs in the authored graph, but it must not pre-create Analysis DB rows for downstream steps.
- Workflow composition contracts expose required inputs and possible outputs. Possible outputs are only compatibility hints until a running step produces an actual typed reference.
- Program descriptors/adapters own lazy Analysis DB row creation during step materialization/result mapping. Downstream dependent steps should be readied only when their required actual refs exist; otherwise they remain blocked, skipped, or failed according to the transition policy.
- Remaining operational gap: add a Qt2-visible projection health/status surface for subscription lag, last error, and recovery guidance.

---

## 5.1 UIRead Coverage Requirements

### Must-have projections before cutover
1. Job list and detail summaries.
   - `ui_job_summary`
   - `ui_job_detail`
   - `ui_job_artifact`
2. Seed probe views:
   - `ui_seed_probe_summary`
   - `ui_seed_probe_delta_point` (family + x/y/span + delta)
   - `ui_seed_probe_unique_value`
3. Battle exploration views:
   - `ui_battle_group`
   - `ui_battle_wave`
   - `ui_battle_turn_job`
   - `ui_battle_followup`
4. Artifact browser:
   - `ui_artifact_browser`
5. Archive package and rehydrate status:
   - `ui_archive_catalog`
6. Workflow orchestration visibility:
   - `ui_workflow_instance`
   - `ui_workflow_step`
   - `ui_workflow_edge`
   - `ui_workflow_alert`
7. Projector subscription progress:
   - `ui_projection_subscription` (authoritative after Stage 3e)
   - `ui_projection_checkpoint` (compatibility during transition only)

---

## 5.2 SavorQt Migration Tasks

1. Replace direct data queries with read-model gateway calls.
2. Replace direct writes with command handlers invoking context services.
3. Replace raw `vm_kv`, `input_ini`, job-event payload, and INI-derived output reads with typed read-model fields for seed probe/battle result screens.
   - Execution/UI workflow projections should expose only operational details such as job state, attempts, timing, leases, workflow step state, and errors.
   - Authoring, Analysis, and State remain the source of truth for authored specs, predicates, plans, analysis results, artifacts, savestates, and lineage.
   - Raw execution input may be exposed only as an explicitly labeled diagnostic view, not as the normal domain detail model.
4. Add feature flags:
   - `UseNewUIRead`
   - `UseNewExecutionWrites`
   - `UseArchivePipeline`
5. Instrument screen-level query times.
6. Add projector-subscription observability surfaces for ops:
   - subscription status (`ACTIVE`/`PAUSED`/`ERROR`)
   - last processed outbox cursor/event id per projector and source stream
   - last error and recovery action guidance
7. Move authoring editors out of inline panes and blocking dialogs:
   - Predicate specs, battle plans, and workflow graphs open in modeless top-level editor windows.
   - Parent panes track open windows and focus an existing editor for the same draft/record when possible.
   - Save operations write to Authoring context services and refresh the parent browser after success.
   - Workflow graph editors never collect external input values or launch workflow instances.
8. Keep workflow launch separate from authoring:
   - Add/complete a Workflow Launcher pane that selects an authored graph revision.
   - Collect required external input bindings from State/Analysis source records.
   - Collect scalar instance arguments such as TAS RTC and battle fake-attack bounds.
   - Fan out TAS RTC ranges into one workflow instance per RTC value.
   - Submit instances to Execution DB without mutating the authored graph revision.
9. Demote job sets to workflow drill-down:
   - The primary operations surface should group work by workflow instance, then step, then step job set.
   - Job sets remain available as a detail tab for execution diagnostics, retries, and per-job inspection.

---

## 5.2.1 Legacy Cleanup Direction

The current target architecture intentionally replaces several early DBMigrate structures. Because this branch is still early in production, compatibility with those legacy structures is not required unless a later migration note says otherwise.

- Static workflow definitions, the static workflow engine/builder/validator path, `exec_workflow_input_event`, and instance-level `input_ref_kind` / `input_ref_id` bootstrap paths have been removed from active workflow launch.
- New workflow launches should use Authoring-owned graph templates, immutable revisions, Execution-owned `exec_workflow_instance_input_binding` rows, and scalar `exec_workflow_instance_argument` rows.
- Step-level `input_ref_kind` / `input_ref_id` remains valid for actual runtime refs that are produced or consumed by descriptors and dynamic downstream steps.
- Retire remaining `exec_trigger` behavior where it appears; graph instancing and source outbox projection should cover new behavior.
- Move launch-time values out of authored specs where they are not reusable authoring intent:
  - TAS selected DTM artifact -> instance input binding.
  - TAS RTC value/range -> launcher input and per-instance argument.
  - Battle fake-attack min/max exploration bounds -> instance arguments.
- Remove seedprobe auto-schedule and other cross-workflow launch behavior from authored specs. Downstream flow comes from graph edges and descriptor-produced actual refs.
- Remove UI derivation from raw legacy `vm_kv`, raw `input_ini`, and result INI parsing except explicit diagnostics. Typed details should come from Authoring, Analysis, State, Execution, or UIRead projections.

---

## 5.3 Backfill Plan

### Backfill scope
- Existing artifact/savestate records -> State DB.
- Existing run metadata -> Analysis Spine (`asp_*`).
- Existing seed probe records -> Analysis.SeedProbe (`sp_*`) including axis/input-frame normalization.
- Existing battle result records -> Analysis.Battle (`ab_*`) including terminal follow-up defaults (`UNREVIEWED`).
- Existing execution events -> optional archive package bootstrap.

### Backfill order
1. State artifacts and savestates.
2. Analysis spine runs and lineage.
3. SeedProbe durable records (`sp_probe_*`, `sp_axis_xy`, `sp_input_frame`, seeds).
4. Battle records (`ab_battle_set`, candidates, waves, turn jobs, selection, follow-up).
5. UIRead rebuild.

---

## 5.4 Validation Gates

## Functional gates
- SeedProbe screen parity:
  - neutral seed, status, codec, savestate label
  - delta maps by family
  - unique seeds list
- Battle explorer parity:
  - root groups, wave-by-turn hierarchy
  - per-turn metrics (`rng_seed`, `delta_vi`, predicate counters, outcome)
  - manual follow-up status (`UNREVIEWED`/`RECORDED`) and recorded DTM linkage
- Artifact browser parity.
- Archive list and restore flow works end-to-end.

## Data integrity gates
- No orphaned typed references after backfill.
- `RECORDED` follow-up rows always have a DTM artifact reference.
- Event replay into empty UIRead reproduces same counts as live UIRead.
- Reset/replay of one projector subscription does not regress any other projector subscription cursor.
- Multi-projector same-stream replay produces consistent read-model counts with no starvation.

## Performance gates
- Job list query under target latency.
- Seed probe detail query under target latency for large runs.
- Battle wave/job query under target latency for large trees.
- Projector lag remains under threshold.

---

## 5.5 Rollout Strategy

1. Internal alpha: new DBs + read models enabled for dev workflows.
2. Shadow mode: dual projection verification, old UI still primary.
3. Controlled cutover: enable `UseNewUIRead` by default and make subscription cursors the authoritative projector progress source.
4. Full cutover: enable new write paths.
5. Decommission old read paths, legacy shared-checkpoint-only paths, and dead code.

---

## 5.6 Fallback Plan

- Keep legacy `SAVORQt` runnable until Stage 5 signoff.
- Keep DB snapshots before irreversible migration steps.
- Support toggling off new paths via feature flags.
- If needed, reset one or more projector subscriptions and re-run UIRead projection replay from source outbox history without impacting unrelated projector subscriptions.

---

## 5.7 Signoff Checklist

- [ ] All stage documents completed and linked to tracked work items.
- [ ] Schema, event, and projector docs updated.
- [ ] Smoke tests pass on clean environment.
- [ ] Performance gates met.
- [ ] Rollback runbook reviewed.
