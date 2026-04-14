# SoaSimMLD plan: migrate NJCM output to a Blender-like intermediate representation

Date: 2026-04-14

## 1) Current SoaSimMLD status (what we have now)

This is the current effective pipeline:

1. MLD parser discovers NJCM chunks and decodes each chunk into `model::NjcmDecodedChunk`.
2. Decode path (often parity path) emits:
   - object records
   - attach records
   - semantic vertices
   - semantic polygons and semantic primitives
3. `GeometryBuilder` converts decoded attach data into generic `GeometryObject` with:
   - `SemanticMesh.vertices`
   - `SemanticMesh.polygons`
4. Qt scene conversion flattens that further into `SceneMesh { vertices, indices }` and renders as plain triangle/line buffers.

What is missing versus Blender/SA3D style ingest:

- No single normalized mesh interchange format that preserves both:
  - per-vertex weighted information
  - per-corner attributes (UV/color) grouped by material runs
- No direct exportable artifact that Blender can consume for A/B parity validation.
- Rendering path currently consumes simplified semantic polygons, so decode-vs-render disagreement is hard to isolate.

## 2) Target intermediate representation (IR)

Create a new SoaSim IR that mirrors the concepts used by SAIO/SA3D weighted buffers.

### 2.1 New core types (new files)

Add new model header/source pair:

- `SoaSimMLD/Model/NjWeightedMeshIR.h`
- `SoaSimMLD/Model/NjWeightedMeshIR.cpp` (if helpers are needed)

Proposed types:

- `IrVertex`
  - `position` (float3)
  - `normal` (float3)
  - `hasNormal`
  - `weights` (`vector<{boneOrNodeIndex, weight}>`) — optional for current static use but required for Blender-like parity.

- `IrCorner`
  - `vertexIndex`
  - `uv` (float2) + `hasUv`
  - `colorRgbaLinear` (float4) + `hasColor`
  - `normal` override optional (future-proof)

- `IrMaterial`
  - chunk material/flags payload (blend, cull, texture id, etc.)
  - stable `materialHash`

- `IrTriangleSet`
  - `materialIndex`
  - `corners` as packed triplets (`vector<IrCorner>` where size % 3 == 0)
  - source metadata (`polyType`, chunk offsets, replay/cache origin)

- `IrMesh`
  - `label`
  - `sourceObjectAddress`
  - `sourceAttachOffset`
  - `vertices`
  - `materials`
  - `triangleSets`
  - diagnostics (degenerate count, out-of-range count, replay count)

- `IrScene`
  - `meshes`
  - optional node/object transform refs

### 2.2 Design rules

- IR is **post-decode, pre-render**.
- IR must be deterministic and serializable (JSON debug output).
- Renderer consumes triangulated geometry derived from IR, not directly from NJCM decode records.
- Keep NJCM raw decode structs for auditing; do not remove them initially.

## 3) Code changes by subsystem

## 3.1 SoaSimMLD parse/normalize layer

### Add

1. `SoaSimMLD/Parsing/NjcmIrBuilder.h/.cpp`
   - Input: `ParseResult` + `decodedNjcmChunks`
   - Output: `model::IrScene`
   - Responsibilities:
     - convert semantic vertices/polygons/primitives into `IrMesh`
     - material/run partitioning
     - corner expansion
     - index validation and diagnostics

2. `SoaSimMLD/Parsing/NjcmIrDiagnostics.h/.cpp`
   - shared checks:
     - out-of-range indices
     - degenerate triangles
     - repeated cache replay contribution
     - winding consistency stats

3. `SoaSimMLD/Export/NjcmIrJsonExporter.h/.cpp`
   - write `IrScene` to JSON for Blender-side validator.

### Modify

1. `SoaSimMLD/Parsing/MldParser.h/.cpp`
   - Add option toggles:
     - `emitNjcmIr`
     - `emitNjcmIrJson`
     - output path for JSON artifacts.
   - Add `irScene` (or `optional<irScene>`) to `ParseResult`.

2. `SoaSimMLD/Parsing/SaToolsParity*Parser.cpp`
   - Preserve enough metadata during decode for IR material grouping:
     - chunk type
     - draw/cache flags
     - texture/material-affecting state where available.

3. `SoaSimMLD/Model/NjcmModel.h`
   - Keep existing decode structs, but add lightweight links/ids needed by IR builder.

### Remove / deprecate (phase-gated)

- Do **not** remove current decode structs now.
- Mark direct `GeometryBuilder` NJCM path as transitional once Qt path switches to IR-derived geometry.

## 3.2 Geometry assembly layer

### Modify

1. `SoaSimMLD/Parsing/GeometryBuilder.cpp`
   - For NJCM objects, stop building directly from `semanticPolygons/semanticPrimitives`.
   - Instead consume `IrMesh -> triangleSets` and flatten to `SemanticMesh` only as compatibility output.

