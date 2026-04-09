# DBMigrateQueues 07 — Phase 2 Implementation Plan (SeedProbe Workflow Pilot)

## Status
Draft v0.1 (implementation playbook)

## Purpose
Provide a step-by-step migration plan for the SeedProbe workflow as the first real workload on the new async CQRS queue layer.

## Phase 2 Scope (from Phase Plan)
- Implement SeedProbe workflow paths first as the pilot.
- Route pilot traffic directly to new async layer (no dual-path).
- Validate correctness, persistence behavior, and emitted event flow.

Exit criteria:
- SeedProbe pilot paths stable under expected test load.
- No correctness regressions in emitted events.

## Implementation Outcomes
By the end of Phase 2:
1. All in-scope SeedProbe commands/queries run through async facade only.
2. Correctness parity is demonstrated via deterministic assertions on DB state + events.
3. Operational behavior (retries, backpressure, latency) is understood under expected load.

## Workstream A — Pilot Operation Cutover

### A1. Finalize SeedProbe operation matrix
For each operation, define:
- request schema and validation rules
- target handler and DB adapter method
- idempotency requirement and dedupe key
- expected success and error responses

### A2. Replace direct callsites
Migrate caller entrypoints from direct DB methods to command/query buses.

Rules:
- no dual-write or dual-read path
- no fallback to synchronous adapter path after cutover
- maintain correlation/causation propagation

### A3. Preserve ordering guarantees
Apply agreed ordering key conventions to SeedProbe aggregate operations and validate ordering in tests.

## Workstream B — Data Correctness and Event Integrity

### B1. State transition verification
Define pre/postconditions for each command:
- record creation/update semantics
- terminal state protections
- conflict behavior and error code mapping

### B2. Outbox parity verification
For every relevant write:
- verify expected outbox row content
- verify publish attempts and mark success/failure behavior
- verify no duplicate semantic events from retries

### B3. Idempotency replay verification
Exercise duplicate submissions with same idempotency key and confirm replayed completion metadata semantics.

## Workstream C — Reliability and Failure Drills

### C1. Transient failure drills
Inject representative transient failures:
- `SQLITE_BUSY`-like contention
- temporary publisher downstream failure
- queue saturation/backpressure

Verify retry timing, retry ceilings, and final outcomes.

### C2. Non-retryable failure drills
Inject validation/conflict/non-retryable infrastructure errors and verify immediate surfaced failure with correct code.

### C3. Stuck-work detection validation
Run slow-operation scenarios and validate threshold-based warnings/metrics for SeedProbe operation classes.

## Workstream D — Load and Stability Validation

### D1. Expected-load simulation
Drive a load profile approximating planned pre-production usage:
- mixed command/query distribution
- burst behavior causing temporary queue pressure
- concurrent SeedProbe runs

### D2. Stability assertions
Confirm:
- no unbounded queue growth
- retry behavior converges
- no event correctness regressions
- latency remains within acceptable planning expectations

### D3. Telemetry review
Review logs/metrics for missing dimensions or noisy/insufficient signals; patch before phase close.

## Cutover Checklist (No Dual Path)
Before enabling pilot path broadly:
1. Caller migration complete and code search confirms no legacy callsites.
2. Integration test suite covers all pilot operations and failure classes.
3. On-call-facing pilot runbook section drafted (known alerts and diagnosis steps).
4. Feature configuration defaults to async path only.

## Decision Gates
Phase 2 closes only when:
1. SeedProbe operations are fully on async path with no legacy bypasses.
2. DB state and event emissions match expected behavior across success and failure scenarios.
3. Expected-load tests show stable queue/retry behavior.
4. Pilot runbook material is ready for broader context rollout.

## Suggested Execution Order
1. Lock operation matrix and callsite inventory.
2. Cut over callsites and enforce no dual path.
3. Validate correctness + outbox integrity.
4. Execute failure drills.
5. Run load/stability tests and close telemetry gaps.

## Risks and Mitigations
- **Risk:** hidden synchronous assumptions in callers.
  - **Mitigation:** hard-disable legacy paths and run integration tests against real async facades.
- **Risk:** retry behavior causes duplicate semantic side effects.
  - **Mitigation:** strict idempotency checks and duplicate-event assertions.
- **Risk:** pilot load exposes queue sizing issues.
  - **Mitigation:** tune capacities and retry delays using observed telemetry before phase close.

## Exit Artifacts
1. SeedProbe callsites migrated to async facades.
2. Correctness/event parity evidence documented.
3. Failure drill outcomes documented.
4. Expected-load stability report completed.
5. Pilot runbook section added.
