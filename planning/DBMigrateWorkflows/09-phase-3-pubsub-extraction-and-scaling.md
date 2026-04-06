# 09 - Phase 3 Detailed Plan: Pub/Sub Extraction and Scaling

## Phase intent

Split coordinator responsibilities and harden event-driven throughput/operability.

## Add / Modify / Remove

## Add

1. **Service split implementation**
   - `WorkflowMaterializationService`
   - `WorkflowDispatchCoordinator`

2. **In-memory claimed-job DB + payload map**
   - Claimed-job staging lifecycle:
     1) claim
     2) materialize payload
     3) mark materialized
     4) dispatch from materialized subset

3. **Claim-selection policy implementation**
   - Priority order:
     1. savestate affinity
     2. program/runtime affinity
     3. fairness

4. **High-frequency event batching path**
   - Batch-publish progress events.
   - Keep terminal events on separate stream from progress traffic.

## Modify

1. **Coordinator dataflow**
   - Worker coordinator claims from ExecutionDB directly.
   - No dependency on `job.queued` topic for claiming decisions.

2. **Operational telemetry**
   - Add staged-job metrics:
     - materialization latency
     - stale claims
     - dispatch miss rate

3. **Outbox/consumer configs**
   - Tune partitions/consumer groups for split traffic.

## Remove

- Remove remaining direct dispatch-from-materialization coupling.
- Remove any queueing assumptions that bypass claimed-job materialization state.

## Guidance and constraints

1. **Correctness over max throughput**
   - Preserve completion-gate and transition invariants while scaling.

2. **Dedicated dedupe tables per service**
   - Each service owns dedupe retention and cleanup policy.

3. **Keep control-plane terminal source of truth**
   - Service split must not reintroduce ambiguity in terminal authority.

## Validation execution requirements

1. **SimCoreTests**
   - Run phase-3 service-split, batching, and dedupe-isolation suites.
2. **SimCoreDBValidation CLI**
   - Add/run phase-3 validation entries in `SimCoreDBValidation` for replay, lag/dead-letter, and stream-separation checks.
   - Phase cannot exit until both test suites and CLI validations pass.

## Suggested SimCoreTests to add for phase exit readiness

1. **Materialization/dispatch split integration test**
   - Verify claimed jobs are not dispatched before payload materialization completion.

2. **Claimed-job in-memory DB selection test**
   - Assert selection ordering honors savestate affinity, then program/runtime affinity, then fairness.

3. **Progress-vs-terminal stream separation test**
   - Validate high-frequency progress events do not delay terminal event processing.

4. **Batch publish behavior test**
   - Verify progress batching emits expected batch sizes and preserves per-batch ordering guarantees.

5. **Per-service dedupe isolation test**
   - Assert dedupe decisions in one service do not affect another service’s dedupe table behavior.
