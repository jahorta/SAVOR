# Stage 1 - Foundations and Project Setup

## Objective
Establish the implementation skeleton so schema and event work can proceed in parallel with minimal merge conflicts.

## Exit Criteria
- `SimCoreDB` project exists and builds.
- `SoaSimQt2` project exists (initially copied from `SoaSimQt`) and builds.
- Shared migration conventions and folder layout are in place.
- No behavior changes required yet.

---

## Deliverables

### 1. Solution and project wiring
- Add `SimCoreDB` project to solution.
- Add `SoaSimQt2` project to solution.
- Keep `SoaSimQt` untouched for fallback until cutover.

### 2. SimCoreDB package structure
Create initial namespaces/folders:

- `SimCoreDB/Execution`
- `SimCoreDB/State`
- `SimCoreDB/Analysis/Spine`
- `SimCoreDB/Analysis/SeedProbe`
- `SimCoreDB/Analysis/Battle`
- `SimCoreDB/Authoring`
- `SimCoreDB/UIRead`
- `SimCoreDB/Archive`
- `SimCoreDB/Common/Events`
- `SimCoreDB/Common/Migrations`

### 3. Migration infrastructure baseline
- Decide migration runner mechanism (reuse existing infra or define a new migration host).
- Define migration naming convention:
  - `YYYYMMDDHHMM_<context>_<description>.sql`
- Define migration folders per context.

### 4. Connection and DB factory contracts
Introduce context-specific DB factories/interfaces:
- `IExecutionDb`
- `IStateDb`
- `IAnalysisSpineDb`
- `IAnalysisSeedProbeDb`
- `IAnalysisBattleDb`
- `IAuthoringDb`
- `IUiReadDb`
- `IArchiveDb`

### 5. Environment/bootstrap config
Define configurable paths:
- `ExecutionDbPath`
- `StateDbPath`
- `AnalysisSpineDbPath`
- `AnalysisSeedProbeDbPath`
- `AnalysisBattleDbPath`
- `AuthoringDbPath`
- `UiReadDbPath`
- `ArchiveDbPath`
- `ObjectStoreRoot`
- `ArchiveStoreRoot`

### 6. Cross-cutting type definitions
Add shared primitives:
- Typed IDs (or strongly named wrappers).
- UTC timestamp helpers.
- Event envelope contract (`event_type`, `event_version`, etc.).
- Enum definitions for statuses/outcomes.

---

## Recommended Implementation Order (Task Breakdown)

1. Create projects and add to solution.
2. Add empty context folders and placeholder README in each.
3. Add per-context DB path configuration.
4. Add DB factory interfaces.
5. Add migration runner scaffolding with no-op migration.
6. Confirm build and dependency graph.

---

## Validation Checklist

- [ ] Solution builds with new projects.
- [ ] Startup loads DB path config without crash.
- [ ] Migration runner can execute one no-op migration per context.
- [ ] No runtime behavior changed yet.

---

## Risks and Mitigations

- **Risk:** Excessive coupling introduced while scaffolding.
  - **Mitigation:** Keep only interfaces/contracts in stage 1.
- **Risk:** Project copy introduces stale references in `SoaSimQt2`.
  - **Mitigation:** Add compile gate to ensure all namespaces and assets resolve.
