# DBMigrateQueues 05 — Phase 0 Implementation Plan (Planning and Contracts)

## Status
Finalized v1.0 (approved decisions captured)

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

## Phase 0 Decision Register (2026-04-10 Meeting)

This register captures the final choices made to close Phase 0.

### 1) Envelope + contract surface
- 1.1 Command envelope required fields: **Option B**
  - Require `request_id`, `correlation_id`, `context`, `command_name`, `submitted_at_utc`, `payload`.
  - `causation_id` remains optional where applicable.
  - Payloads must be typed (no untyped blobs).
- 1.2 Query envelope required fields: **Option B**
  - Require `request_id`, `correlation_id`, `context`, `query_name`, `submitted_at_utc`, `payload`.
- 1.3 ID format and constraints: **Option B**
  - Use UUID strings with fixed max length constraints.
- 1.4 Timestamp semantics: **Option B**
  - Caller sets `submitted_at_utc`; worker sets `completed_at_utc`.
- 1.5 Result envelope state machine: **Option B**
  - Exactly one terminal state (`success | canceled | failed`) and immutable once set.
- 1.6 Idempotency key requirement policy: **Option B**
  - Required for all mutating commands.

### 2) Operation-specific response policy
- 2.1 Default response shape: **Option B**
  - Common envelope is default; extension fields are opt-in.
- 2.2 Approval bar for extensions: **Option B**
  - Add operation-specific fields only when caller behavior requires them now.
- 2.3 Forward compatibility strategy: **Option B**
  - Additive-only contract changes in v1.

### 3) Error payload and taxonomy
- 3.1 Error payload keys: **Option B + partial C**
  - Require `code` and `message`.
  - Optional `retry_after_ms`.
  - No stack traces in payload.
  - Optional DB provider error code allowed when DB failures occur.
- 3.2 Starter code families and baseline: **Option B**
  - Keep core cross-context codes plus context-specific prefixes.
- 3.3 Code governance: **Option B**
  - Use a shared, documented registry section; context owners propose additions.

### 4) Retryability matrix
- 4.1 Retry eligibility: **Option B**
  - Retry transient infra + backpressure classes only.
- 4.2 Retry budget defaults: **Option B**
  - Bounded retries with exponential backoff + jitter.
- 4.3 Caller behavior after retry exhaustion: **Option B**
  - Return terminal failed envelope with final code and available retry metadata.
- 4.4 Backpressure retry semantics: **Option B**
  - Delayed automatic retry with jitter and observability counters.

### 5) Idempotency + dedupe behavior
- 5.1 Dedupe key composition: **Option B**
  - `context + command_name + idempotency_key`.
- 5.2 Duplicate replay response: **Option B**
  - Return original completion envelope/metadata.
- 5.3 Dedupe retention policy: **Option B**
  - Finite retention with cleanup policy by command category.

### 6) SeedProbe pilot scope freeze
- 6.1 In-scope operation list: **Option B**
  - Minimal vertical slice including command + query + outbox path.
- 6.2 Owner/dependency mapping: **Option B**
  - Explicit operation-level ownership and dependencies.
- 6.3 Scope creep guardrail: **Option B**
  - Additions require explicit approval and rationale.

### 7) Pilot success assertions
- 7.1 DB correctness assertions: **Option C**
  - Do not require DB transition assertions in pilot acceptance checks.
- 7.2 Outbox/event assertions: **Option B**
  - Validate existence and expected metadata propagation.
- 7.3 Envelope/error assertions: **Option B**
  - Validate IDs, timestamps, state, and error payload shape.

### 8) Sync callsite migration map
- 8.1 Inventory completeness: **Option B**
  - Include all direct DB callsites reachable from pilot paths.
- 8.2 Migration ordering: **Option B**
  - Prioritize high-coupling/high-risk callsites first.

### 9) Ownership matrix
- 9.1 Accountability model: **Option B**
  - Single accountable owner plus backup per context.
- 9.2 Ownership boundaries: **Option B**
  - Explicit ownership for queue config, handlers, outbox behavior, and runbook maintenance.

### 10) Lifecycle contract
- 10.1 Start/Stop/Drain semantics: **Option A**
  - Best-effort/loosely defined semantics accepted for this phase.
- 10.2 Health snapshot minimum fields: **Option B**
  - Include running state, queue depth, in-flight work, error/liveness indicators.

### 11) Runbook template skeleton
- 11.1 Mandatory sections: **Option B**
  - Alerts, diagnosis, mitigations, and escalation paths are required sections.
- 11.2 Failure-drill requirements: **Option A**
  - Failure drills are optional in this phase.

### 12) Decision gate evidence model
- 12.1 Contracts gate evidence: **Option A**
  - Verbal approval accepted.
- 12.2 Pilot gate evidence: **Option C**
  - Full gate is deferred to Phase 2 validation outcomes.
- 12.3 Ownership gate evidence: **Option A**
  - Team-level assignment is accepted.
- 12.4 Backlog gate evidence: **Option B**
  - Tickets must include dependencies, owners, ordering, and acceptance criteria.

## Phase 0 Closure Summary

Phase 0 is considered closed with the above decision register as the normative baseline for Phase 1–4 execution.
