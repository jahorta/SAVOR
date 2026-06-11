# DBMigrateQueues 09 — Phase 4 Implementation Plan (Hardening and Optimization)

## Status
Future/current hardening plan. The stress-analysis items should start against the current facade-first
queued databases before any full command/query bus work is reconsidered.

## Purpose
Define the stabilization and tuning phase after all contexts are migrated, including capacity tuning, retry optimization, partitioning decisions, and optimization backlog capture.

## Phase 4 Scope (from Phase Plan)
- Tune queue capacities and retry defaults.
- Add advanced partitioning if needed.
- Plan separate batching optimization document/workstream.

Exit criteria:
- Stable operational performance.
- Backlog of follow-on optimizations prioritized.

## Implementation Outcomes
By the end of Phase 4:
1. Queue/retry configurations are tuned from observed data, not assumptions.
2. Partitioning decisions are made per context using explicit trigger criteria.
3. A separate batching optimization workstream is scoped and prioritized.

## Workstream A — Performance Baseline and Tuning Loop

### A0. Define stress-load scenarios for current databases
Create a repeatable stress harness for the current queued facade layer:
- Execution: ready-job claim/materialize/dispatch-shaped writes plus workflow query scans.
- State: artifact/savestate record creation and lookup bursts.
- Analysis: seed-probe and battle-analysis read/write mixes.
- Authoring: workflow graph/spec reads with bursty save/update operations.
- UIRead: projection-query bursts and subscription-status reads while projectors advance.
- Archive: archive catalog, package preview, and rehydrate-status operations.

For each context, record:
- queue depth p50/p95/max,
- enqueue accepted/rejected counts,
- completed/failed counts,
- total throughput,
- elapsed wall time,
- per-operation latency p50/p95/max when available,
- outbox/projection lag where relevant.

The first goal is evidence, not tuning. Use the results to decide whether facade-level capacity tuning is
enough or whether a specific operation needs command/query handler promotion.

### A1. Capture baseline metrics by context/lane
Collect multi-run baseline data:
- queue depth distribution (p50/p95/max)
- enqueue wait and processing latency
- retry counts and retry success rates
- outbox lag and publish failure rates

### A2. Tune queue capacities
Per context/lane, adjust capacities to reduce sustained backpressure while avoiding unbounded memory growth.

Method:
1. identify chronic pressure lanes
2. adjust capacity in controlled increments
3. rerun load profile
4. compare regression table

### A3. Tune retry defaults
Adjust:
- max retry ceiling
- backoff growth rate
- jitter range
- max delay cap

Target:
- maximize successful transient recovery while minimizing retry storms and latency inflation.

## Workstream B — Advanced Partitioning Decision and Implementation

### B1. Define partitioning triggers
Partitioning is introduced only when evidence shows single-lane serialization is insufficient, e.g.:
- persistent queue growth despite tuned capacity
- lane-specific latency SLO misses
- prolonged enqueue wait during burst workloads

### B2. Partitioning design per context
For candidate contexts, define:
- partition key strategy (aggregate-based or workload-specific)
- worker-to-partition assignment
- ordering guarantees preserved within partition key

### B3. Validate partitioned behavior
Run correctness and performance tests to ensure:
- no ordering regressions
- expected throughput gains
- no starvation of low-volume keys

## Workstream C — Reliability Hardening

### C1. Failure pattern review
Analyze recurring failures seen in Phase 2/3:
- top transient error sources
- retry exhaustion patterns
- outbox instability windows

### C2. Harden detection/alerting thresholds
Refine stuck-operation thresholds and backlog alerts by context based on observed distributions.

### C3. Run chaos-style drills
Execute controlled disruptions:
- DB contention spikes
- downstream publisher instability
- abrupt service restart during backlog

Verify graceful recovery and bounded backlog growth.

## Workstream D — Batching Optimization Workstream Definition

### D1. Create dedicated batching planning document
Document options for write batching/micro-batching:
- candidate operations
- expected wins and tradeoffs
- transactional and ordering implications

### D2. Prioritize follow-on experiments
Rank candidate optimizations by expected impact and implementation complexity.

### D3. Define guardrails
Specify non-negotiables for future batching work:
- preserve correctness and idempotency semantics
- preserve observable traceability
- avoid cross-context transactional coupling

## Decision Gates
Phase 4 closes only when:
1. Current database stress-load profiles are captured for all six contexts.
2. Queue and retry tuning is complete with before/after evidence.
3. Partitioning decisions (implement or defer) are explicitly documented per context.
4. Reliability drills confirm stable recovery behavior.
5. Batching optimization workstream is documented and prioritized.

## Suggested Execution Order
1. Define and run current database stress profiles (A0).
2. Collect baseline and tune capacities/retries (A).
3. Evaluate and implement/defer partitioning by criteria (B).
4. Harden reliability thresholds with drills (C).
5. Produce and prioritize batching follow-on plan (D).

## Risks and Mitigations
- **Risk:** overtuning for one workload harms others.
  - **Mitigation:** require per-context/lane regression comparison before accepting tuning changes.
- **Risk:** partitioning increases complexity without clear gain.
  - **Mitigation:** enforce evidence-based partitioning trigger criteria.
- **Risk:** batching planning reintroduces correctness risk.
  - **Mitigation:** record explicit safety guardrails before any batching implementation starts.

## Exit Artifacts
1. Current database stress-load report for all six contexts.
2. Capacity/retry tuning report with before/after data.
3. Partitioning decision record per context.
4. Reliability drill report and updated thresholds.
5. Batching optimization planning document drafted.
6. Prioritized optimization backlog published.
