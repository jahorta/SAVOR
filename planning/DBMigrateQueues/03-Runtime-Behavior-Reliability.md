# DBMigrateQueues 03 — Runtime Behavior, Reliability, and Concurrency

## Status
Draft v0.3 (planning)

## Purpose
Define runtime semantics for queue processing, retries, error handling, and operational safety.

## Write Path Semantics
- Commands are enqueued per context.
- Write workers process with per-aggregate ordering.
- A command is considered completed when DB commit succeeds.
- Event publication is decoupled through outbox publisher workers.

## Read Path Semantics
- Queries execute asynchronously via explicit per-context read queues.
- Query operations are side-effect free.
- Query responses include operation timing fields where available.

## Retry Policy (initial)
- Retry only transient infrastructure failures.
- Do not retry validation/conflict failures.
- Use bounded retry count + jittered backoff.

## Deadlines / Timeouts Decision
- No timeout deadlines in phase 1.
- Focus on observability for operation duration and stuck-work detection.

## Backpressure
- Each queue has bounded capacity.
- Queue capacity is a **per-context configurable variable** (not one global constant).
- Overflow policy: reject and place operation into delayed automatic retry flow.

## Retry After Backpressure
- Requests rejected due to queue capacity should be automatically retried after a delay.
- Delay should use jitter to avoid synchronized retry bursts.
- Repeated backpressure retries should be observable via counters/timers.

## Outbox Publication Reliability
- Outbox publisher loops per context.
- Reads unpublished batch, publishes, marks success/failure.
- Failed publishes remain retryable with failure metadata.
- No dead-letter queue in phase 1.

## Concurrency Model (phase 1)
- One write worker per workload category with per-aggregate ordering.
- Read path uses explicit queue workers per workload category.
- No priority lanes in phase 1.
- No write batching in this phase.

## Phase 1 Numeric Defaults (Decision)
Queue capacities (initial):
- default lanes: `30`
- high-throughput lanes (seedprobe/jobs): `100`
- outbox lanes: `50`

Stuck-operation thresholds (initial):
- read ops: `5s`
- normal writes: `10s`
- heavy writes/completion ops: `30s`
- outbox publish ops: `15s`

Backpressure retry ceiling (initial):
- max automatic retries: `8`
- backoff: exponential with jitter
- max retry delay cap: `5s`

## Observability Requirements
Required:
- queue depth (per context, per lane)
- enqueue-to-start latency
- processing latency
- retry counts by error type
- backpressure retry counts
- outbox publish success/failure counts and lag

Success criterion for this phase:
- behavior is functionally correct and stable under expected test load.
- no formal metric gate thresholds required for promotion.

## Stuck Operation Detection
- Use duration threshold per operation type.
- Start with reasonably long thresholds and tune down as behavior is understood.

## Decisions Logged (Latest Review)
1. Queue capacity is per-context configurable and should be tuned by workload category.
2. Stuck detection uses per-operation-type duration thresholds, starting long and tuned down with data.
3. Backpressure rejections should auto-retry after a delayed/jittered interval.

## Open Questions
- None currently.
