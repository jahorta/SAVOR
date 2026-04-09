# 11 - Phase 4 Runbook and On-Call Checklist

## Purpose

This runbook defines incident handling and recovery execution procedures for Phase 4 hardening.

It is the operational companion to:

- `10-phase-4-hardening-and-recovery.md` (scope, requirements, and suggested tests)
- `SimCoreDBValidation` phase-4 checks (gating and replay/recovery drills)

## Incident procedures: delayed, duplicate, and out-of-order events

Use this section when alerts indicate event-delivery anomalies (lag spikes, duplicate terminal signals, ordering jitter).

### A. Delayed events

**Detection signals**

- Relay/subscription lag exceeds SLO threshold.
- Terminal transition latency histogram shows sustained tail growth.
- Queue depth for pending materialized claimed jobs grows while worker availability is normal.

**Procedure**

1. Confirm scope
   - Identify affected stream (`exec_workflow_input_event` vs `exec_workflow_event`) and tenant/workflow cohort.
   - Confirm whether delay is producer-side (outbox enqueue) or consumer-side (subscription drain).
2. Stabilize
   - Freeze non-critical backfills/replays that compete for the same consumer resources.
   - If needed, throttle high-frequency progress publishers to protect terminal-path throughput.
3. Recover
   - Restart the affected subscriber/coordinator instance with replay cursor preserved.
   - Validate no terminal-authority bypass occurred (terminal actions must remain control-plane authoritative).
4. Verify
   - Run `phase4.power_loss_during_claimed_job_materialization` when restart path was involved.
   - Run `phase4.missing_decision_result_restart_rerun` if decision-result gaps were observed.
5. Close
   - Document lag window, root trigger, and recovery duration.

### B. Duplicate events

**Detection signals**

- Duplicate key/nonce observations in dedupe tables.
- Repeated terminal event ingestion attempts for same workflow step transition.

**Procedure**

1. Confirm duplicate source
   - Distinguish retried publish from replay-driven re-consume.
   - Validate envelope idempotency keys and dedupe-table retention coverage.
2. Contain impact
   - Keep consumers running unless duplicate flood causes instability; avoid premature global pause.
   - Disable ad-hoc replay tooling that may be amplifying duplicates.
3. Recover
   - Validate duplicate suppression at terminal transition boundary.
   - If duplicate terminal side-effects are suspected, trigger invariant-violation flow (see next section).
4. Verify
   - Run `phase4.duplicate_terminal_replay` and require pass before incident closure.
5. Close
   - Record whether dedupe policy/TTL adjustment is required.

### C. Out-of-order events

**Detection signals**

- Step transition event arrives before prerequisite input-complete/decision-result markers.
- Completion-gate mismatch counters increase.

**Procedure**

1. Confirm ordering break
   - Identify first violated prerequisite edge (input -> decision -> terminal).
   - Correlate with relay retry/restart windows.
2. Contain
   - Pause only the affected workflow instances/cohort when possible.
   - Keep unrelated workflow cohorts active.
3. Recover
   - Re-run decider for impacted steps to reconstitute authoritative next action.
   - Apply remediation sequence if invariant checks fail.
4. Verify
   - Run `phase4.invariant_violation_remediation_sequence` if any invariant failed.
   - Run `phase4.missing_decision_result_restart_rerun` when ordering failure involved decision-result absence.
5. Close
   - Capture ordering-break cause and prevention action.

## Invariant violation remediation procedure

Use when invariant checks fail (completion mismatch, illegal terminal sequence, or writer/state divergence).

### Procedure: detect -> pause -> repair -> reopen/fail

1. **Detect**
   - Alert or checker identifies invariant breach and records violation code.
   - Emit invariant-violation event with workflow instance, step key, reason code, and detection timestamp.
2. **Pause**
   - Transition affected workflow instance to paused/guarded state.
   - Block further step progression and terminal publication for the impacted step until remediation completes.
3. **Repair**
   - Execute authoritative reconciliation job:
     - Rebuild canonical step status from source-of-truth events/state tables.
     - Reconcile mapper/writer side-effects.
     - Re-emit missing authoritative events if required by contract.
4. **Reopen or fail**
   - **Reopen path:** invariants restored and prerequisite evidence complete; reopen step and resume queueing.
   - **Fail path:** invariants cannot be safely restored or side-effects are irreversible; terminal-fail with explicit failure code.
