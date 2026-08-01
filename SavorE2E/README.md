# SavorE2E

Real-worker end-to-end harness for SavorDb workflow scenarios.

## Purpose

`SavorE2E` is distinct from `SavorDbSchemaExport`:

- **SavorDbSchemaExport** focuses on fast deterministic contract/phase-gate checks.
- **SavorE2E** focuses on slower runtime integration checks using real worker process orchestration.

## Usage

```bash
SavorE2E \
  --scenario seedprobe \
  --savestate-file <path-to-start.sav> \
  --iso <path-to-game.iso> \
  --dolphin-base-dir <path> \
  --poll-ms 100
```
`--scenario` is optional, defaults to `seedprobe`.
You can pass it multiple times or use `--scenario all` to run:
`tasmovie`, `seedprobe`, `seedprobe_battle`, `battle`, `battle_macro_probe`,
`tasmovie_seedprobe`, `tasmovie_seedprobe_battle`,
`tasmovie_seedprobe_battle_override`, `tasmovie_battle` in that order.
Optional:

- `--migration-root <path-to-SavorDb/migration>`
- `--workspace-root <path>` (when omitted, uses `${TMP}/savor-e2e-default`)
- `--worker-dir-root <path>`

`SavorWorker.exe` is resolved from the same output directory as `SavorE2E`.
Worker-backed scenarios do not have an elapsed run deadline. They stop when
the workflow reaches a durable terminal state or the harness is explicitly
canceled. The split SeedProbe path also stops on irrecoverable worker-fleet
startup exhaustion. Worker startup, protocol commands, and control-plane
liveness probes remain bounded host operations.

## Current scenario

- `seedprobe`
  - starts DB contexts through `DBService`,
  - seeds a starting savestate in StateDB,
  - seeds seedprobe spec rows in AuthoringDB,
  - saves an authored workflow graph revision with one `seedprobe.run`
    step and creates its `workflow_graph` execution instance,
  - runs the production split `WorkflowCoordinatorService`,
    `JobExecutionCoordinator`, `WorkerCoordinator`, and
    `ProgramResultProcessor` path with the requested worker count,
  - verifies coordinator telemetry and durable job-set progress before
    reporting success.
- `seedprobe_battle`
  - preserves the previous seedprobe-backed battle smoke behavior.
- `battle`
  - starts from `--savestate-file`,
  - seeds a three-frame authored input set,
  - runs a direct `battle_chain` workflow without TAS or seedprobe prelude.
