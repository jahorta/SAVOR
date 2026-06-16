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
  --timeout-ms 30000 \
  --poll-ms 100
```
`--scenario` is optional, defaults to `seedprobe`.
You can pass it multiple times or use `--scenario all` to run:
`tasmovie`, `seedprobe`, `seedprobe_battle`, `battle`, `battle_macro_probe`,
`tasmovie_seedprobe`, `tasmovie_seedprobe_battle`,
`tasmovie_seedprobe_battle_override`, `tasmovie_battle` in that order.

Optional:

- `--migration-root <path-to-SavorDb/migration>`
- `--workspace-root <path>` (when omitted, uses `${TMP}/savor-e2e-default` and deletes that folder at startup)
- `--worker-dir-root <path>`

`SavorWorker.exe` is resolved from the same output directory as `SavorE2E`.

## Current scenario

- `seedprobe`
  - starts DB contexts through `DBService`,
  - seeds a starting savestate in StateDB,
  - seeds seedprobe spec rows in AuthoringDB,
  - creates `sp_probe_set` + `sp_probe_run` rows in AnalysisDB,
  - saves an authored workflow graph revision and creates a `workflow_graph` execution instance via API,
  - runs `DBWorkflowWorkerCoordinator` with one worker,
  - polls UiReadDB subscription state while coordinator loop is active.
- `seedprobe_battle`
  - preserves the previous seedprobe-backed battle smoke behavior.
- `battle`
  - starts from `--savestate-file`,
  - seeds a three-frame authored input set,
  - runs a direct `battle_chain` workflow without TAS or seedprobe prelude.
