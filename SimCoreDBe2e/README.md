# SimCoreDBe2e

Real-worker end-to-end harness for SimCoreDB workflow scenarios.

## Purpose

`SimCoreDBe2e` is distinct from `SimCoreDBValidation`:

- **SimCoreDBValidation** focuses on fast deterministic contract/phase-gate checks.
- **SimCoreDBe2e** focuses on slower runtime integration checks using real worker process orchestration.

## Usage

```bash
SimCoreDBe2e \
  --scenario seedprobe_real_worker_smoke \
  --savestate-file <path-to-start.sav> \
  --iso <path-to-game.iso> \
  --dolphin-base-dir <path> \
  --timeout-ms 30000 \
  --poll-ms 100
```

Optional:

- `--migration-root <path-to-SimCoreDB/migration>`
- `--workspace-root <path>`
- `--worker-dir-root <path>`

`SimCoreWorker.exe` is resolved from the same output directory as `SimCoreDBe2e`.

## Current scenario

- `seedprobe_real_worker_smoke`
  - starts DB contexts through `DBService`,
  - seeds a starting savestate in StateDB,
  - seeds seedprobe spec rows in AuthoringDB,
  - creates `sp_probe_set` + `sp_probe_run` rows in AnalysisDB,
  - builds/validates the `SEED_PROBE_CHAIN` workflow from definition and creates an execution workflow instance via API,
  - runs `DBWorkflowWorkerCoordinator` with one worker,
  - polls UiReadDB subscription state while coordinator loop is active.
