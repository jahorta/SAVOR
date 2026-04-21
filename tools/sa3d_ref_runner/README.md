# SA3DRefRunner

Reference bridge runner for generating `parity_report_v1` JSON from fixture inputs, including extracted NJ block byte files.

## Commands

- `run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] --block-manifest <path> [--slice <n>] [--sa3d-modeling-dll <path>]`
- `run-all --manifest <path> --out <dir> [--slice <n>] [--sa3d-modeling-dll <path>]`

`run-all` fixture resolution behavior:

1. If `fixtures[].mld_path` entries are present in the manifest, those paths are used directly.
2. Otherwise, `fixture_policy.root` + `fixture_policy.glob` auto-discovery is used.
3. Relative paths are resolved from the manifest file directory.

## SA3D.Modeling binding

- The runner invokes `SA3D.Modeling` directly (via reflection) for each block in the fixture block manifest.
- `--sa3d-modeling-dll <path>` can be used to force a specific assembly location.
- If omitted, the runner attempts to find `SA3D.Modeling.dll` next to `SA3DRefRunner` or under known `third-party/SA3D.Modeling` build output locations.

## Block manifest protocol

- `--block-manifest <path>` allows one fixture-level invocation carrying all extracted NJ blocks.
- The runner validates block paths and emits consolidated block metrics (`block_count`, object/motion counts).
- This is intended for SoaSimFileParsing A/B flows that pass one manifest per fixture per slice.

## Determinism behavior

- UTF-8 no BOM output
- stable fixture ordering for `run-all`
- sorted key ordering for metric and IO dictionaries
- consistent diagnostic and batch summary shapes

## Build / Run

```bash
dotnet run --project tools/sa3d_ref_runner/SA3DRefRunner.csproj -- run-one --input SoaSimFileParsing/inputs/example.mld --out SoaSimFileParsing/parsed
```
