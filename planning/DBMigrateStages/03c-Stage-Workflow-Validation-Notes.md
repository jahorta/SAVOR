# Stage 3c Workflow Validation Notes

## Scope

Operational notes collected during Stage 3c validation for the SeedProbe workflow vertical slice.

## Bottlenecks Observed

- **Readiness scan fan-out**: `PollReadyStepsFromDb` currently scans all running workflow instances and loads each graph snapshot; this is safe for the pilot but will scale linearly with active-instance count.
- **Projection granularity**: projector currently reprojects full instance shape on each relevant outbox event; this favors idempotency/simplicity over minimal writes.
- **Dual-path overhead**: in dual modes, both legacy and workflow transitions execute, increasing write amplification.

## Current Telemetry Fields

- `ready_scan_count`
- `ready_steps_enqueued`
- `last_ready_scan_latency_ms`
- `max_ready_queue_depth`

These are exposed via `DBWorkflowWorkerCoordinator::SnapshotTelemetry()` and intended for test harness capture.

## Mitigation Candidates

1. Add incremental ready-step query path keyed by `ready_at_utc` + `state='READY'`.
2. Batch workflow-instance projection updates by distinct `aggregate_id` over outbox windows.
3. Introduce bounded dedupe caches with TTL for terminal/ready signals to control memory growth in long-running processes.
