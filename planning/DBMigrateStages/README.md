# DBMigrateStages

Detailed staged implementation plan for the SOASim bounded-context database split, event-driven integration, archive/rehydrate flow, and UI cutover.

## Files

- `00-Architecture-Decisions-and-Target.md`
  - Final architecture decisions and target bounded contexts.
- `01-Stage-Foundations-and-Project-Setup.md`
  - New project scaffolding and migration infrastructure setup.
- `02-Stage-Schema-and-Migration-Plan.md`
  - Concrete DB schema inventory and migration sequencing.
- `03b-Stage-Workflow-Orchestration-and-Codec-Decomposition.md`
  - Codec responsibility split, workflow-step orchestration model, and trigger compatibility cutover plan.
- `03c-Stage-Workflow-Validation-Vertical-Slice.md`
  - Vertical slice implementation plan to validate workflow orchestration with real runner integration before broad Stage 3 rollout.
- `03c-Stage3b-Compatibility-Matrix.md`
  - Concrete mapping of Stage 3b workflow schema/contracts to source files plus Stage 3c startup guard requirements.
- `03-Stage-Events-Outbox-and-Projectors.md`
  - Event catalog v1, outbox rules, and projector implementation plan.
- `04-Stage-Archive-and-Rehydrate.md`
  - JSONL+blob archive packaging and execution DB rehydration.
- `05-Stage-UI-Cutover-and-Validation.md`
  - SoaSimQt2 cutover, backfill, validation, and fallback.

## Suggested Usage

Implement in numeric order. Each stage file contains:
- objective
- deliverables
- ordered tasks
- exit criteria
- validation checklist

Recommended implementation sequence:
1. `01-Stage-Foundations-and-Project-Setup.md`
2. `02-Stage-Schema-and-Migration-Plan.md`
3. `03b-Stage-Workflow-Orchestration-and-Codec-Decomposition.md`
4. `03c-Stage-Workflow-Validation-Vertical-Slice.md`
5. `03-Stage-Events-Outbox-and-Projectors.md`
6. `04-Stage-Archive-and-Rehydrate.md`
7. `05-Stage-UI-Cutover-and-Validation.md`
