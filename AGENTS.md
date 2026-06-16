## SavorE2E arguments

When running SavorE2E.exe, use these arguments for the required artifacts:
`--dtm-file "D:\SoATAS\beginning_in_first_battle.dtm" --savestate-file "D:\SoATAS\beginning_in_first_battle_rtc0.sav" --iso "D:\SoATAS\SkiesofArcadiaLegends(USA).gcm" --dolphin-base-dir "D:\SoATAS\dolphin-2506a-x64"`

- Use a managed background process so you can poll durable output and database state during the run.

## SavorQt UI refreshes

- Widgets, pages, dialogs, and controllers that refresh database-backed UI state must use the shared async refresh pipeline in `SavorQt/GUI/Refresh`.
- Database reads and display DTO conversion should complete in the background before UI apply code runs.
- UI apply callbacks must only apply prepared data, and should use targeted table/model updates with stable row keys where available.
- DB refresh apply code may update DB-derived tables, lists, labels, and badges, but must not reset user-authored input widgets unless it captures/restores draft state or the selected backing item no longer exists.
- Do not add new ad hoc timer plus `QFutureWatcher` database refresh loops unless there is a documented reason the shared pipeline cannot fit.
