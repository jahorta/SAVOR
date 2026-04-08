# DBMigrateQueues 02 — Layering on Top of Existing Sqlite*Db Services

## Status
Draft v0.3 (planning)

## Purpose
Describe how CQRS + per-context queues layer on top of existing `Sqlite*Db` classes without replacing their core SQL responsibilities.

## Baseline Facts
- `DBService` currently opens each sqlite DB and instantiates one `Sqlite*Db` service per context.
- These services are exposed through interface pointers (`IExecutionDb`, `IAnalysisDb`, etc.).
- Analysis already exposes command-like methods, query methods, and outbox operations.

## Layering Strategy
### Keep existing adapters
`Sqlite*Db` classes remain:
- SQL execution boundary
- transaction boundary per operation
- row mapping and schema-specific details
- outbox read/mark/purge operations

### Add async application layer above adapters
Per context, add workload-category worker sets with explicit queues:
- `*CommandBus`
- `*QueryBus`
- `*WriteQueue` (explicit)
- `*WriteWorker` (one per workload category)
- `*ReadQueue` (explicit)
- `*ReadWorker` (one per workload category)
- `*CommandHandlers`
- `*QueryHandlers`
- `*OutboxPublisherWorker` (as child component with lifecycle API)

The new layer depends on `I*Db` interfaces and does not embed SQL.

## Example: Analysis Context Mapping
### Command handlers map to methods such as
- `CreateSeedProbeSet`
- `RequestSeedProbeRun`
- `RecordSeedProbeGridSeed`
- `CompleteSeedProbeRun`
- `CreateBattleTurnWave`
- `RecordBattleTurnJob`

### Query handlers map to methods such as
- `GetSeedProbeRun`
- `LookupSeedProbeResultId`
- `LookupSeedProbeNeutralSeed`
- `ListSeedProbeGridSeeds`

### Outbox publisher maps to methods such as
- `ReadUnpublishedOutboxBatch`
- `MarkOutboxPublished`
- `MarkOutboxPublishFailure`

## Interface Split Decision
Use separate interfaces for command and query responsibilities per context.

Recommended shape:
- `I<Context>CommandDb`
- `I<Context>QueryDb`
- concrete `Sqlite<Context>Db` can implement both in phase 1

Benefits:
- clearer responsibility boundaries
- easier test doubles for command/query paths independently
- simpler future specialization of read models

## Ownership and Lifecycle Decision
Each `Sqlite*Db` class should own and advertise its own queues/workers/facades for its context.

Implication for `DBService`:
- `DBService` only instantiates each database service layer.
- lifecycle start/stop is delegated to each context service.

## Compatibility / Migration Decision
No compatibility shims are required because service is not yet in production.

## Queue Library Decision
Start with a shared queue implementation library across contexts.
Specialized queue variants can be introduced later if needed.

## Shared Child Lifecycle API (v1 Decision)
Standard child component API for queues/workers/publishers:
- `Start() -> bool`
- `Stop() -> void`
- `Drain(timeout) -> bool`
- `IsRunning() -> bool`
- `HealthSnapshot()`

## Workload Category Flexibility
Workload categories are intentionally flexible in pre-production.
We will define/modify/remove categories case-by-case during implementation.
Operation-specific response objects can also evolve case-by-case.

## Responsibility Matrix
- Async layer owns: scheduling, retry policy, queue depth limits, backpressure, cancellation state.
- Sqlite layer owns: schema knowledge, SQL correctness, transaction internals.

## Dependency Direction
- Domain/application (handlers) -> command/query interfaces
- command/query interfaces -> concrete `Sqlite*Db`
- Never invert dependency from SQL adapter to queue/worker layer.

## Decisions Logged (Latest Review)
1. Initial workload categories will be defined case-by-case and remain flexible pre-production.
2. Outbox publisher remains a child component with explicit lifecycle API owned by each context service.
3. Start with a shared queue implementation library and evolve specialization later only if needed.

## Open Questions
- None currently.
