# 07 - Phase 1 Detailed Plan: Step Input Aggregation MVP

## Phase intent

Introduce step input aggregation as a first-class path with strict readiness and timeout handling.

## Add / Modify / Remove

## Add

1. **`StepInputAggregationService` (initial implementation)**
   - In-process service hosted with control plane.
   - RAM-backed context keyed by `(workflow_instance_id, step_key)`.
   - Required input fragments are strict all-required.

2. **Input aggregation state model**
   - Add in-memory state tracking for:
     - required input descriptors
     - fragment readiness by source
     - pending request IDs
     - deadline and retry status

3. **Timeout + retry-once behavior**
   - Add explicit timeout path with single retry.
   - On second timeout/failure, mark step failure path ready for terminal handling.

4. **Event emission path**
   - Emit:
     - `step.input_requested`
     - `step.input_fragment_ready`
     - `step.input_complete`

## Modify

1. **Control-plane orchestration flow**
   - Insert aggregation service calls before materialization.
   - Ensure step cannot materialize before all required fragments are present.

2. **Telemetry**
   - Add per-step input latency metrics and timeout counters.

## Remove

- Remove implicit assumptions that ready step == materializable step.
- Keep existing coordinator loop while adding explicit aggregation gating.

## Guidance and constraints

1. **Seed Probe scope only**
   - Apply to Seed Probe pilot paths only for this phase.

2. **Keep behavior deterministic**
   - Duplicate fragments must be idempotent.
   - Retry count fixed at one for phase 1.

3. **Failure semantics**
   - Timeout retry exhaustion routes to blocked/failed flow for later terminal decision.

## Validation execution requirements

1. **SavorTests**
   - Run phase-1 aggregation behavior and idempotency suites.
2. **Validation coverage**
   - Add/run phase-1 coverage in `SavorTests` and relevant SavorE2E scenarios.
   - Phase cannot exit until both test suites and CLI validations pass.

## Suggested SavorTests to add for phase exit readiness

1. **All-inputs-required gating test**
   - Verify step does not materialize until every required fragment is present.

2. **Duplicate-fragment idempotency test**
   - Submit repeated identical fragment events and assert aggregation state remains stable.

3. **Timeout + retry-once behavior test**
   - Simulate missing fragment timeout; assert one retry attempt then blocked/failed path readiness.

4. **Event emission sequence test**
   - Assert `step.input_requested` -> `step.input_fragment_ready` -> `step.input_complete` sequence and required metadata.

5. **Aggregation keying test**
   - Validate contexts are isolated by `(workflow_instance_id, step_key)` and do not cross-contaminate.
