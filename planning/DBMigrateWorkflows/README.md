# DB Migrate Workflows Plan (Hybrid Approach)

This folder contains design docs for migrating SimCoreDB workflow execution toward the **Hybrid orchestration model**:

- Central workflow control/orchestration path for step progression.
- Event-driven publish/subscribe outbox subscriptions for async coordination and projections.
- Program-kind pluggable adapters (`IJobPersistenceAdapter`, `IRuntimeInitAdapter`, `IResultMapper`, `IWorkflowTransitionHandler`).

## Why this folder exists

SimCoreDB already has:

- Workflow step state + orchestration command/query APIs.
- Outbox event contracts and relay/subscription infrastructure.
- A coordinator loop that polls ready steps and materializes work.

These docs define how to evolve from current state to a robust hybrid architecture without a full rewrite.

## Validation implementation note

- Workflow-migration phases use two verification paths:
  1. `SimCoreTests` (unit/integration coverage),
  2. `SimCoreDBValidation` CLI executable (phase-gate operational validations and replay/backfill checks).
- Phase docs below should include both paths in implementation and exit criteria.

## Documents

1. `01-current-state-and-gap-analysis.md`
   - What exists today and where it maps to the target hybrid architecture.
2. `02-target-hybrid-architecture.md`
   - Component boundaries, responsibilities, interactions.
3. `03-pubsub-topic-map-and-contracts.md`
   - Concrete topic map, event envelope standards, idempotency requirements.
4. `04-implementation-phases.md`
   - Incremental delivery plan and rollout gates.
5. `05-open-questions.md`
   - Implementation questions we need to resolve before locking each phase.
6. `06-phase-0-contracts-and-schema-prep.md`
   - Detailed add/modify/remove plan for Phase 0.
7. `07-phase-1-step-input-aggregation-mvp.md`
   - Detailed add/modify/remove plan for Phase 1.
8. `08-phase-2-adapter-chain-and-seedprobe-split.md`
   - Detailed add/modify/remove plan for Phase 2.
9. `09-phase-3-pubsub-extraction-and-scaling.md`
   - Detailed add/modify/remove plan for Phase 3.
10. `10-phase-4-hardening-and-recovery.md`
   - Detailed add/modify/remove plan for Phase 4.

## Iteration workflow

For each pass through these docs:

1. Resolve top-priority questions in `05-open-questions.md`.
2. Update the architecture/topic map docs to reflect decisions.
3. Re-run consistency checks listed in `04-implementation-phases.md`.
4. Capture unresolved contradictions in a dedicated “Inconsistencies” section of each doc.
