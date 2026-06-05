# DBMigrateQueues 06 — Phase 1 Implementation Plan (Framework Skeleton)

## Status
Draft v0.2 (facade-first implementation playbook)

## Purpose
Define exactly how to build and harden the first production-grade framework slice: queued facades, queue workers, lifecycle wiring, and baseline observability.

## Current Implementation Decision

Phase 1 proceeds through queued facades over existing DB interfaces before introducing a full command/query bus:

- `DBService` owns the concrete SQLite adapters and wraps them with queued facades.
- Existing context interfaces stay stable so current workflow, projector, archive, and UI code can migrate without broad callsite rewrites.
- SQLite adapters remain the SQL, transaction, and outbox write boundary.
- CQRS envelope/bus/handler pieces are deferred until a specific path needs stronger replay, idempotency, cross-process durability, or operation-level scaling than the facade model provides.

This narrows Phase 1 from "build every bus and handler abstraction first" to "stabilize the queue substrate in production-shaped code, then pull CQRS features forward selectively."

## Phase 1 Scope (from Phase Plan)
- Add queued facade primitives and lifecycle wiring.
- Preserve existing context interfaces while routing calls through read/write lanes.
- Add baseline metrics and structured logging.

Exit criteria:
- System starts/stops cleanly with framework enabled.
- End-to-end command/query + outbox loop works in local/integration tests.

## Implementation Outcomes
By the end of Phase 1, we should have:
1. Reusable queue + worker components with clear lifecycle semantics.
2. One fully wired context slice proving queued read, write, and outbox flows.
3. Structured observability sufficient to debug routing, latency, retries, and stuck work.

## Workstream A — Shared Queue/Worker Framework

### A1. Implement bounded queue primitive
Requirements:
- bounded capacity (per-context/per-lane configurable)
- non-blocking enqueue result (`accepted`, `rejected/backpressure`)
- optional delayed re-enqueue support hook for backpressure retries
- queue depth snapshot support

### A2. Implement worker loop abstraction
Requirements:
- start/stop/drain lifecycle
- cooperative cancellation support
- retry executor for transient failures using jittered exponential backoff
- per-item timing capture (`enqueue_to_start_ms`, `processing_ms`)

### A3. Implement health snapshot contract
Expose common health state:
- running status
- queue depth and capacity
- oldest item age (if available)
- retry counters
- last error summary

## Workstream B — Facade Contracts and CQRS Extension Points

The immediate work is facade hardening. Command/query buses remain extension points for later operation-specific promotion.

### B1. Command bus behavior
When a path is promoted beyond the facade, implement bus responsibilities:
- envelope validation
- idempotency pre-check hook
- ordering-key assignment/validation
- enqueue into correct context/lane
- return accepted/rejected envelope response

### B2. Query bus behavior
When a path is promoted beyond the facade, implement query responsibilities:
- envelope validation
- context/lane routing
- asynchronous execution via read queue
- typed result mapping and standardized failure reporting

### B3. Handler contracts
For promoted paths, define handler interfaces and adapter boundaries:
- command handlers call `I<Context>CommandDb`
- query handlers call `I<Context>QueryDb`
- no SQL in handlers; DB adapters remain transaction/SQL owners

### B4. Facade result semantics
Harden current facade behavior before adding bus layers:
- distinguish queue rejection from successful domain `false`/empty/`nullopt` results
- preserve existing method semantics for normal execution
- expose enough error text and telemetry for callers/tests to identify backpressure or stopped-lane failures

## Workstream C — First Context Slice Wiring

### C1. Choose minimal-but-real slice
Use the same context chosen for early framework proving (can be a subset of Analysis operations if SeedProbe full pilot is Phase 2).

### C2. Wire component graph
For selected context:
- context interface -> queued facade -> write queue -> write worker -> DB adapter
- context interface -> queued facade -> read queue -> read worker -> DB adapter
- outbox publisher worker -> DB outbox methods -> publisher -> mark success/failure

### C3. Lifecycle integration
Ensure context service owns child components and exposes unified startup/shutdown behavior.

### C4. UIRead projection lifecycle
`DBService` owns the UIRead projection worker alongside the queued facades. The worker:
- opens its own UIRead SQLite connection,
- attaches source context database files,
- runs `UiOutboxRelayCoordinator` against source outbox streams,
- advances `ui_projection_subscription` state in UIRead,
- keeps Qt2 on read-only UIRead access.

This is the SQLite3 implementation of the outbox projection decision. It replaces the temporary synchronous State-to-UIRead shortcut and keeps producer contexts responsible for publishing source events while UIRead remains a projection target.

## Workstream D — Observability Baseline

### D1. Structured logs
Log events at key transitions:
- enqueue accepted/rejected
- worker start/finish/failure
- retry scheduled/exhausted
- outbox publish success/failure

Include fields:
- `request_id`, `correlation_id`, `context`, `operation_name`
- `ordering_key` and decomposed key fields
- retry attempt, delay, and error code

### D2. Baseline metrics
Emit counters/gauges/histograms for:
- queue depth by context/lane
- enqueue rejections (backpressure)
- processing latency and enqueue wait latency
- retry count by code family
- outbox publish success/failure and lag

### D3. Stuck work detection hooks
Add duration threshold checks per operation type (warn-level first) with contextual logging.

## Workstream E — Test and Verification Harness

### E1. Unit tests
- queue boundary behavior (accept/reject)
- retry executor behavior (transient vs non-retryable)
- envelope validation and error code mapping

### E2. Integration tests
- command success path with DB commit
- query success/failure path
- outbox publish and mark flow including transient failure retry

### E3. Lifecycle tests
- repeated start/stop cycles
- drain completion under in-flight work
- graceful shutdown with no lost acknowledged work

## Decision Gates
Phase 1 closes only when:
1. Framework primitives and lifecycle API are stable and documented.
2. One context slice runs end-to-end command/query/outbox in integration tests.
3. Structured logs and baseline metrics are visible and validated.
4. Startup/shutdown/drain behavior is deterministic in local test runs.

## Suggested Execution Order
1. Build shared queue/worker core (A).
2. Harden facade result/error semantics and CQRS extension points (B).
3. Wire first context slice and outbox worker (C).
4. Add logs/metrics/stuck detection (D).
5. Lock in automated validation (E).

## Risks and Mitigations
- **Risk:** framework abstraction leaks context-specific behavior.
  - **Mitigation:** keep shared layer minimal; push domain rules into handlers.
- **Risk:** lifecycle race conditions on shutdown.
  - **Mitigation:** add deterministic drain semantics and repeated lifecycle tests.
- **Risk:** missing telemetry slows debugging.
  - **Mitigation:** require mandatory fields in logging helpers and metric labels.

## Exit Artifacts
1. Shared queue/worker/facade abstractions implemented.
2. First context slice fully wired through queued facades.
3. Baseline metrics/logging implemented and reviewed.
4. End-to-end local/integration tests pass.
5. Lifecycle behavior documented with examples.
