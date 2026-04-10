# 01 - Scope and Success Criteria

## Problem Statement

Given:
- Walkable geometry extracted from dungeon data (GRND and related assets).
- Collision geometry and interaction/cutscene trigger volumes.
- A start condition and target objective.

We need to produce an input strategy that reaches the objective in the fewest VI frames, with acceptable determinism/reliability.

## In-Scope (MVP)

1. **Static world navigation**
   - Reconstruct walkable graph/mesh from extracted level data.
   - Represent non-walkable obstacles and trigger volumes.

2. **Objective-based routing**
   - Route between named objectives:
     - chest/interaction
     - doorway/zone transition
     - map exit/load trigger

3. **Cutscene-aware progression**
   - Detect and model cutscene-triggered interruptions.
   - Continue planning from post-cutscene state.

4. **Time-optimal baseline**
   - Optimize for completion time in VI frames, but keep optimization strategy simple in MVP.

5. **Simulator-backed route execution**
   - Produce a route/spline and solve for executable inputs in workers.

6. **UI-first workflow**
   - Navigation phase is UI-driven (no CLI workflow for objective specification in MVP).

## Non-Goals (MVP)

- Full global route planning across multiple maps with long-term resource constraints.
- Combat strategy co-optimization.
- Heavy optimization/meta-optimization for search budgets in first pass.
- Mandatory repeat-run validation gates in first pass (add if deterministic assumptions fail in practice).

## Success Criteria (MVP)

1. **Functional completion**
   - Can produce a successful route between at least two objective pairs in one dungeon.

2. **Cutscene continuity**
   - If a mandatory cutscene triggers, planner resumes from post-cutscene location and still reaches objective.

3. **Performance target**
   - Planning + execution pipeline completes within an acceptable offline budget (draft target: < 10 minutes per objective pair on dev hardware).

4. **Quality target**
   - Beats a hand-authored baseline route in at least one benchmark objective pair.

5. **World-model visibility**
   - UI can render a reconstructed 3D world model and selected path for inspection.

## Deliverables

- Data extraction spec for navigation-relevant world data.
- Planner/refiner artifact format for routes and candidate telemetry.
- Phase integration contract (job payload/result schema and step kinds).
- 3D world-model viewer and path overlay support in UI.
- Benchmark set + reporting template.

## Acceptance Benchmarks (Draft)

- Benchmark A: simple straight traversal with 0 cutscenes.
- Benchmark B: traversal requiring 1 mandatory interaction cutscene.
- Benchmark C: traversal with overlapping walk mesh elevation and tight collision corners.

## Risks

- Geometry mismatch between extracted data and runtime collision behavior.
- Trigger/script coupling (.SCT + controller behavior) not fully represented initially.
- Moving platform/controller rules requiring a second modeling pass.
