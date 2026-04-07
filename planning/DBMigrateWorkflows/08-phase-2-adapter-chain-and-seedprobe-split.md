# 08 - Phase 2 Detailed Plan: Adapter Chain and Seed Probe Split

## Phase intent

Wire the full adapter chain and split Seed Probe into granular descriptor-based steps.

## Add / Modify / Remove

## Add

1. **Adapter-chain invocation wiring**
   - `input complete -> IJobPersistenceAdapter`
   - `job claimed -> IRuntimeInitAdapter`
   - `job terminal -> IResultMapper`
     - mapper must first build **step-specific `ResultINI` from `PRResult`**.
     - mapper must then consume that same `ResultINI` for step-specific terminal semantics (state decision, row writes, artifact resolution).
   - `step terminal -> IWorkflowTransitionHandler`

2. **Completion-gate enforcement**
   - Add gate requiring all jobs in step job set terminal before step completion/transition.
   - Add mismatch handling: `STEP_BLOCKED_COUNT_MISMATCH` + one reconciliation pass + terminal fail fallback.

3. **Seed Probe descriptor split (3-pass execution)**
   - Pass A: isolate neutral-phase interfaces.
   - Pass B: split grid/unique dependencies.
   - Pass C: enforce per-descriptor contracts and remove shims.

4. **Context-owned writer integration**
   - `IResultMapper` must expose **`BuildResultIniFromPrResult`** and `MapPrimaryResult`.
   - `ResultINI` generation and consumption are both workflow-step specific and must mirror legacy codec behavior boundaries.
   - Context-owned writers persist payloads where split ownership is enabled; otherwise mapper-side DB writes remain step-scoped until writer replacement is complete.

## Modify

1. **ProgramKindDescriptor registry**
   - Register `SeedProbe.Neutral`, `SeedProbe.Grid`, `SeedProbe.Unique` descriptors.

2. **Workflow definition registry**
   - Update grouped Seed Probe workflow to sequence those steps.

3. **Control-plane transition path**
   - Ensure transition decisions run async and only from terminal-gated step state.

## Remove

- Remove direct monolithic Seed Probe coupling from new pilot path.
- Retain old path behind compatibility toggle only as temporary fallback if needed during rollout.

## Guidance and constraints

1. **Case-by-case migration**
   - Do not expand beyond Seed Probe until pilot stabilization.

2. **Contracts first**
   - Every new step descriptor declares required inputs / provided outputs.

3. **Validation service usage**
   - Validate grouped workflow graph compatibility before enabling.

## Validation execution requirements

1. **SimCoreTests**
   - Run phase-2 adapter-chain, completion-gate, and split-contract suites.
2. **SimCoreDBValidation CLI**
   - Add/run phase-2 validation entries in `SimCoreDBValidation` for adapter order and transition gating.
   - Phase cannot exit until both test suites and CLI validations pass.

## Suggested SimCoreTests to add for phase exit readiness

1. **Adapter invocation order test**
   - Assert canonical call chain executes in order across lifecycle boundaries.

2. **Completion-gate correctness test**
   - Verify step terminal/transition is blocked until all jobs in job set are terminal.

3. **Mismatch semantics test (`STEP_BLOCKED_COUNT_MISMATCH`)**
   - Simulate expected-vs-discovered job count mismatch and assert block -> reconcile -> terminal-fail fallback behavior.

4. **Seed Probe split contract tests**
   - Validate `SeedProbe.Neutral`, `SeedProbe.Grid`, `SeedProbe.Unique` descriptors independently satisfy declared inputs/outputs.

5. **ResultMapper-to-writer contract test**
   - Assert mapper builds step-specific `ResultINI` from `PRResult`, consumes that same `ResultINI`, and either:
     - persists expected rows directly (legacy-compatible mode), or
     - emits typed payload that context-owned writer persists with provenance fields.
