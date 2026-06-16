# Navigation Phase Planning

## Status

Future plan.

SAVOR owns navigation workflow orchestration, route planning, control solving, simulator execution, UI
selection, and result persistence. SPICE (Skies Package Interchange and Content Encoder) is the planned
submodule owner for Skies of Arcadia filetype parsing and content inspection. SAVOR should consume
SPICE-generated area packages/views instead of implementing MLD, SCT, or other SoA file decoders in this
repo.

This folder tracks planning documents for a new **Navigation Phase** focused on moving between world objectives in Skies of Arcadia (e.g., chest, door, loading zone) while minimizing total completion time in VI frames.

## Goals

- Consume a SPICE-generated representation of walkable space from the current area's MLD/SCT content.
- Plan objective-to-objective movement with a frame-time cost function.
- Account for interruptions (cutscenes, forced transitions, camera shifts).
- Refine candidate input tapes in simulator for time-optimal results.
- Integrate into existing SAVOR job/phase infrastructure.
- Render a 3D area view highlighting walking planes and potential navigation targets such as script
  triggers, treasure chests, doors, and load/zone transitions.

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

## Ownership Boundary

- SPICE owns SoA package/file parsing, including MLD/SCT reads, low-level model/script extraction,
  walking-plane candidate generation, and target discovery from script/object content.
- SAVOR owns persisted navigation specs, workflow launch, route/search/control-solver jobs, simulator
  validation, UI target selection, and presentation of SPICE content in SAVOR workflows.
- SA3DPort planning has been retired from this repo. Any parser/reference-comparison work belongs behind
  SPICE.
