# SoaSimFileParsing MLD A/B scripts (Windows)

These scripts run the existing A/B mode in `SoaSimFileParsing`:

- `sa3d_port` (C++ path)
- `.NET sa3d` bridge reference path

A/B mode is enabled via `--ab-sa3d-port-vs-sa3d-bridge`, and slice selection is forwarded via `--dotnet-bridge-slice`.

## Scripts

- `run_ab_slice.bat`
  - Runs one slice.
- `run_ab_all_slices.bat`
  - Runs a configurable slice range (defaults `0..9`).

## Usage

From repo root (example):

```bat
SoaSimFileParsing\MLDScripts\run_ab_all_slices.bat "build\bin\Release\SoaSimFileParsing.exe"
```

With explicit bridge command:

```bat
SoaSimFileParsing\MLDScripts\run_ab_all_slices.bat ^
  "build\bin\Release\SoaSimFileParsing.exe" ^
  "SoaSimFileParsing\inputs" ^
  "SoaSimFileParsing\parsed\ab_slices" ^
  "" ^
  "dotnet run --project tools/sa3d_ref_runner/SA3DRefRunner.csproj --"
```
