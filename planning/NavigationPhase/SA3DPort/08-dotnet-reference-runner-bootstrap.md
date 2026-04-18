# .NET Reference Runner Bootstrap (Pre-Submodule)

Date: 2026-04-18
Status: In Progress

This document tracks what can be built now while the `jahorta/SA3D.Modeling` `DetailedIO` source is not yet integrated as a submodule.

## Completed bootstrap components

1. `phase0/REFERENCE_SOURCE.md`
   - Pinned parser commit and runner branch references.
2. `tools/sa3d_ref/fetch_ref.sh`
   - Automated checkout for `X-Hax/SA3D.Modeling@13813e7`.
3. `tools/sa3d_ref/fetch_runner.sh`
   - Automated checkout for `jahorta/SA3D.Modeling` `DetailedIO`.
4. `phase0/FIXTURE_MANIFEST.json`
   - Initial schema shell for fixture enumeration.
5. `tools/sa3d_ref/validate_manifest.py`
   - Basic static validation for fixture schema and on-disk paths.
6. `phase0/PARITY_REPORT_SCHEMA.json`
   - Locked baseline schema for `parity_report_v1`.

## Next steps once runner source is added

1. Add `tools/sa3d_ref_runner/` .NET project from document 07 proposed layout.
2. Wire `run-one` and `run-all` CLI commands.
3. Integrate summary extraction + slice IO pair capture.
4. Produce deterministic JSON output and batch summary index.
