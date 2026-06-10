# .NET Reference Runner Bootstrap (Submodule Integrated)

Date: 2026-04-18
Status: In Progress

This document tracks bootstrap and follow-up tasks now that `jahorta/SA3D.Modeling` `DetailedIO2` is integrated as the `third-party/SA3D.Modeling` submodule.

## Completed bootstrap components

1. `phase0/REFERENCE_SOURCE.md`
   - Pinned parser commit and runner branch references.
2. `tools/sa3d_ref/fetch_ref.sh`
   - Automated checkout for `X-Hax/SA3D.Modeling@13813e7`.
3. `tools/sa3d_ref/fetch_runner.sh`
   - Automated checkout for `jahorta/SA3D.Modeling` `DetailedIO2`.
4. `phase0/FIXTURE_MANIFEST.json`
   - Initial schema shell for fixture enumeration.
5. `tools/sa3d_ref/validate_manifest.py`
   - Basic static validation for fixture schema and on-disk paths.
6. `phase0/PARITY_REPORT_SCHEMA.json`
   - Locked baseline schema for `parity_report_v1`.
7. `tools/sa3d_ref_runner/` framework scaffold
   - Added `SA3DRefRunner.csproj` + `Program.cs` with `run-one`/`run-all` commands and JSON report skeleton output.

## Next steps with runner source integrated

1. Integrate actual SA3D.Modeling parser calls into the `tools/sa3d_ref_runner` framework scaffold.
2. Extend `run-one` and `run-all` to emit real structural/semantic metrics (not framework placeholders).
3. Integrate summary extraction + slice IO pair capture for the current active slice.
4. Produce deterministic JSON output and batch summary index.

Implementation update (2026-04-20):
- `run-all` now supports either explicit fixture entries (`fixtures[].mld_path`) or policy-based auto-discovery.
- Relative fixture paths are resolved from the manifest location for reproducible invocation from different working directories.
- SPICE bridge invocation now supports `--dotnet-bridge-slice` and forwards `--slice` to the .NET runner.
- A/B comparison output now records reference `comparison` presence and pass-state probe fields.

Implementation update (2026-04-25):
- Bridge invocation now targets parity-capture APIs in-memory (`ParityReportGenerator.CreateFromBytes`) when available.
- Per-block parity diagnostics are checked and merged into fixture diagnostics.
- Block-level captures are collated to `slice_io_pairs` grouped by slice, where each pair entry includes `function_id`, `input_fields`, and `output` for downstream replay in `SPICE`.
- Pair inputs now carry base64 payload blobs to support direct deserialization/casting in port-side tests.