5. **Audit**
   - Persist remediation transcript (detected-by, paused-at, repaired-by, reopened/failed decision, and operator identity).

## Decider replay/restart playbook and rollback guidance

## Preconditions

- Incident commander identified affected workflow cohort and time window.
- Replay cursor/checkpoint position captured before changes.
- Backfill and manual tooling locks acquired to avoid competing writes.

## Replay/restart execution

1. Snapshot
   - Capture current coordinator/decider state, replay cursor, and queue depth metrics.
2. Drain (if required)
   - For severe inconsistency, pause targeted workflows and drain in-flight worker claims.
3. Restart
   - Restart decider/coordinator services with configured replay from captured cursor.
4. Re-evaluate
   - Force decider evaluation for workflows missing decision-result or with stale pending states.
5. Validate
   - Run `phase4.missing_decision_result_restart_rerun` for missing-result scenarios.
   - Run `phase4.power_loss_during_claimed_job_materialization` when restart follows interruption during materialization.

## Rollback guidance

Use rollback only if replay/restart worsens inconsistency or introduces new invariant violations.

1. Stop forward replay.
2. Restore last known-good cursor/checkpoint and service config.
3. Reapply stable build/revision.
4. Reopen traffic gradually (canary cohort first).
5. Re-run targeted phase-4 validations before full reopen.

## Recovery matrix execution checklist (mapped to `phase4.*` validations)

Run from `SimCoreDBValidation`:

`SimCoreDBValidation --run <validation-name>`

- [ ] `phase4.invariant_violation_remediation_sequence`
  - Scope: pause -> invariant event -> repair job -> reopen/fail ordering is preserved.
  - Evidence: remediation event log, pause marker, repair completion marker.
- [ ] `phase4.power_loss_during_claimed_job_materialization`
  - Scope: restart semantics after interruption during claimed-job materialization.
  - Evidence: no duplicate side-effects; final state matches canonical replay.
- [ ] `phase4.duplicate_terminal_replay`
  - Scope: duplicate terminal replays are deduped and applied exactly once.
  - Evidence: single terminal transition with idempotent replay logs.
- [ ] `phase4.partial_writer_failure_recovery`
  - Scope: mapper-success/writer-fail path restores consistent pre-write or repaired state.
  - Evidence: no half-applied side-effects; reconciliation transcript exists.
- [ ] `phase4.missing_decision_result_restart_rerun`
  - Scope: restart rerun emits/recovers missing decision-result path correctly.
  - Evidence: decision-result presence and valid subsequent transition.

**Execution rule:** Phase-4 operational recovery is not considered complete until all checklist items pass in the incident context or in equivalent targeted drill scope.

## Exit criteria checklist (mirrors `10-phase-4-hardening-and-recovery.md`)

- [ ] Invariant-violation remediation automation exists and is proven: pause, emit violation, repair/reconcile, reopen-or-fail.
- [ ] Recovery test matrix coverage includes:
  - [ ] power loss during claimed-job materialization
  - [ ] duplicate terminal event replay
  - [ ] partial writer failure (mapper success, writer fail)
  - [ ] completion-gate mismatch path
  - [ ] missing decision-result event on startup (decider rerun)
- [ ] Operational runbooks are available and on-call accessible for invariant violations and replay recovery.
- [ ] Decider replay/restart procedures are documented and practiced.
- [ ] Alerting/SLO definitions include completion-gate mismatch frequency, decider replay loops, and dedupe growth anomalies.
- [ ] Cleanup/retention policies are finalized for dedupe tables and claimed-job staging cleanup.
- [ ] Temporary rollout toggles and transitional shims are removed after stability proof.
- [ ] Auditability requirement is met: corrective actions emit auditable events and reason codes.
- [ ] Replay-safe behavior is demonstrated: handlers remain idempotent under repeated replay.
- [ ] Validation gate satisfied in both required paths:
  - [ ] `SimCoreTests` recovery matrix suites pass.
  - [ ] `SimCoreDBValidation` phase-4 hardening/recovery validations pass.

## Incident artifact template (optional)

- Incident ID:
- Detection time (UTC):
- Affected workflows/cohorts:
- Symptom class: delayed / duplicate / out-of-order / invariant violation
- Paused scope:
- Validations executed (`phase4.*` + result):
- Remediation outcome: reopened / terminal-failed
- Rollback used: yes/no
- Follow-up actions:
