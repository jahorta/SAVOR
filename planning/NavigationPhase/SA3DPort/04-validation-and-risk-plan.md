# SA3D NJ Port Validation + Risk Plan

Date: 2026-04-18

Decision update: 2026-04-18
- Reference implementation for parity is pinned to:
  - parser reference repo: `https://github.com/X-Hax/SA3D.Modeling`
  - parser release tag: `1.2.1`
  - parser commit hash: `13813e7`
  - reference-runner fork/branch: `https://github.com/jahorta/SA3D.Modeling/tree/DetailedIO2`
  - policy: modify `DetailedIO2` per-slice to emit slice-specific input/output pairs.
- Milestone target is read/parse parity only; write parity is deferred.

---

## 1) Validation strategy

## A) Parity levels

1. **Structural parity**
   - Node counts
   - Attach counts
   - Poly chunk counts by type
   - Motion/node/frame counts

2. **Semantic parity**
   - Triangle totals per attach
   - Material/texture run boundaries
   - Strip-derived winding behavior and degenerates

3. **Binary parity (optional)**
   - Re-serialization byte equality where deterministic
   - Header/block layout equivalence for NJ output

## B) Golden outputs per fixture

For each NJ model/animation fixture, persist:
- block map (`offset -> header`)
- object tree summary
- chunk histogram
- triangle/material stats
- diagnostics

Store under:
- `planning/NavigationPhase/SA3DPort/goldens/` (proposed)
- fixture discovery root: `SoaSimFileParsing/inputs` using `*.mld` (all files).

## D) Parity harness architecture (reference .NET + C++)

1. **Reference extractor (.NET)**
   - Use forked runner from `jahorta/SA3D.Modeling` `DetailedIO2` branch.
   - For each fixture, emit stable JSON summaries (structural + semantic metrics) plus slice-specific input/output pairs.
2. **Port extractor (C++)**
   - Run Sa3Dport parser backend (`sa3d_port`) on the same fixtures.
   - Emit the same JSON schema.
3. **Reference extractor invocation (.NET bridge)**
   - Invoke SA3D reference parse (`sa3d`) through the .NET bridge from `SoaSimFileParsing` A/B CLI mode.
4. **Comparator**
   - Compare JSON outputs field-by-field with tolerances only where explicitly documented.
   - Emit per-fixture pass/fail plus mismatch diagnostics.
5. **A/B switch**
   - Drive A/B from `SoaSimFileParsing` CLI so `sa3d_port` and `.NET sa3d` consume the same MLD-derived fixture bytes.

## E) Recommended parity report format (v1)

Each fixture should emit one JSON document with:

1. **Header**
   - `fixture_id`, `mld_path`, `block_offsets`, `reference_version` (`SA3D.Modeling@13813e7`), `reference_runner_branch` (`DetailedIO2`), `timestamp_utc`.
2. **Structural metrics**
   - node count, attach count, chunk histogram, motion node count, frame count.
3. **Semantic metrics**
   - triangles per attach, material run count, texture run count, strip degenerates/winding counters.
4. **Diagnostics**
   - warnings/errors with stable codes and source locations (block offset + node index where applicable).
5. **Slice IO pairs**
   - per-slice captured inputs and outputs (operation id, input bytes/fields, output values/structures).
6. **Comparison summary**
   - per-section pass/fail, mismatch count, and top-N mismatch samples.

## F) Step-wise parity harness expansion (Slices 1-3)

Harness growth must follow implementation slices; do not require unavailable sections early.

### Slice 1 (primitives only)
- Implement in harness:
  - primitive IO pair capture/replay from `DetailedIO2` over real extracted NJ blocks (endian reads/writes + image-base arithmetic),
  - pointer LUT behavior IO pairs,
  - BAMS conversion IO pairs.
- Report sections enabled:
  - `primitives` (enabled),
  - `block_map`, `node_graph`, `mesh`, `motion` (marked `not_applicable`).
- Pass criteria:
  - byte/value parity for vectors and zero unexpected diagnostics.

### Slice 2 (NJ block + metadata shell)
- Implement in harness:
  - MLD-path fixture ingestion and NJ block extraction pipeline.
  - reference vs C++ block map comparison (`offset -> header`).
  - metadata shell decode diagnostics comparison.
  - replay `DetailedIO2`-emitted slice-specific IO pairs for block + metadata operations.
- Report sections enabled:
  - `primitives` + `block_map` + `metadata_shell` (enabled),
  - `node_graph`, `mesh`, `motion` (`not_applicable`).
- Pass criteria:
  - all expected NJ blocks resolved at matching offsets and headers.

### Slice 3 (Node core object graph)
- Implement in harness:
  - reference and C++ node tree summary extraction.
  - linkage invariant checks (child/next/parent coherence).
  - transform/attribute summary comparison by node index/path.
  - replay `DetailedIO2`-emitted node-graph IO pairs for targeted read/transform operations.
- Report sections enabled:
  - `primitives` + `block_map` + `metadata_shell` + `node_graph` (enabled),
  - `mesh`, `motion` (`not_applicable`).
- Pass criteria:
  - node graph structural parity at 100% metrics for fixture set.

## C) Acceptance gates

- Slice 3 gate: Node graph parity >= 100% structural metrics.
- Slice 5 gate: poly chunk histogram + triangle totals match reference.
- Slice 8 gate: animation frame/node summaries match reference.
- Slice 9 gate: weighted/corner stream checks match expected ranges.

---

## 2) Risk register

| Risk | Why it matters | Mitigation |
|---|---|---|
| Endian/imageBase mismatch | corrupt pointers and wrong subtree roots | build early pointer/endian microtests and assert all address arithmetic |
| LUT memoization divergence | duplicate nodes/attaches or broken references | emulate LUT get-add semantics exactly before parser recursion |
| Strip decoding drift | visible geometry mismatch | per-chunk golden tests; track winding/degenerate counts |
| Material state drift | wrong segmentation/material assignment | persist run-length stats and compare per attach |
| Hidden SA branches accidentally used | unexpected behavior creep | compile-time scope flags for NJ-only feature set |
| Over-porting nonessential files | schedule risk | strict “needed by entry path” checklist and slice gates |

---

## 3) Instrumentation checklist

- [ ] Add parser trace mode for block scan and image base changes.
- [ ] Add node read trace with address/label/attach pointer.
- [ ] Add poly chunk trace with type, size, and semantic impact.
- [ ] Add animation trace with node count, shortRot, frame counts.
- [ ] Add summary JSON emitter for fixture comparison.

Notes:
- Most risk signals should come from parity harness output.
- Keep lightweight in-parser assertions/microtests for early failures that the full harness may surface later
  (e.g., endian/address arithmetic, LUT uniqueness guarantees).

---

## 4) Recommended first fixture set

- 3 NJ model files: one simple, one medium, one problematic/replay-heavy.
- 2 NJ animation files: one shortRot false, one shortRot true.
- 1 “failure expected” file to validate diagnostics.

---

## 5) Completion definition (NJ-first milestone)

NJ-first milestone is complete when:
1. `ModelFile.ReadNJ` equivalent parses fixture set with stable summaries.
2. `AnimationFile.ReadNJ` equivalent parses fixture set with stable motion summaries.
3. Known mismatch cases have explicit diagnostic categories and are reproducible.
4. Deferred SA/Level paths remain intentionally out-of-scope and documented.
