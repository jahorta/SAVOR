# Stage 1 - Foundations and Project Setup

## Objective
Establish the implementation skeleton so schema and event work can proceed in parallel with minimal merge conflicts.

## Exit Criteria
- `SavorDb` project exists and builds.
- `SavorQt` project exists (initially copied from `SAVORQt`) and builds.
- Shared migration conventions and folder layout are in place.
- No behavior changes required yet.

---

## Deliverables

### 1. Solution and project wiring
- Add `SavorDb` project to solution.
- Add `SavorQt` project to solution.
- Keep `SAVORQt` untouched for fallback until cutover.

### 2. SavorDb package structure
Create initial namespaces/folders:

- `SavorDb/Execution`
- `SavorDb/State`
- `SavorDb/Analysis/Spine`
- `SavorDb/Analysis/SeedProbe`
- `SavorDb/Analysis/Battle`
- `SavorDb/Authoring`
- `SavorDb/UIRead`
- `SavorDb/Archive`
- `SavorDb/Common/Events`
- `SavorDb/Common/Migrations`

### 3. Migration infrastructure baseline
- Decide migration runner mechanism (reuse existing infra or define a new migration host).
- Define migration naming convention:
  - `YYYYMMDDHHMM_<context>_<description>.sql`
- Define migration folders per context.

### 4. Connection and DB factory contracts
Introduce context-specific DB factories/interfaces:
- `IExecutionDb`
- `IStateDb`
- `IAnalysisDb` (single DB file; logical groups for Spine/SeedProbe/Battle)
- `IAuthoringDb`
- `IUiReadDb`
- `IArchiveDb`

### 5. Environment/bootstrap config
Define configurable paths:
- `ExecutionDbPath`
- `StateDbPath`
- `AnalysisDbPath`
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
- **Risk:** Project copy introduces stale references in `SavorQt`.
  - **Mitigation:** Add compile gate to ensure all namespaces and assets resolve.
