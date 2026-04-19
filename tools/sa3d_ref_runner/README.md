# SA3DRefRunner (Framework Scaffold)

This is an initial framework for the .NET SA3D bridge runner.

## Commands

- `run-one --input <file.mld> --out <dir> [--output-file <path>] [--manifest <path>] [--slice <n>]`
- `run-all --manifest <path> --out <dir> [--slice <n>]`

## Current status

- Emits deterministic JSON scaffold reports with `parity_report_v1` shape.
- Emits `slice_io_pairs` placeholders for slice-level replay wiring.
- Does **not** yet bind to `SA3D.Modeling` parser APIs (framework only).

## Build / Run

```bash
dotnet run --project tools/sa3d_ref_runner/SA3DRefRunner.csproj -- run-one --input SoaSimFileParsing/inputs/example.mld --out SoaSimFileParsing/parsed
```
