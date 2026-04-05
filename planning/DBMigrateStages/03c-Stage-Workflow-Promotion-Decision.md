# Stage 3c SeedProbe Workflow Promotion Decision Record

## Decision

- **Decision date:** 2026-04-05
- **Candidate promotion:** `DualWriteObserve` -> `WorkflowPrimary`
- **Status:** Pending evidence refresh

## Promotion Thresholds

1. **Parity:** >= 99.0% matched steps over comparison window.
2. **Recovery:** crash/restart reconciliation tests pass.
3. **Integrity:** invariant checks pass with zero critical violations.
4. **Readiness latency:** p95 readiness scan latency at or below agreed threshold.

## Evidence Artifact Contract

The promotion gate should consume machine-readable fields:

- `approved`
- `parity_percent`
- `parity_compared_steps`
- `parity_matched_steps`
- `recovery_passed`
- `integrity_passed`
- `readiness_scan_p95_ms`
- `readiness_scan_threshold_ms`
- `blockers[]`

## Approvers

- Workflow lead: _TBD_
- Runtime lead: _TBD_
- QA lead: _TBD_

## Rollback Triggers

1. Sustained parity regression below threshold.
2. Any reproducible duplicate-terminalization defect.
3. Integrity checker reporting dangling workflow edges or missing materialization links in post-run verification.
