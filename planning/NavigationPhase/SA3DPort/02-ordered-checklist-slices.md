# SA3D C++ Port Ordered Checklist (NJ-only, Level excluded)

Date: 2026-04-18

Status legend:
- `[ ]` not started
- `[-]` in progress
- `[x]` complete

---

## Slice 0 — Ground rules and compatibility harness

- [ ] Freeze source revision hashes for `SA3D.Modeling` files in this plan.
- [ ] Define C++ naming and namespace mapping rules from C# source.
- [ ] Build fixture corpus (NJ model + NJ motion files).
- [ ] Build parity report format (counts, hashes, diagnostics).
- [ ] Create toggleable backends (`current parser` vs `sa3d_port`) for A/B.

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
- [ ] Implement endian-aware primitive reads/writes with image-base semantics.
- [ ] Implement pointer LUT behavior for read memoization + write de-dup.
- [ ] Port BAMS float/angle conversion behavior.
- [ ] Add unit tests for endian stack, pointers, and BAMS exactness.

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
- [ ] Port `WriteNJ` path only if write parity is needed in phase 1.
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
- [ ] Port `Motion.Read` and write path with LUT semantics.
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
- [ ] Port write path only when needed by downstream tasks.
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
