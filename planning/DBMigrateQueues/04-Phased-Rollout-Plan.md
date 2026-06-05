# DBMigrateQueues 04 — Phased Rollout Plan

## Status
Draft v0.4 (facade-first decision recorded)

## Purpose
Plan an incremental migration from direct DB adapter usage to queued per-context database access while minimizing risk.

## Current Architecture Decision: Facade-First Queue Rollout

The active implementation path is a facade-first queue rollout:

- Keep existing `IExecutionDb`, `IStateDb`, `IAnalysisDb`, `IAuthoringDb`, `IUiReadDb`, and `IArchiveDb` caller contracts intact.
- Wrap each concrete SQLite adapter with a per-context queued facade owned by `DBService`.
- Preserve SQLite adapters as the SQL and transaction boundary.
- Add CQRS-style capabilities incrementally only where the facade model needs them for correctness, observability, or throughput.

This decision intentionally supersedes an immediate full command/query bus rollout. The full CQRS envelope/bus/handler model remains the long-term reference architecture, but it is not the next implementation step for every context.

Near-term priorities are:

1. Make facade backpressure and failure results harder to misinterpret.
2. Add idempotency/dedupe where mutating operations can be replayed or retried.
3. Add retry policy and structured telemetry around queue wait, processing time, rejection, and failure.
4. Promote operation-specific command/query handlers only for paths whose correctness, replay, or scaling needs exceed the facade model.

## Phase 0 — Planning and Contracts
- Finalize architecture and contracts docs.
- Define request/result envelopes and error taxonomy.
- Identify pilot workflows and implementation strategy.

Exit criteria:
- Contract docs approved.
- Open questions narrowed to implementation-ready decisions.

## Phase 1 — Framework Skeleton
- Stabilize queued facade primitives and lifecycle wiring.
- Keep command/query bus abstractions as an opt-in extension point rather than mandatory first-step infrastructure.
- Add baseline metrics and structured logging.

Exit criteria:
- System starts/stops cleanly with framework enabled.
- End-to-end queued facade + outbox loop works in local/integration tests.

## Phase 2 — SeedProbe Workflow Pilot
- Implement SeedProbe workflow paths first as the pilot.
- Route pilot traffic directly through the queued facade layer (no legacy DB bypass).
- Validate correctness, persistence behavior, and emitted event flow.

Exit criteria:
- SeedProbe pilot paths stable under expected test load.
- No correctness regressions in emitted events.

## Phase 3 — Expand to Remaining Contexts
- Add execution/state/analysis/authoring/archive/uiread queued facades incrementally until all contexts are implemented.
- Standardize per-context operational runbooks.

Exit criteria:
- All contexts implemented in async layer.
- On-call runbook and operational checklists established.

## Phase 4 — Hardening and Optimization
- Tune queue capacities and retry defaults.
- Add advanced partitioning if needed.
- Plan separate batching optimization document/workstream.

Exit criteria:
- Stable operational performance.
- Backlog of follow-on optimizations prioritized.

## Pre-Production Handling Decision (No Rollback Path)
Because this migration is pre-production build/test work:
- We do not define rollback triggers.
- If issues are found, we diagnose and fix bugs directly.
- We may adjust implementation scope/approach on the fly while iterating.

## Ownership Model Decision
Context/domain teams own their own queues/workers and context-specific behavior.
Shared framework components are consumed by context teams but do not centralize domain ownership.

## Phase Boundary Decision
Phase boundaries are scope-based (not date-based).
A phase closes when its defined scope/exit criteria are complete.

## Risk Register (starter)
- Hidden coupling to synchronous call assumptions
- Queue overload under burst traffic
- Incomplete idempotency causing duplicate effects
- Outbox publisher lag under downstream instability
- Facade rejection/fallback results being mistaken for successful domain results

## Phase 3 Context Completion Checklist (Decision)
A context is phase-complete only when all are true:
1. Command path migrated to queued facade or an explicitly approved command bus path
2. Query path migrated to queued facade or an explicitly approved query bus path
3. Outbox publish loop enabled and verified
4. Standardized error codes emitted and documented
5. Backpressure/retry behavior tested
6. Failure drill passed (e.g., DB busy/transient failure)
7. Required metrics visible
8. Runbook and ownership docs updated

## Decision Log (to fill during iteration)
- [x] SeedProbe workflow is the first implementation pilot
- [x] No dual-path validation period
- [x] No rollback path for pre-production migration; iterate/fix forward
- [x] Implement all contexts by end of phase 3
- [x] Context/domain teams own their own queues/workers
- [x] Phase boundaries are scope-based

## Decisions Logged (Latest Review)
1. Pre-production migration follows fix-forward iteration (no rollback plan).
2. Context/domain teams own their own queues/workers and context behavior.
3. Phase boundaries are scope-based.
4. Phase 0 planning/contracts decisions finalized on 2026-04-10 (see `05-Phase-0-Implementation-Plan.md` decision register).
5. Facade-first queue rollout is the active implementation strategy; full CQRS bus/handler rollout is deferred until a path needs stronger replay, scaling, or operational semantics than the facade can provide.
6. UIRead updates use source-context outbox projection. With SQLite3, the active implementation is an attached-source projector service owned by `DBService`: it attaches Execution, State, Analysis, Authoring, and Archive databases to a UIRead connection and advances UIRead subscription cursors without requiring Qt2 to write UIRead directly.

## Open Questions
- None currently.

## Detailed Phase Implementation Documents
- Phase 0: `05-Phase-0-Implementation-Plan.md`
- Phase 1: `06-Phase-1-Implementation-Plan.md`
- Phase 2: `07-Phase-2-Implementation-Plan.md`
- Phase 3: `08-Phase-3-Implementation-Plan.md`
- Phase 4: `09-Phase-4-Implementation-Plan.md`
