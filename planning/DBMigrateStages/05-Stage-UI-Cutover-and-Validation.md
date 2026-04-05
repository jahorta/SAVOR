# Stage 5 - UI Cutover, Backfill, and Validation

## Objective
Move `SoaSimQt2` to the new architecture safely with measurable correctness and performance gates.

## Exit Criteria
- SoaSimQt2 reads are served from UIRead models.
- Command paths write to new context DBs.
- Critical screens validated against expected data.
- Rollback and fallback documented.

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
7. Projector checkpoints:
   - `ui_projection_checkpoint`

---

## 5.2 SoaSimQt2 Migration Tasks

1. Replace direct data queries with read-model gateway calls.
2. Replace direct writes with command handlers invoking context services.
3. Replace INI-derived output reads with typed read-model fields for seed probe/battle result screens.
4. Add feature flags:
   - `UseNewUIRead`
   - `UseNewExecutionWrites`
   - `UseArchivePipeline`
5. Instrument screen-level query times.

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

## Performance gates
- Job list query under target latency.
- Seed probe detail query under target latency for large runs.
- Battle wave/job query under target latency for large trees.
- Projector lag remains under threshold.

---

## 5.5 Rollout Strategy

1. Internal alpha: new DBs + read models enabled for dev workflows.
2. Shadow mode: dual projection verification, old UI still primary.
3. Controlled cutover: enable `UseNewUIRead` by default.
4. Full cutover: enable new write paths.
5. Decommission old read paths and dead code.

---

## 5.6 Fallback Plan

- Keep legacy `SoaSimQt` runnable until Stage 5 signoff.
- Keep DB snapshots before irreversible migration steps.
- Support toggling off new paths via feature flags.
- If needed, re-run UIRead full rebuild from outbox history.

---

## 5.7 Signoff Checklist

- [ ] All stage documents completed and linked to tracked work items.
- [ ] Schema, event, and projector docs updated.
- [ ] Smoke tests pass on clean environment.
- [ ] Performance gates met.
- [ ] Rollback runbook reviewed.
