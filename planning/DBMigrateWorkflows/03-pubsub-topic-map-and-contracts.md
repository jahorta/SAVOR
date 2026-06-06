# 03 - Pub/Sub Topic Map and Event Contracts (Hybrid)

## Topic naming convention

`sim.<domain>.<entity>.<event>.v1`

Examples:

- `sim.workflow.step.input_requested.v1`
- `sim.workflow.step.input_fragment_ready.v1`
- `sim.execution.job.finished.v1`

## Required envelope fields

```json
{
  "event_id": "uuid",
  "event_type": "string",
  "event_version": 1,
  "context_name": "Execution|State|Analysis*|Authoring|Archive",
  "aggregate_kind": "workflow_instance|workflow_step|job_set|job",
  "aggregate_id": "string",
  "correlation_id": "string",
  "causation_id": "string|null",
  "occurred_at_utc": 0,
  "payload_ref_kind": "string",
  "payload_ref_id": 0
}
```

### Aggregate-id convention (resolved)

- For step-scoped events, set `aggregate_kind = workflow_step` and `aggregate_id = "<workflow_step_id>"` (string-encoded).
- Keep `workflow_instance_id` and `step_key` in payload/body for queryability and validation context.

## Topic map (phase-1 required)

| Topic | Producer | Consumer(s) | Partition key |
|---|---|---|---|
| `sim.workflow.step.input_requested.v1` | Control plane | Input providers | `workflow_instance_id` |
| `sim.workflow.step.input_fragment_ready.v1` | Input providers | Step aggregation | `workflow_step_id` |
| `sim.workflow.step.input_complete.v1` | Step aggregation | Materializer | `workflow_step_id` |
| `sim.execution.jobset.materialized.v1` | Materializer | Coordinator, observability | `job_set_id` |
| `sim.execution.job.finished.v1` | Workers | Result mapper, step completion | `job_set_id` |
| `sim.workflow.step.terminal.v1` | Control plane | Transition service/control plane | `workflow_step_id` |
| `sim.workflow.transition.decided.v1` | Transition handler | Control plane | `workflow_instance_id` |

Note: worker coordinator should not rely on a `job.queued` topic for claiming decisions; it should request/claim from ExecutionDB using coordinator-side heuristics.

## Durable payload-family note

Current Execution outbox rows use durable source payload refs such as `workflow_event`.
The earlier `workflow_input_event` payload family has been removed from the active schema/runtime; do not add new durable launch-input payloads there.
Launch inputs belong to `exec_workflow_instance_input_binding` and scalar launch choices belong to `exec_workflow_instance_argument`.

## Idempotency rules

1. Consumers must dedupe by `event_id`.
2. Step aggregation must dedupe by `(workflow_step_id, source_key, request_id?)`.
3. Materialization must enforce one active materialization per `workflow_step_id`.
4. Result mapping must be idempotent on `(job_id, mapper_version)`.
5. Transition decision subscriber must dedupe by `(workflow_step_id, terminal_state)` to prevent duplicate advancement.
6. Dedupe keys should be persisted in **dedicated tables per service** (not shared global utility table).

## Ordering assumptions

- Order is guaranteed only within partition key.
- Cross-key ordering is not guaranteed.
- Terminal step decision must be triggered only after all jobs in the step job set are terminal.
- System invariant target: no new job terminal events should appear after step completion is marked.

## Inconsistencies to resolve

- None for current iteration.

## Resolved inconsistencies

- Event namespace convention: use `Execution` namespace for workflow orchestration events.
- Source of truth for terminal step events: control plane.
- Decider evidence contract standardized via `evidence_ref` descriptor (`evidence_kind`, `evidence_id`, `source_context`, `snapshot_ts_utc`) plus idempotent fetch behavior.
- Invariant reconciliation policy defined: run authoritative repair job that recomputes step terminal state from job_set rows and emits corrective decision-result event with audit trail.

## Implementation questions for this doc

1. None for current iteration.

## Resolved questions

1. Split high-frequency progress traffic from terminal traffic.
2. For high-frequency topics, publish in batches.
3. Encode `workflow_step_id` as aggregate id string.
4. Async decider services fetch/request dependency evidence themselves.
5. For auditing/recovery, decider must emit decision-result event to control plane; on startup, missing decision results trigger new decider runs.
6. Worker coordinator should not consume `job.queued` topic; it claims directly from ExecutionDB.
7. Dedupe key storage approach: dedicated tables per service.
8. Decider evidence retrieval mode for milestone 1: DB-query contracts only (event-snapshot evidence deferred).
