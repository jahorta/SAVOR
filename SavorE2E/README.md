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
You can pass it multiple times or use `--scenario all` to run `seedprobe`.
Both TAS Movie scenarios are intentionally excluded from `all` and must be
requested alone.
Optional:

- `--migration-root <path-to-SavorDb/migration>`
- `--workspace-root <path>` (when omitted, uses `${TMP}/savor-e2e-default`)
- `--worker-dir-root <path>`

`SavorWorker.exe` is resolved from the same output directory as `SavorE2E`.
Worker-backed scenarios do not have an elapsed run deadline. They stop when
the workflow reaches a durable terminal state or the harness is explicitly
canceled. The split runtime also stops on irrecoverable worker-fleet
startup exhaustion. Worker startup, protocol commands, and control-plane
liveness probes remain bounded host operations.

## Current scenario

- `tasmovie`
  - requires the handcrafted root movie through `--dtm-file`,
  - deletes the scenario's existing databases, object/archive stores, and TAS
    Movie runtime outputs before DB startup while retaining prior log files,
  - creates one `tas_movie_establish_root_cursor` workflow unit containing one
    single-attempt `tasmovie.establish_root_cursor` step,
  - takes no RTC or savestate argument and does not chain another phase,
  - requires a durable `RootCursorEstablished` Analysis attempt at
    `0x80101E48`, then materializes and verifies its one-entry `TMI1` artifact,
  - reports a durable typed `Invalid` as a failed E2E smoke result without
    changing the successfully completed business job.
- `tasmovie_with_validation`
  - performs the same fresh-database root-cursor establishment first,
  - requires one exact `--tasmovie-rtc` in `0..4294967295`, expressed as
    GameCube seconds since 2000-01-01,
  - waits for the first singleton workflow and dispatch to become quiescent,
    then creates a separate singleton `tas_movie_validate_root` workflow,
  - retains one live split coordinator and worker process while giving the two
    artifact-atomic worksets distinct nonzero workset epochs,
  - independently verifies the RTC-patched DTM hash and durable typed `Valid`
    root/checkpoint/sidecar publication or `Invalid` quarantine result.
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

Legacy battle and navigation scenario sources remain available as historical
reference, but they are not part of the E2E executable contract or build. The
active workflow scenarios use the shared split coordinator composition only.
