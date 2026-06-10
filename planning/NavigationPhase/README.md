# Navigation Phase Planning

This folder tracks planning documents for a new **Navigation Phase** focused on moving between world objectives in Skies of Arcadia (e.g., chest, door, loading zone) while minimizing total completion time in VI frames.

## Goals

- Build a robust representation of walkable space from GRND + collision/interactions.
- Plan objective-to-objective movement with a frame-time cost function.
- Account for interruptions (cutscenes, forced transitions, camera shifts).
- Refine candidate input tapes in simulator for time-optimal results.
- Integrate into existing SAVOR job/phase infrastructure.

## Documents

- `01-scope-and-success-criteria.md`
  - Problem statement, non-goals, and measurable success criteria.
- `02-state-and-world-model.md`
  - Proposed navigation state representation and geometry/cutscene modeling.
- `03-search-and-optimization-strategy.md`
  - Multi-stage planning strategy (A* + simulator refinement).
- `04-phase-and-job-integration.md`
  - Proposed phase contracts, step kinds, and artifacts in existing system.
- `05-open-implementation-questions.md`
  - Unresolved decisions and experiments to de-risk implementation.

## Iteration approach

These are initial planning docs with open implementation details. We should expect to revise aggressively as we prototype extraction, heuristics, and determinism checks.
