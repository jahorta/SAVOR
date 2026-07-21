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
`battle_end` is an explicit scenario and is not expanded by `all` because it requires a source savestate ID from an existing workflow run. `battle_end_results` is a source-compatible alias.

Optional:

- `--migration-root <path-to-SavorDb/migration>`
- `--workspace-root <path>` (when omitted, uses `${TMP}/savor-e2e-default`; `battle_end` and its alias preserve the selected workspace so the source DB row remains available)
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
- `battle_end` (`battle_end_results` alias)
  - requires `--source-savestate-id` from a successful `BattleSingleTurn` Victory job in the selected workspace,
  - creates the hidden `battle_completion -> field_return_seed_probe -> battle_results_screen` workflow through the execution DB,
  - selects one field-return seed with `--battle-end-seed-selector neutral|seed_value|seed_delta`; exact selectors also require `--battle-end-seed-value`,
  - relies on `WorkflowCoordinatorService` and normal worker claiming; it has no direct VM/job launch path.
