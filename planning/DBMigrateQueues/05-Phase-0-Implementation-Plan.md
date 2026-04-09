# DBMigrateQueues 05 — Phase 0 Implementation Plan (Planning and Contracts)

## Status
Draft v0.1 (implementation playbook)

## Purpose
Translate **Phase 0** from `04-Phased-Rollout-Plan.md` into concrete implementation steps so teams can execute planning/contracts work with no ambiguity.

## Phase 0 Scope (from Phase Plan)
- Finalize architecture and contracts docs.
- Define request/result envelopes and error taxonomy.
- Identify pilot workflows and implementation strategy.

Exit criteria:
- Contract docs approved.
- Open questions narrowed to implementation-ready decisions.

## Implementation Outcomes
By the end of Phase 0, we must have:
1. A frozen v1 queue contract surface for command/query/outbox operations.
2. A clear error code baseline, retryability mapping, and ownership model.
3. A SeedProbe pilot map with exact operations to migrate in Phase 2.
4. A delivery backlog that can be executed in Phases 1–4 without re-litigating foundational decisions.

## Workstream A — Contract Finalization

### A1. Freeze envelope schemas
Define canonical envelope fields and semantics for command/query requests and responses:
- mandatory IDs (`request_id`, `correlation_id`, `causation_id` where applicable)
- context routing (`context`, `command_name`/`query_name`)
- timestamps (`submitted_at_utc`, `completed_at_utc`)
- idempotency requirements for mutating operations
- shared `state` and structured `error` payload

Deliverables:
- A schema appendix section in contracts docs (JSON-like shapes + field definitions).
- Field-level constraints (required/optional, max lengths, legal values).

### A2. Define operation-specific response policy
For each pilot command/query, explicitly decide whether operation-specific response fields are required immediately.

Deliverables:
- A table mapping operation -> response extension object -> rationale.
- A compatibility note describing how future fields can be added safely.

### A3. Normalize error payload format
Finalize a common error payload shape across contexts:
- `code`
- `message`
- `retry_after_ms` (optional, only when known)
- optional diagnostic metadata safe for logs

Deliverables:
- Error payload contract section with examples for each top-level error class.

## Workstream B — Error Taxonomy and Retry Semantics

### B1. Codify code families and initial catalog
Adopt the agreed starter code families and define exact initial v1 codes.

Deliverables:
- Cross-context baseline list (`SCDB_VALIDATION_*`, `SCDB_TRANSIENT_*`, etc.).
- Context extension rulebook (`SCDB_EXEC_*`, `SCDB_ANALYSIS_*`, ...).

### B2. Define retryability matrix
Create a matrix for every error class/code indicating:
- automatic retry allowed?
- max retries and backoff profile source
- expected caller behavior (surface failure vs internal retry)

Deliverables:
- Error-to-retry decision table referenced by Phase 1 worker implementation.

### B3. Document idempotency + dedupe behavior
Specify dedupe key format and replay semantics.

Deliverables:
- Dedupe table key strategy.
- Replay response behavior for duplicate commands.

## Workstream C — SeedProbe Pilot Definition

### C1. Freeze pilot operation inventory
List exact operations in pilot scope, including command/query/outbox touchpoints.

Deliverables:
- Pilot operation list with owners and dependencies.

### C2. Define pilot success assertions
For each pilot operation, define correctness checks:
- DB state transitions
- emitted outbox/event expectations
- expected response envelope/error behavior

Deliverables:
- Pilot acceptance checklist used in Phase 2 verification.

### C3. Map synchronous callsites to async facades
Enumerate current direct DB callsites that must be replaced.

Deliverables:
- Migration map: caller -> current method -> target bus/facade call.

## Workstream D — Ownership, Lifecycle, and Runbook Skeleton

### D1. Confirm ownership boundaries
Per context, assign owning team for queue config, handlers, outbox worker behavior, and runbook maintenance.

Deliverables:
- Ownership matrix (context/team/on-call contact).

### D2. Define lifecycle contract for child components
Standardize `Start/Stop/Drain/IsRunning/HealthSnapshot` expectations.

Deliverables:
- Lifecycle behavior specification and minimum health fields.

### D3. Create runbook template skeleton
Prepare a standard runbook template to be filled per context in Phase 3.

Deliverables:
- Template sections (alerts, diagnosis steps, safe mitigations, escalation paths).

## Decision Gates
Phase 0 closes only when all gates are met:
1. **Contracts Gate:** request/result/error contracts approved by all context owners.
2. **Pilot Gate:** SeedProbe operations and test assertions are fully enumerated.
3. **Ownership Gate:** each context has named ownership and lifecycle expectations.
4. **Backlog Gate:** Phases 1–4 have implementation tickets with dependencies.

## Suggested Execution Order
1. Freeze envelope/error contracts (A + B).
2. Lock SeedProbe pilot scope and assertions (C).
3. Confirm ownership/lifecycle template (D).
4. Convert remaining unknowns into explicit, dated decisions.

## Risks and Mitigations
- **Risk:** Ambiguous contracts cause rework in Phase 1.
  - **Mitigation:** perform a contract walkthrough with all context owners before sign-off.
- **Risk:** Pilot scope creep delays framework work.
  - **Mitigation:** hard-freeze pilot inventory and move extras to Phase 3 backlog.
- **Risk:** Error handling inconsistencies across contexts.
  - **Mitigation:** centralize error code and retryability matrix in a single normative section.

## Exit Artifacts
1. Envelope and response schemas finalized.
2. Error taxonomy + retryability matrix finalized.
3. SeedProbe pilot operation map finalized.
4. Ownership matrix and runbook template finalized.
5. No open question blocks remaining for contracts/runtime basics.
