# SA3D C++ Port Ordered Checklist (NJ-only, Level excluded)

Date: 2026-04-18

Decision update: 2026-04-18
- Scope is now locked to NJ-first (`ModelFile` + `AnimationFile` entry paths), CHUNK-focused mesh path,
  and required normalization (`Buffer*`/`Weighted*`), with `LevelFile` + BASIC/GC + SA container branches
  deferred until a later milestone.
- Port order in this document is finalized as the execution order.
- `WriteNJ`/write-side animation work is explicitly deferred for this milestone.
- A mirrored implementation tree is locked under `Sa3Dport` (see Slice 0).

Status legend:
- `[ ]` not started
- `[-]` in progress
- `[x]` complete

---

## Slice 0 — Ground rules and compatibility harness

- [x] Lock NJ-first scope and defer non-goals.
- [x] Freeze source revision to:
  - parser reference repo: `https://github.com/X-Hax/SA3D.Modeling`
  - parser release tag: `1.2.1`
  - parser commit hash: `13813e7`
  - reference-runner fork: `https://github.com/jahorta/SA3D.Modeling/tree/DetailedIO`
  - reference-runner policy: use `DetailedIO` branch and update it per-slice to emit slice-specific input/output pairs.
- [x] Define C++ naming and namespace mapping rule:
  - match C# source naming and namespace hierarchy as closely as possible.
- [x] Create dedicated Visual Studio C++ project `Sa3Dport` for parser implementation.
- [x] Lock mirrored implementation tree under `Sa3Dport`:
  - `File/`
  - `ObjectData/Enums/`
  - `ObjectData/`
  - `Mesh/Chunk/PolyChunks/`
  - `Mesh/Chunk/Structs/`
  - `Mesh/Buffer/`
  - `Mesh/Weighted/`
  - `Animation/Utilities/`
  - `Animation/`
  - `Structs/`
- [x] Build fixture corpus policy:
  - source fixtures are auto-discovered from `SoaSimFileParsing/inputs/*.mld` and all discovered files are included.
  - extraction path is via MLD parser block provider (not direct standalone NJ file loading).
- [ ] Build parity report format (counts, hashes, diagnostics).
- [x] Add A/B CLI mode to `SoaSimFileParsing` for `sa3d_port` (C++) vs `sa3d` (.NET bridge) comparison over discovered fixtures.
- [-] Implement .NET bridge invocation path in A/B mode so `SoaSimFileParsing` executes `sa3d` reference parsing per fixture and emits stable JSON for comparison against `sa3d_port`.
  - Framework scaffold added under `tools/sa3d_ref_runner` with `run-one`/`run-all` commands and `parity_report_v1` JSON shape.

Exit criteria:
- Repeatable fixture runner exists before first ported parser code lands.

---

## Slice 1 — Binary and pointer primitives (fewest dependencies)

Target files/types:
- `File/FileHeaders.cs`
- `Structs/EndianIOExtensions.cs`
- `Structs/PointerLUT.cs`
- `Structs/BAMSFHelper.cs`

Checklist:
- [x] Implement endian-aware primitive reads/writes with image-base semantics.
- [x] Implement pointer LUT behavior for read memoization + write de-dup.
- [x] Port BAMS float/angle conversion behavior.
- [x] Add unit tests for endian stack, pointers, and BAMS exactness.
  - Added `SimCoreTests/test_sa3dport_stage1.cpp` covering file headers, endian read semantics, pointer LUT memoization, and BAMS conversion round-trips.
- [ ] Extend parity harness (Slice 1 mode):
  - compare primitive/lut/bams input/output pairs captured from `DetailedIO` processing of real extracted NJ blocks,
  - emit `parity_report_v1` with `slice_stage = 1` and only `primitives` section populated,
  - skip node/attach/motion sections as `not_applicable`.

Exit criteria:
- Byte-accurate primitive tests passing for representative values.

---

## Slice 2 — NJ container detection and metadata shell

Target files/types:
- `File/NJBlockUtility.cs`
- `File/MetaBlockType.cs`
- `File/Structs/MetaWeight*.cs`
- `File/MetaData.cs` (read paths needed by ModelFile/AnimationFile)

Checklist:
- [ ] Port NJ block scan and block lookup semantics exactly.
- [ ] Port metadata block decode required by animation/model file wrappers.
- [ ] Keep metadata-write path behind feature flag initially.
- [ ] Add block map regression tests from sample NJ files.
- [ ] Extend parity harness (Slice 2 mode):
  - ingest MLD fixtures and extract NJ model/motion block addresses via MLD parser path,
  - compare block map (`offset -> header`) to .NET reference output,
  - compare slice-specific input/output pairs emitted by `DetailedIO` for NJ block + metadata shell operations,
  - emit `parity_report_v1` with `slice_stage = 2` including `block_map` + metadata shell diagnostics.

Exit criteria:
- For fixtures, discovered block addresses match reference outputs.

---

## Slice 3 — Node core object graph

Target files/types:
- `ObjectData/Enums/ModelFormat.cs`
- `ObjectData/Enums/NodeAttributes.cs`
- `ObjectData/Node.cs`
- `ObjectData/Node.Tree.cs`
- `ObjectData/Node.Attributes.cs`
- `ObjectData/Node.Transforms.cs`
- `ObjectData/Node.Enumerate.cs`

