# Common/Migrations

Migration filename convention:

`YYYYMMDDHHMM_<context>_<description>.sql`

Per-context migrations are rooted at `SimCoreDB/migration/<Context>/`.

## Embedded migration table generation

`MigrationRunner` uses an embedded migration table by default. The table is generated in
`$(IntDir)/GeneratedMigrations.h` and keyed deterministically by:

1. migration context folder name, then
2. migration filename.

Regenerate the embedded table after adding new `.sql` files:

```powershell
# from repository root (Windows/VS developer shell)
cmd /c SimCoreDB\update_generated_migrations_command.txt
```

The same command is wired into `SimCoreDB.vcxproj` pre-build steps and only regenerates
when it detects new `context/filename` entries.
