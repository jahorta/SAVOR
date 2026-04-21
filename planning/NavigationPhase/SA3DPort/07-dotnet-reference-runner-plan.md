# .NET Reference Runner Plan (`jahorta/SA3D.Modeling` `DetailedIO`)

Date: 2026-04-18
Status: Draft
Purpose: design and implementation plan for a reference runner that emits fixture summaries for C++ parity comparison.

Implementation note (2026-04-19):
- Initial framework scaffold now exists at `tools/sa3d_ref_runner` with command-line plumbing and JSON skeleton emission.
- SA3D.Modeling parser binding and slice-specific capture internals remain pending.

---

## 1) Objectives

- Build a deterministic .NET runner that uses SA3D.Modeling as the reference parser.
- Consume NJ model/motion blocks extracted from MLD fixtures.
- Emit stable JSON summaries aligned with `parity_report_v1`.
- Emit per-slice input/output pairs that can be replayed directly by the C++ port.
- Align each slice payload with a matching C++ test shortcut header (`Sa3Dport/Testing/Slice{N}TestApi.h`) so parity fixtures can be injected into tests without custom glue per test file.
- Support batch execution over fixture manifest.

Non-goals:
- No write/serialization parity in this phase.
- No SA/Level/BASIC/GC branch validation.

---

## 2) Inputs and outputs

## Inputs
- pinned parser source: `X-Hax/SA3D.Modeling` tag `1.2.1`, commit `13813e7`.
- runner source: `jahorta/SA3D.Modeling` branch `DetailedIO`.
- fixture discovery policy (`SoaSimFileParsing/inputs/*.mld`, include all present files).
- extracted NJ model/motion bytes (provided by MLD parser extraction stage).

## Outputs
- one JSON summary per fixture/reference run.
- one JSON IO-pair artifact per fixture/slice (`inputs` + `outputs`).
- optional aggregated batch summary (`summary_index.json`).

---

## 3) Runner architecture

1. **Fixture discovery reader**
   - loads discovery policy from `FIXTURE_MANIFEST.json` and resolves fixtures from `SoaSimFileParsing/inputs`.
2. **Block provider adapter**
   - receives model/motion NJ blocks from MLD extraction pipeline.
   - writes temporary buffers for parser invocation if needed.
3. **Reference parser wrapper**
   - invokes SA3D.Modeling read APIs for model and animation.
4. **Summary builder**
   - computes structural and semantic metrics.
5. **IO pair capture builder**
   - captures slice-targeted input/output pairs during parse operations.
   - normalizes shape so inputs map 1:1 to `Sa3Dport/Testing/Slice{N}TestApi.h` helper entry points.
6. **JSON writer**
   - emits deterministic report + IO pair artifacts using locked ordering and numeric formatting.

---

## 4) Suggested project layout

```text
tools/sa3d_ref_runner/
  SA3DRefRunner.csproj
  Program.cs
  Manifest/
    FixtureManifest.cs
    FixtureManifestReader.cs
  ExtractedBlocks/
    NjBlockInput.cs
    NjBlockProvider.cs
  Reference/
    Sa3dModelReader.cs
    Sa3dMotionReader.cs
  Summary/
    SummaryModels.cs
    StructuralMetrics.cs
    SemanticMetrics.cs
    Diagnostics.cs
  IOPairs/
    SlicePairModels.cs
    SlicePairCapture.cs
    SlicePairWriter.cs
  Output/
    JsonWriter.cs
```

---

## 5) Implementation steps

1. Bootstrap .NET CLI project and lock target framework.
2. Add parser dependency sourced from pinned checkout (`13813e7`) and runner edits on `DetailedIO`.
3. Implement manifest loader and validation.
4. Implement adapter for MLD-extracted NJ blocks.
5. Implement model summary extraction:
   - node/attach/chunk histograms.
6. Implement motion summary extraction:
   - motion node count/frame count/shortRot-related indicators.
7. Implement per-slice IO pair capture/export.
8. Implement JSON output with deterministic ordering.
9. Add batch command:
   - `run-all --manifest ... --out ...`
10. Add per-fixture command:
   - `run-one --fixture-id ...`
11. Add bridge-friendly command wiring for `SoaSimFileParsing`:
   - `run-one --input ... --out ... --manifest ...`

---

## 6) Determinism requirements

- invariant culture numeric formatting.
- stable dictionary key ordering.
- fixed precision rules for floating point fields.
- explicit UTF-8 output and normalized newline policy.

---

## 7) Error handling policy

- report failures in JSON diagnostics section with stable codes.
- include fixture id, model/motion block offsets, and stage name.
- do not crash batch run on single fixture failure; mark fixture failed and continue.

---

## 8) Integration with C++ parity harness

- C++ harness calls:
  1. MLD extraction step for NJ blocks.
  2. `SoaSimFileParsing --ab-sa3d-port-vs-sa3d-bridge` launches `.NET sa3d` bridge runner for expected summaries.
  3. same `SoaSimFileParsing` run executes C++ `sa3d_port` extraction for actual summaries.
  4. comparator for pass/fail and mismatch report.

- Ensure reference and C++ runs consume identical extracted block payloads.

---

## 9) Acceptance criteria

- Runner successfully processes all manifest fixtures.
- JSON outputs are deterministic across repeated runs.
- Comparator consumes reference outputs without schema transformation.
- At least one known-mismatch fixture produces actionable diagnostic mismatch output.
