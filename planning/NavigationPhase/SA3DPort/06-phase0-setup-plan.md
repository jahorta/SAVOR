# SA3D Port Phase 0 Detailed Setup Plan

Date: 2026-04-18
Status: Draft
Scope: detailed implementation plan for Slice 0 decisions.

---

## 1) Locked decisions

- Reference source is fixed to:
  - parser reference repo: `https://github.com/X-Hax/SA3D.Modeling`
  - parser release tag: `1.2.1`
  - parser commit hash: `13813e7`
  - reference-runner fork/branch: `https://github.com/jahorta/SA3D.Modeling/tree/DetailedIO`
  - workflow policy: update `DetailedIO` each slice to emit slice-specific input/output pairs.
- C++ naming and namespace mapping should match C# source as closely as possible.
- Implementation lives in a dedicated `Sa3Dport` Visual Studio C++ project.
- Fixture inputs are all `*.mld` files auto-discovered from `SoaSimFileParsing/inputs`.
- Fixture extraction path is via SoaSim MLD parser (not ad-hoc standalone NJ readers).
- Backend toggle is implemented inside the MLD parser provider path.

---

## 2) Phase 0 deliverables

1. `phase0/REFERENCE_SOURCE.md`
   - Records parser repo URL/tag/commit and reference-runner fork/branch details.
2. `phase0/NAMING_AND_NAMESPACE_MAPPING.md`
   - Defines exact C# -> C++ mapping conventions.
3. `phase0/FIXTURE_MANIFEST.json`
   - Declares auto-discovery policy for fixtures (`SoaSimFileParsing/inputs/*.mld`, selection=`all`).
4. `phase0/PARITY_REPORT_SCHEMA.json`
   - JSON schema for structural/semantic/diagnostic summary output plus slice IO pairs.
5. MLD parser backend switch design note
   - documents runtime switch point and data contract into both backends.

---

## 3) Implementation checklist

## A) Reference source freeze

- [ ] Add script (`tools/sa3d_ref/fetch_ref.sh` or equivalent) that clones/fetches repo and checks out `13813e7`.
- [ ] Add verification command to print checked-out hash.
- [ ] Add script (`tools/sa3d_ref/fetch_runner.sh` or equivalent) that clones/fetches `jahorta/SA3D.Modeling`
  and checks out `DetailedIO`.
- [ ] Add phase0 reference doc with exact commands.

Acceptance:
- parser command reliably lands on commit `13813e7` and runner command reliably lands on `DetailedIO`.

## B) Naming + namespace mapping

- [ ] Document mapping rules with examples:
  - namespace segments preserved under `Sa3Dport`.
  - type names unchanged where legal in C++.
  - method names preserved unless language constraints require adaptation.
  - enum values preserved 1:1.
- [ ] Add exception table for any unavoidable divergences.

Acceptance:
- every new Sa3Dport file can be mapped back to original C# type path with no ambiguity.

## C) Fixture manifest through MLD parser

- [ ] Build `FIXTURE_MANIFEST.json` with auto-discovery policy:
  - fixture root (`SoaSimFileParsing/inputs`),
  - glob (`*.mld`),
  - selection (`all`).
- [ ] Add manifest validator to ensure files exist and are readable.
- [ ] Add loader path in `SoaSimFileParsing` harness that reads discovery policy and resolves manifest entries.

Acceptance:
- manifest load passes and each fixture yields block discovery results.

## D) Parity report format

- [ ] Define schema version (`parity_report_v1`).
- [ ] Include sections:
  - header metadata,
  - structural metrics,
  - semantic metrics,
  - diagnostics,
  - comparison result.
- [ ] Add stable ordering rules for arrays/maps to make diffs deterministic.

Acceptance:
- two repeated runs on unchanged input produce byte-identical normalized report output.

## E) Toggleable backend in MLD parser

- [ ] Add parser mode switch in `SoaSimFileParsing` CLI for A/B:
  - `sa3d_port` (C++ parser path)
  - `sa3d` (.NET bridge reference parser path)
- [ ] Ensure both backends receive the same decoded NJ block bytes from MLD extraction.
- [ ] Add harness command/flag to run both backends over all discovered fixtures.

Acceptance:
- A/B run emits paired outputs per fixture with identical input block provenance.

---

## 4) Proposed parity report shape (example)

```json
{
  "schema": "parity_report_v1",
  "fixture": {
    "id": "mld_fixture_001",
    "mld_path": "path/to/file.mld",
    "model_block_offset": 123456,
    "motion_block_offset": 234567
  },
  "reference": {
    "source": "SA3D.Modeling",
    "tag": "1.2.1",
    "commit": "13813e7"
  },
  "metrics": {
    "structural": {},
    "semantic": {}
  },
  "diagnostics": [],
  "comparison": {
    "pass": true,
    "mismatch_count": 0
  }
}
```

---

## 5) Exit criteria for Phase 0 complete

Phase 0 is complete when:
1. Reference source pin is automated and verified.
2. Naming/namespace rules are documented and approved.
3. Fixture manifest is in place and readable through MLD parser path.
4. Parity report schema is locked and deterministic.
5. MLD parser backend toggle is implemented and wired for A/B runs.
