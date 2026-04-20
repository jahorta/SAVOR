# SA3DRefRunner

Reference bridge runner for generating `parity_report_v1` JSON from fixture inputs, including extracted NJ block byte files.

## Commands

- `run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] [--slice <n>] [--sa3d-parser-cmd <template>]`
- `run-all --manifest <path> --out <dir> [--slice <n>] [--sa3d-parser-cmd <template>]`

`run-all` fixture resolution behavior:

1. If `fixtures[].mld_path` entries are present in the manifest, those paths are used directly.
2. Otherwise, `fixture_policy.root` + `fixture_policy.glob` auto-discovery is used.
3. Relative paths are resolved from the manifest file directory.

## Parser command template

Use `--sa3d-parser-cmd` to invoke an external DetailedIO-compatible parser command.

Supported placeholders:

- `{input}` absolute fixture path
- `{output}` absolute temp output path that the parser command must write
- `{slice}` numeric slice stage

Example template:

```bash
--sa3d-parser-cmd "dotnet run --project /path/to/DetailedIO/Runner.csproj -- --input {input} --output {output} --slice {slice}"
```

If no parser command is provided, the bridge still emits baseline deterministic reports with warning diagnostics.

## Determinism behavior

- UTF-8 no BOM output
- stable fixture ordering for `run-all`
- sorted key ordering for metric and IO dictionaries
- consistent diagnostic and batch summary shapes

## Build / Run

```bash
dotnet run --project tools/sa3d_ref_runner/SA3DRefRunner.csproj -- run-one --input SoaSimFileParsing/inputs/example.mld --out SoaSimFileParsing/parsed
```
