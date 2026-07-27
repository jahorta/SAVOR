## Approved scope

- Do not go beyond the scope explicitly approved by the user. This applies to implementation, refactoring, static analysis, live capture, validation runs, and creation of durable research artifacts.
- When approved work exposes a separate downstream issue or possible next boundary, report it and stop. Do not investigate or implement that boundary until the user explicitly approves it.

## Collaboration focus

- When the user asks a direct question, answer it promptly and narrowly. Do not delay the answer for adjacent investigation or expand into unrelated analysis unless it is necessary for accuracy. If additional investigation may be useful, answer the question first and offer that work separately.

## SavorE2E arguments

When running SavorE2E.exe, use these arguments for the required artifacts:
`--dtm-file "D:\SoATAS\beginning_in_first_battle.dtm" --savestate-file "D:\SoATAS\beginning_in_first_battle_rtc0.sav" --iso "D:\SoATAS\SkiesofArcadiaLegends(USA).gcm" --dolphin-base-dir "D:\SoATAS\dolphin-2506a-x64"`

- Use a managed background process so you can poll durable output and database state during the run.

## SavorPredict checkpoint runs

- `C:\savor` is the short run-root index for live SavorPredict checkpointed runs. When asked about recent checkpoint runs, latest live validation, or SavorPredict run state, inspect `C:\savor` first, sorted by `LastWriteTime`, before relying only on repo-local `Analyses\...` copies.
- Use short run roots under `C:\savor\...` for new SavorPredict checkpoint/live-capture runs unless the user names a different root. Deep repo paths can cause long-path and worker-runtime issues.
- Use the Release SavorPredict binaries for checkpoint/live-capture runs by default: `bin\x64\Release\SavorPredict.exe` and `bin\x64\Release\SavorWorker.exe`. Before long checkpoint sweeps, prefer a clean Release rebuild of `SAVOR.sln` with the VS 18 MSBuild path unless the user explicitly asks for Debug instrumentation.
- For each run root, prefer durable files such as `summary.txt`, `manifest.json`, `capture.jsonl`, `trace_checkpoints.txt`, `traces\*.txt`, `captures\*.jsonl`, and `db\*.db` before scanning worker logs. Worker logs are often large; start with summaries, manifests, trace reports, and database state.
- Keep durable analysis write-ups, reduced CSVs, and handoff findings in repo-local `Analyses\...` folders, but treat `C:\savor\...` as the first place to find the raw recent checkpoint artifacts.
- First-turn frame-state-machine live validation should always include the three-job `FUN_8002eb4c` corpus: source jobs `1755292` (`before_first_effect`, clone `1755312`, `eb4c_seq=341`, draw `9`), `147890` (`within_effect_span`, clone `1755293`, `eb4c_seq=515`, draw `129`), and `749380` (`after_last_effect`, clone `1755301`, `eb4c_seq=1868`, draw `476`). Use source job IDs for new live runs; clone IDs are only evidence references from the 2026-06-28 Release captures.

## SavorQt UI refreshes

- Widgets, pages, dialogs, and controllers that refresh database-backed UI state must use the shared async refresh pipeline in `SavorQt/GUI/Refresh`.
- Database reads and display DTO conversion should complete in the background before UI apply code runs.
- UI apply callbacks must only apply prepared data, and should use targeted table/model updates with stable row keys where available.
- DB refresh apply code may update DB-derived tables, lists, labels, and badges, but must not reset user-authored input widgets unless it captures/restores draft state or the selected backing item no longer exists.
- Do not add new ad hoc timer plus `QFutureWatcher` database refresh loops unless there is a documented reason the shared pipeline cannot fit.

## Local debugger

- WinDbg/cdb is installed at `C:\Program Files\WindowsApps\Microsoft.WinDbg_1.2603.20001.0_x64__8wekyb3d8bbwe\amd64\cdb.exe`.
- For live SavorQt investigation, prefer non-invasive attach (`-pv`) and detach cleanly with `.detach; q` unless the user explicitly asks for invasive debugging or process termination.