2. Add optional new builder:
   - `SoaSimMLD/Parsing/IrGeometryBuilder.h/.cpp`
   - Converts `IrScene` to existing `GeometryBuildResult` for minimal disruption.

## 3.3 Qt scene/render path

### Modify

1. `SoaSimQt3D/Scene/BasicQtSceneBuilder.cpp`
   - Prefer IR-derived mesh data source when available.
   - Carry per-material-run debug labels into scene nodes.

2. `SoaSimQt3D/GUI/RuntimeSceneConverter.cpp`
   - Add overlay/debug info from IR diagnostics:
     - degenerate triangles
     - index overflow
     - cache replay contribution.

3. `SoaSimQt3D/GUI/StaticMeshGeometry.*`
   - keep as is initially (triangle/index consumer), but add optional color/uv attribute support if we visualize corner data later.

## 3.4 Tooling for Blender validation

### Add

1. `tools/njcm_ir_to_blender_json.py` (or within planning scripts area)
   - loads SoaSim IR JSON
   - creates Blender mesh from vertices + triangle corner data
   - assigns materials by `triangleSets.materialIndex`

2. `tools/njcm_ir_compare.py`
   - compares SoaSim IR stats vs expected Blender/SAIO stats:
     - triangle counts per attach
     - material run count
     - max index bounds

3. Goldens folder:
   - `planning/NavigationPhase/Resources/NjcmIrGoldens/`
   - store sampled IR JSON for known objects/triggers.

## 4) Detailed implementation sequence

### Phase 0 — Baseline capture

- Capture current outputs for selected MLD files:
  - parse diagnostics
  - triangle totals per object/attach
  - existing Qt screenshots.

Deliverable: baseline report committed under `planning/NavigationPhase/MLDParseLogs`.

### Phase 1 — Introduce IR types + builder (no renderer switch yet)

- Implement `NjWeightedMeshIR` model and `NjcmIrBuilder`.
- Generate IR from current decoded chunks.
- Add JSON exporter and write artifacts for selected samples.

Exit criteria:
- IR mesh counts and triangle totals match current semantic path (within expected intentional differences).

### Phase 2 — Route geometry through IR compatibility layer

- Introduce `IrGeometryBuilder` (or update existing `GeometryBuilder`).
- Keep existing Qt rendering unchanged externally, but feed it via IR conversion.

Exit criteria:
- Render output unchanged from baseline for non-problematic cases.

### Phase 3 — Blender round-trip validator

- Build/commit Blender-side loader script for IR JSON.
- Validate problematic trigger meshes and compare against SonicAdventureBlenderIO view.

Exit criteria:
- Deterministic reproducible diff report between SoaSim IR and Blender result.

### Phase 4 — Tighten parity and diagnostics

- Add material-run parity checks.
- Add cache replay visibility and per-chunk contribution stats.
- Add degenerate/winding diagnostics into UI panel/logs.

Exit criteria:
- Known mismatch cases have explicit diagnostic reason categories.

### Phase 5 — Clean-up / deprecation

- Deprecate direct NJCM semantic-to-render path once IR path is stable.
- Keep raw decode structures for debugging but make IR the canonical consumer interface.

## 5) Required ParseResult/API changes

Proposed `ParseResult` additions:

- `std::optional<model::IrScene> njcmIrScene`
- `std::vector<std::string> njcmIrDiagnostics`
- `std::vector<std::string> njcmIrArtifactPaths`

Proposed parser options additions (`MldParseOptions`):

- `bool buildNjcmIntermediateIr = true`
- `bool exportNjcmIrJson = false`
- `std::string njcmIrOutputDir`

## 6) Validation matrix

For each sample object/attach:

- Vertex count
- Triangle count
- Material run count
- Out-of-range index count
- Degenerate triangle count
- Bounding box min/max

Compare these across:

1. current SoaSim semantic path
2. new SoaSim IR path
3. Blender import of SoaSim IR JSON
4. SonicAdventureBlenderIO/SAIO.NET reference view

## 7) Risks and mitigations

- Risk: material/chunk state is incomplete in current decode structs.
  - Mitigation: extend parity parsers to preserve state transitions in attach metadata.

- Risk: corner-domain attributes may not exist for some chunk types.
  - Mitigation: represent absent UV/color via flags; still emit geometry.

- Risk: switching renderer source obscures regression origin.
  - Mitigation: keep dual-path runtime toggle (`semantic direct` vs `IR derived`) until parity stabilizes.

## 8) Definition of done

Done means all of the following are true:

1. `ParseResult` can emit a deterministic Blender-like NJCM IR.
2. SoaSim Qt renderer can render from IR-derived geometry.
3. IR JSON can be loaded in Blender via tooling for visual parity checks.
4. Existing problematic trigger meshes produce actionable diagnostics explaining mismatch category.
5. Direct non-IR NJCM geometry path is deprecated and no longer default.
