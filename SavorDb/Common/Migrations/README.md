# Common/Migrations

Migration filename convention:

`YYYYMMDDHHMM_<context>_<description>.sql`

Per-context migrations are rooted at `SavorDb/migration/<Context>/`.

## Embedded migration table generation

`MigrationRunner` uses an embedded migration table by default. The table is generated in
`$(IntDir)/GeneratedMigrations.h` and keyed deterministically by:

1. migration context folder name, then
2. migration filename, and
3. the SHA-256 of the migration contents.

The project pre-build step regenerates the embedded table whenever a migration
file is added, removed, renamed, or edited. It can also be regenerated
explicitly:

```powershell
# from repository root (Windows/VS developer shell)
cmd /c SavorDb\update_generated_migrations_command.txt
```

The same command is wired into `SavorDb.vcxproj` pre-build steps.
