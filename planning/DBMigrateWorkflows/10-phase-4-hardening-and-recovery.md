# 10 - Phase 4 Detailed Plan: Hardening and Recovery

## Phase intent

Finalize operational correctness, recovery behavior, and runbook readiness.

## Add / Modify / Remove

## Add

1. **Invariant-violation remediation automation**
   - Pause workflow instance on violation.
   - Emit invariant violation event.
   - Run authoritative repair/reconciliation job.
   - Reopen step or terminal-fail with explicit code.

2. **Recovery test suite execution path**
   - Power loss during claimed-job materialization.
   - Duplicate terminal event replay.
   - Partial writer failure (mapper success, writer fail).
   - Completion-gate mismatch path.
   - Missing decision-result event on startup (decider rerun).

3. **Operational runbooks**
   - Incident playbooks for invariant violations and replay recovery.
   - Decider replay/restart procedures.

## Modify

1. **Alerting + SLO definitions**
   - Add production alerts for:
     - completion-gate mismatch frequency
     - decider replay loops
     - dedupe table growth anomalies

2. **Cleanup/retention policies**
   - Finalize per-service dedupe table TTL/cleanup jobs.
   - Finalize staged claimed-job cleanup policy.

## Remove

- Remove temporary rollout toggles once recovery paths are proven stable.
- Retire transitional shims left from Seed Probe monolith split.

## Guidance and constraints

1. **Recovery correctness is primary**
   - Do not ship phase completion without passing full recovery matrix.

2. **Auditability requirement**
   - Every corrective action must emit auditable events and reason codes.

3. **Replay-safe design**
   - Ensure all handlers remain idempotent under repeated replay.

## Validation execution requirements

1. **SimCoreTests**
   - Run full phase-4 recovery matrix suites.
2. **SimCoreDBValidation CLI**
   - Add/run phase-4 hardening and recovery validations in `SimCoreDBValidation`.
   - Phase cannot exit until both test suites and CLI validations pass.

## Suggested SimCoreTests to add for phase exit readiness

1. **Invariant-violation remediation test**
   - Simulate violation and assert pause -> invariant event -> repair job -> reopen-or-fail sequence.

2. **Power-loss recovery test**
   - Inject interruption during claimed-job materialization and assert safe restart with correct final state.

3. **Duplicate terminal replay test**
   - Replay terminal events and assert idempotent outcomes without duplicate transitions/writes.

4. **Partial writer failure recovery test**
   - Force writer failure after mapper success and validate retry/reconciliation path correctness.

5. **Missing decision-result restart test**
   - Remove/omit decision-result event and verify startup decider rerun emits corrective decision result.
