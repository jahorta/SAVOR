# SOASim DB Migration Program - Architecture Decisions and Target State

## Purpose
This document captures the finalized architecture decisions for the database split and event-driven migration effort. It is the source of truth for implementation stages in this folder.

---

## Finalized Decisions

### D1. Archive format for ephemeral execution data
- **Decision:** Use `JSONL + blobs` archive packages.
- **Rationale:**
  - Stream-friendly and easy to inspect.
  - Allows large payloads to remain externalized.
  - Supports checksum validation and partial replay.
- **Implications:**
  - Need archive manifest schema.
  - Need package integrity checker.

### D2. Restore behavior
- **Decision:** Rehydrate archived runs back into the **Execution DB**.
- **Rationale:**
  - Preserves future option to rerun old jobs.
  - Keeps replay semantics close to current worker system.
- **Implications:**
  - Need deterministic ID mapping and namespace strategy.
  - Need restore-safe handling for fingerprints.

### D3. Event versioning
- **Decision:** Per-event schema version (`event_type + event_version`).
- **Rationale:**
  - Independent evolution of slices.
  - Avoid global-lockstep evolution across contexts.
- **Implications:**
  - Projector dispatcher must route by event type/version.
  - Contract docs required for each event version.

### D4. Migration split timing
- **Decision:** Split bounded contexts all at once (single coordinated migration wave).
- **Rationale:**
  - Avoid prolonged dual-write/dual-read complexity.
  - Reach clean architecture faster.
- **Implications:**
  - Requires comprehensive rollout checklist.
  - Requires strong migration verification suite.

### D5. Analysis schema strategy
- **Decision:** Hybrid model:
  - **Small shared provenance spine**
  - **Rich explicit mode tables**
- **Rationale:**
  - Keeps cross-mode traceability.
  - Preserves clear, implementation-friendly domain schemas.

### D6. Job set discipline
- **Decision:** `job_set` remains orchestration-only.
- **Rule:** Domain grouping must live in domain set tables (e.g., `battle_set`, `seed_probe_set`).

### D7. Seed probe ownership
- **Decision:** Add **Analysis.SeedProbe** as explicit analysis context in implementation now.
- **Required results to persist:**
  - neutral seed
  - grid seed values
  - unique seed values
- **Rule:** Seed probe output is durable analysis data, not execution-only log data.

---

## Target Bounded Contexts and Datastores

1. **Execution DB** (ephemeral): jobs, triggers, lease state, event log, outbox.
2. **State DB** (durable): object refs, savestates, derivations.
3. **Analysis DB** (durable): one DB file with logical schema groups:
   - Analysis.Spine (small cross-mode provenance)
   - Analysis.SeedProbe (seed probe durable facts/results)
   - Analysis.Battle (explicit battle exploration tables)
4. **Authoring DB** (durable): plans/templates/predicates/settings.
5. **UI Read DB** (rebuildable): denormalized query models.
6. **Archive Index DB** (durable): package catalog and restore requests.
7. **Blob/Object Store** (durable): artifacts and archive files.

---

## Operating Principles

- Event-driven integration via transactional outbox in each write context.
- Outbox/event envelopes must include `event_id`, `event_type`, `event_version`, `context_name`, `correlation_id`, and `causation_id` to support idempotency and cross-context traceability.
- Idempotent projectors only.
- No cross-DB foreign keys assumed.
- Strong auditability: every derivation should have event provenance.
- Any data required for future reasoning should be in durable analysis tables.

---

## Implementation Ordering Summary

1. Foundation and project structure.
2. Schema definitions + migrations for all target DBs.
3. Workflow orchestration transition (codec decomposition + explicit workflow steps + trigger compatibility bridge).
4. Workflow validation vertical slice (runner integration + dual-path parity + recovery validation).
5. Outbox + event contracts + projector scaffolding.
6. Archive/rehydration pipeline for execution data.
7. UI Read model projections and SoaSimQt2 read cutover.
8. Validation suite, backfill, and production hardening.

See stage documents `01`, `02`, `03b`, `03c`, `03`, `04`, and `05` for concrete tasks.
