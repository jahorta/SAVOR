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
1. Run list and run detail summaries.
2. Seed probe result views:
   - neutral seed
   - grid seed values list
   - unique seed values list
   - counts per class
3. Battle turn tree and per-node outcomes.
4. Archive package catalog and restore request status.

---

## 5.2 SoaSimQt2 Migration Tasks

1. Replace direct data queries with read-model gateway calls.
2. Replace direct writes with command handlers invoking context services.
3. Add feature flags:
   - `UseNewUIRead`
   - `UseNewExecutionWrites`
   - `UseArchivePipeline`
4. Instrument screen-level query times.

---

## 5.3 Backfill Plan

### Backfill scope
- Existing seed probe records -> Analysis.SeedProbe tables.
- Existing battle result records -> Analysis.Battle tables.
- Existing run metadata -> Analysis Spine.
- Existing execution events -> optional archive package bootstrap.

### Backfill order
1. State objects and savestates.
2. Analysis spine runs and lineage.
3. SeedProbe durable records.
4. Battle records.
5. UIRead rebuild.

---

## 5.4 Validation Gates

## Functional gates
- SeedProbe screen parity with expected neutral/grid/unique values.
- Battle tree parity with expected parent/child links.
- Archive list and restore flow works end-to-end.

## Data integrity gates
- No orphaned typed references after backfill.
- Event replay into empty UIRead produces same counts as live read DB.

## Performance gates
- Run list query under target latency.
- Seed probe detail query under target latency for large runs.
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
