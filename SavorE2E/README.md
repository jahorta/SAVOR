# SavorE2E

Real-worker end-to-end scenario runner for SavorDb workflows.

## Purpose

`SavorE2E` is distinct from `SavorDbSchemaExport`:

- **SavorDbSchemaExport** focuses on fast deterministic contract and phase-gate checks.
- **SavorE2E** runs investigation scenarios through real worker process
  orchestration, checks infrastructure and durable contracts, and records the
  resulting game trajectory for human review.

SavorE2E never decides whether an observed game trajectory was desirable. A
coherent negative domain result, a different dynamic population, or an
output-guarded downstream skip does not fail a scenario. Automated failure is
reserved for execution/infrastructure failure or malformed durable evidence.
Canceled execution jobs are normal pruning outcomes: they remain in trajectory
reporting but do not fail invariant assessment. Failed jobs still do.

## Usage

```bash
SavorE2E \
  --scenario seedprobe \
  --savestate-file <path-to-start.sav> \
  --iso <path-to-game.iso> \
  --dolphin-base-dir <path> \
  --poll-ms 100
```

`--scenario` is optional and defaults to `seedprobe`. All three TAS Movie
scenarios are intentionally excluded from `all` and must be requested alone.

Optional workspace arguments include:

- `--migration-root <path-to-SavorDb/migration>`
- `--workspace-root <path>`
- `--worker-dir-root <path>`

SeedProbe and Battle may use an existing approved checkpoint with
`--workspace-root ... --source-savestate-id ...`. This preserves and appends to
the existing workspace. Fresh-source scenarios reset only their designated
scenario workspace.

An exact registered workflow unit with one required input and one static step
may be launched against an existing complete workspace with:

```bash
SavorE2E \
  --scenario workflow_unit \
  --workspace-root <existing-workspace> \
  --workflow-unit <unit-kind> \
  --source-ref-kind <durable-ref-kind> \
  --source-ref-id <positive-id> \
  --iso <path-to-game.iso> \
  --dolphin-base-dir <path>
```

This is a generic production-unit entry surface. It resolves the static unit
registry, authors one run-unique node, binds the declared input data kind to the
supplied durable reference, and uses the normal production registry and split
coordinators. Hidden units such as `battle_completion` and
`battle_recording` are supported without phase-specific dispatch. Run them as
separate explicit requests: completion consumes
`analysis_battle.turn_job:<turn-job-id>`, while recording consumes the resulting
`analysis_battle.battle_completion:<completion-id>`. The workspace is preserved
and the source binding is re-read after execution.

SeedProbe search authoring can be controlled with
`--seedprobe-min-value`, `--seedprobe-max-value`,
`--seedprobe-samples-per-axis`,
`--seedprobe-combo-attempts-per-target`, and
`--seedprobe-combo-sampler-tries`. Battle additionally accepts
`--battle-fake-attack-min` and `--battle-fake-attack-max`. The lightweight
defaults remain unchanged. The legacy-analogous first-Battle investigation uses
range `48..207`, 20 samples per axis, 32 combination attempts per target, 8
sampler tries, and fake attacks `0..5` from a prepared checkpoint.

`SavorWorker.exe` is resolved next to `SavorE2E`. Worker-backed scenarios have
no elapsed run deadline. They stop when the workflow reaches a durable terminal
state or the harness is explicitly canceled. Worker startup, protocol commands,
control-plane liveness probes, and irrecoverable startup exhaustion remain
bounded host operations.

## Assessment output

Every completed scenario emits a final assessment with three independent
fields:

- `execution=SUCCEEDED|FAILED` describes whether the scenario ran coherently;
- `invariants=PASS|FAIL` describes infrastructure and durable-contract checks;
- `trajectory=HUMAN_REVIEW_REQUIRED` marks all observed game results as a
  human decision.

The same trajectory records are written to stdout and the scenario's durable
log. They include workflow and step states, guard-skip reasons, job and job-set
results, artifacts, canonical progress, and phase-specific observations. No
expected turn count, candidate count, outcome, or workload shape affects the
exit code.

## Current scenarios

- `tasmovie`
  - authors and runs one singleton root-cursor establishment workflow;
  - validates the durable shape and provenance of either
    `RootCursorEstablished` or `Invalid`;
  - verifies the one-entry `TMI1` artifact when establishment succeeds;
  - reports the observed terminal PC, input count, attempt, job, artifact, and
    progress evidence without judging the result.
- `tasmovie_with_validation`
  - performs root-cursor establishment and, only when established, creates a
    separate exact-RTC root-validation workflow;
  - validates the RTC-patched DTM hash and the durable shape of either `Valid`
    root/checkpoint/sidecar publication or `Invalid` quarantine;
  - reports validation as not activated when establishment is `Invalid`.
- `tasmovie_seedprobe`
  - authors one four-node graph whose guarded typed edges connect root-cursor
    establishment, root validation, checkpoint sterilization, and SeedProbe;
  - verifies every realized phase and its exact provenance;
  - treats `Invalid` as a coherent domain result and requires downstream units
    to be guard-skipped when a required success output is absent;
  - reports which phases ran without requiring any particular realized path.
- `seedprobe`
  - supports imported savestates and approved prepared checkpoints;
  - validates the authored graph, entry provenance, typed results,
    accepted-frame/confirmation linkage, hashes, and durable run/job hierarchy;
  - reports discovered deltas, accepted representatives, stage participation,
    and workload counts without requiring a particular search trajectory.
- `battle`
  - supports fresh approved TAS Movie validation/sterilization or an existing
    prepared sterilized checkpoint;
  - validates the static SeedProbe/Battle Context join, exact Battle Context
    PCs and artifacts, BattleSet and realized wave lineage, predicate
    accounting, and outcome-dependent artifact contracts;
  - reports every observed BattleSet, wave, turn, candidate outcome, RNG/timing
    value, authored and concrete target, variant key, fake-attack choice,
    predicate binding/evidence, artifact, and progress event;
  - authors two generic turns where actor 0 attacks `AnyEnemy` and actor 1
    attacks the same resolved target; symbolic variants are materialized by
    coordination before workers receive them;
  - does not require a particular turn, continuation, selection, Victory,
    Defeat, or final BattleSet status.
- `workflow_unit`
  - preserves an existing complete workspace and launches one exact registered
    unit from a durable source reference;
  - validates the one-node authored contract, static activation/step lineage,
    immutable source binding, terminal evidence, and any observed root outputs;
  - reports dynamic downstream steps, domain results, artifacts, and progress
    without requiring a particular domain trajectory;
  - does not invoke or synthesize the Battle Results Screen handler.

All active workflow scenarios use the shared split coordinator composition.
Trajectory acceptance remains a human review activity; short synthetic tests
cover only the invariant/reporting boundary.
