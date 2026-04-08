# DBMigrateQueues 00 — CQRS + Per-Context Queues Architecture Overview

## Status
Draft v0.3 (planning)

## Purpose
Define the target architecture for moving SimCoreDB from direct synchronous DB service calls to a CQRS-inspired model with per-context async queues/workers.

This document is the top-level map. Detailed documents in this folder define contracts, runtime flow, context layering, and rollout.

## Scope
In scope:
- SimCoreDB bounded contexts (Execution, State, Analysis, Authoring, UIRead, Archive)
- Async request handling via per-context queues/workers
- CQRS split of command vs query paths
- Reliability model for outbox/event publication
- Non-goals for this phase (notably: batching policy)

Out of scope (deferred):
- Write batching and micro-batching policy tuning
- Cross-process transport replacement details (if any)
- Full observability implementation details (metrics naming specifics)

## Current Baseline (from existing code)
- `DBService` owns sqlite connections and creates one `Sqlite*Db` service per context.
- Analysis context (`SqliteAnalysisDb`) already has explicit command-like methods, query methods, and outbox methods.
- Connection defaults already include WAL and FULLMUTEX, which support multi-threaded access patterns.

## Target Architecture Summary
For each bounded context:
1. **Command path**
   - Command bus entrypoint
   - Per-context write queue
   - Per-aggregate ordering for all contexts in phase 1 (context-specific keying)
   - Command handlers invoking existing DB adapter methods
2. **Query path**
   - Query bus entrypoint
   - Per-context read queue
   - Query handlers invoking existing DB adapter methods or read projections
3. **Outbox publication path**
   - Per-context outbox publisher worker
   - Poll unpublished outbox rows
   - Publish and mark publish success/failure

## Query Execution Options (for decision context)
Two valid query path options were considered:
1. **Explicit query queue + worker(s)**
   - Every query is enqueued and processed by dedicated queue workers.
   - Pros: consistent control surface (depth, backpressure, tracing), uniform behavior with command side.
   - Cons: additional queue hop overhead and queue implementation complexity.
2. **Bounded executor without explicit queue object**
   - Queries run via a bounded thread pool/semaphore model.
   - Pros: simpler implementation and potentially lower overhead.
   - Cons: less explicit queue-state visibility and potentially less uniform operations model.

**Decision:** use explicit per-context query queues.

## Design Principles
- Keep bounded context ownership strict.
- Avoid distributed transactions across DBs.
- Preserve domain event/outbox atomicity on write path.
- Prefer explicit contracts (typed envelopes/results/errors).
- Keep write serialization simple first; scale by partitioning later only when needed.

## What Changes vs What Stays
Stays:
- Existing `Sqlite*Db` implementations remain the persistence adapters.
- Existing outbox schema/pattern remains the reliability bridge.

Changes:
- Callers stop invoking DB adapters directly for workflow operations.
- New app-layer buses, queues, handlers, and workers are introduced.
- Standard async request/response contracts are defined.
- Per-context async facades become the primary entrypoint.

## Response Shape (baseline)
All async operations return a baseline response object containing only required fields:
- `state`: `success | canceled | failed`
- `error`: nullable/optional structured error payload

Optional metadata belongs to operation-specific response object types, not the shared baseline shape.

For commands in this phase, completion returns after DB commit succeeds or fails.

## Non-goals for this iteration
- No batching behavior design yet.
- No storage-engine swap.
- No redesign of domain tables.

## Deliverables (planning phase)
- Architecture docs (this folder)
- Open-question ledger in each doc
- Phase rollout plan with decision gates

## Decisions Logged (Latest Review)
1. Extended response objects are required for SeedProbe pilot writes: `RequestSeedProbeRun` (probe_run_id, queued_at, state), `RecordSeedProbeGridSeed` (grid_seed_id, probe_result_id, state), `RecordSeedProbeUniqueSeed` (unique_seed_id, probe_result_id, state), and `CompleteSeedProbeRun` (probe_result_id, run_status, state).
2. A shared helper should be used for constructing/validating context-specific ordering key format while preserving context-owned key semantics.
3. Logging should include both `ordering_key` and decomposed fields (`context`, `aggregate_type`, `aggregate_id`) for easier debugging.

## Open Questions
- None currently.
