# DBMigrateQueues 04 — Phased Rollout Plan

## Status
Draft v0.3 (planning)

## Purpose
Plan an incremental migration from direct DB adapter usage to CQRS + per-context queues/workers while minimizing risk.

## Phase 0 — Planning and Contracts
- Finalize architecture and contracts docs.
- Define request/result envelopes and error taxonomy.
- Identify pilot workflows and implementation strategy.

Exit criteria:
- Contract docs approved.
- Open questions narrowed to implementation-ready decisions.

## Phase 1 — Framework Skeleton
- Add queue/worker/bus abstractions and lifecycle wiring.
- Add concrete handlers for selected first context slice.
- Add baseline metrics and structured logging.

Exit criteria:
- System starts/stops cleanly with framework enabled.
- End-to-end command/query + outbox loop works in local/integration tests.

## Phase 2 — SeedProbe Workflow Pilot
- Implement SeedProbe workflow paths first as the pilot.
- Route pilot traffic directly to new async layer (no dual-path).
- Validate correctness, persistence behavior, and emitted event flow.

Exit criteria:
- SeedProbe pilot paths stable under expected test load.
- No correctness regressions in emitted events.

## Phase 3 — Expand to Remaining Contexts
- Add execution/state/authoring/archive/uiread incrementally until all contexts are implemented.
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

## Phase 3 Context Completion Checklist (Decision)
A context is phase-complete only when all are true:
1. Command path migrated to async facade
2. Query path migrated to async facade/queue
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

## Open Questions
- None currently.

## Detailed Phase Implementation Documents
- Phase 0: `05-Phase-0-Implementation-Plan.md`
- Phase 1: `06-Phase-1-Implementation-Plan.md`
- Phase 2: `07-Phase-2-Implementation-Plan.md`
- Phase 3: `08-Phase-3-Implementation-Plan.md`
- Phase 4: `09-Phase-4-Implementation-Plan.md`

