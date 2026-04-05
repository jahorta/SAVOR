# Stage 3c SeedProbe Workflow Promotion Decision Record

## Decision

- **Decision date:** 2026-04-05
- **Candidate promotion:** `DualWriteObserve` -> `WorkflowPrimary`
- **Status:** Completed

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

Latest Results:
RequiredDataFieldsPresent=YES PassEvidence: approved=true, parity_percent=100, parity_compared_steps=100, parity_matched_steps=100, recovery_passed=true, integrity_passed=true, readiness_scan_p95_ms=5, readiness_scan_threshold_ms=20, blockers_count=0

## Approvers

- Workflow lead: SM
- Runtime lead: YA
- QA lead: LT

## Rollback Triggers

1. Sustained parity regression below threshold.
2. Any reproducible duplicate-terminalization defect.
3. Integrity checker reporting dangling workflow edges or missing materialization links in post-run verification.