Checklist:
- [ ] Port node struct read/write recursion.
- [ ] Port child/next tree linkage invariants.
- [ ] Port transform and quaternion/euler handling.
- [ ] Add graph consistency checks (cycle guard, sibling/parent coherence).
- [ ] Extend parity harness (Slice 3 mode):
  - compare node tree structural metrics (node count, depth, child/next linkage invariants),
  - compare per-node transform/attribute summaries where available,
  - compare slice-specific node-graph input/output pairs emitted by `DetailedIO`,
  - emit `parity_report_v1` with `slice_stage = 3` enabling structural section for nodes.

Exit criteria:
- Node tree round-trip is stable for NJ model fixtures.

---

## Slice 4 — Attach dispatch and CHUNK attach container

Target files/types:
- `Mesh/Attach.cs`
- `Mesh/Chunk/ChunkAttach.cs`
- `Mesh/Chunk/Structs/ChunkVertex.cs`
- `Mesh/Chunk/Structs/ChunkCorner.cs`
- `Mesh/Chunk/Structs/ChunkStrip.cs`

Checklist:
- [ ] Implement attach dispatch limited to CHUNK path for initial scope.
- [ ] Port CHUNK vertex and strip container decoding.
- [ ] Port bounds recompute behavior used by attach.
- [ ] Defer BASIC/GC attach implementations.

Exit criteria:
- CHUNK attach instances parse and serialize for fixture set.

---

## Slice 5 — PolyChunk framework + NJ strip/material behavior

Target files/types:
- `Mesh/Chunk/PolyChunk.cs`
- `Mesh/Chunk/PolyChunkType.cs`
- `Mesh/Chunk/ChunkTypeExtensions.cs`
- `Mesh/Chunk/PolyChunks/*.cs` (NJ relevant subset)

Checklist:
- [ ] Port poly chunk dispatcher and read loop termination semantics.
- [ ] Port `StripChunk` decoding (strip length, reverse winding behavior parity).
- [ ] Port material/texture/bits chunks that affect mesh output segmentation.
- [ ] Keep optional/rare chunks (`VolumeChunk`) behind support matrix.
- [ ] Add per-chunk golden tests (type histogram, triangle totals).

Exit criteria:
- Poly chunk decode parity for known problematic NJ samples.

---

## Slice 6 — ModelFile NJ entry path

Target files/types:
- `File/ModelFile.cs` (NJ-specific branches first)

Checklist:
- [ ] Port `CheckIsModelFile` with NJ detection branch.
- [ ] Port `ReadNJ` path (`GetBlockAddresses`, model block, image base, `Node.Read`).
- [ ] Do not port `WriteNJ` in this milestone (explicit defer).
- [ ] Keep `ReadSA/WriteSA` as out-of-scope stubs or deferred tasks.

Exit criteria:
- `ModelFile.ReadFromBytes` equivalent can parse NJ fixtures and emit stable object/attach counts.

---

## Slice 7 — Animation primitives and Motion graph

Target files/types:
- `Animation/Enums.cs`
- `Animation/Frame.cs`
- `Animation/Keyframes.cs`
- `Animation/NodeMotion.cs`
- `Animation/Utilities/KeyframeRead.cs`
- `Animation/Utilities/KeyframeWrite.cs`
- `Animation/Utilities/KeyframeRotationUtils.cs`
- `Animation/Motion.cs`

Checklist:
- [ ] Port keyframe enum/flags and interpolation handling.
- [ ] Port keyframe read helpers and rotation decoding.
- [ ] Port `Motion.Read` path with LUT semantics.
- [ ] Defer motion write path for this milestone.
- [ ] Validate node-count and short-rot fallback behavior.

Exit criteria:
- Motion parse parity on NJ animation fixtures.

---

## Slice 8 — AnimationFile NJ entry path

Target files/types:
- `File/AnimationFile.cs` (NJ-specific branches first)

Checklist:
- [ ] Port `CheckIsAnimationFile` NJ detection path.
- [ ] Port `ReadNJ` path with required node count guard.
- [ ] Do not port animation write path in this milestone (explicit defer).
- [ ] Keep SA animation versions/features deferred.

Exit criteria:
- `AnimationFile.ReadFromBytes` equivalent matches frame/node summary parity.

---

## Slice 9 — Required SAIO-like normalization path

Target files/types:
- `ObjectData/Node.Attach.cs` (`BufferMeshData` usage)
- `Mesh/Buffer/*.cs`
- `Mesh/Weighted/*.cs`

Checklist:
- [ ] Port minimal buffer mesh conversion needed by consumer.
- [ ] Port weighted mesh creation for parity with SAIO-style downstream consumers.
- [ ] Defer advanced weld/merge features unless parity tests demand them.

Exit criteria:
- C++ path emits weighted/corner stream comparable to SAIO-stage data.

---

## Deferred / explicitly out-of-scope for initial NJ port

- `File/LevelFile.cs` and landtable-specific classes.
- BASIC-only and GC-only attach implementations.
- SA container version compatibility branches not required for NJ block ingest.
- Nonessential exporter/re-writer features until read parity is green.
