# SA3D Port Dependency Graph (ModelFile + AnimationFile, NJ-focused)

Date: 2026-04-18  
Scope: dependencies reachable from `File/ModelFile.cs` and `File/AnimationFile.cs`, excluding `LevelFile.cs` paths and excluding SA1/SA2/BASIC/GC-unique branches unless shared by NJ-CHUNK behavior.

Decision update: 2026-04-18
- Scope above is locked for this milestone.
- Port order constraints in section 4 are finalized as implementation order.
- Write-side NJ/animation paths are not part of this milestone.

---

## 1) Scope filter used

Included:
- `ModelFile` and `AnimationFile` read/write/check paths.
- NJ block discovery (`NJBlockUtility`, `FileHeaders` block constants).
- Node hierarchy read/write (`Node` + partials used by read/write and mesh buffering).
- CHUNK attach + polygon decode path (`ChunkAttach`, `PolyChunk` and relevant poly chunk subclasses).
- Motion read/write path (`Motion`, keyframe readers/writers/utilities).
- Shared binary/lut structs (`EndianIOExtensions`, `PointerLUT`, metadata structs).
- Optional post-read normalization path used by Blender-style ingest (`Node.BufferMeshData`, `WeightedMesh`, `Buffer*`).

Explicitly excluded from this dependency graph:
- `LevelFile.cs` and any landtable-specific types.
- SA1/BASIC-only and SA2B/GC-only mesh/attach implementations not needed for NJ CHUNK pipeline.
- Features only needed for writing SA container variants (kept noted, but not required for NJ-read parity).

---

## 2) Top-level dependency graph

```text
ModelFile
├─ FileHeaders
├─ NJBlockUtility
├─ MetaData
│  ├─ MetaBlockType
│  └─ MetaWeight/MetaWeightNode/MetaWeightVertex
├─ PointerLUT
├─ Node
│  ├─ Node.Tree
│  ├─ Node.Attributes
│  ├─ Node.Transforms
│  ├─ Node.Attach
│  │  ├─ Attach (abstract base)
│  │  │  ├─ ChunkAttach
│  │  │  │  ├─ PolyChunk
│  │  │  │  │  ├─ StripChunk
│  │  │  │  │  ├─ MaterialChunk
│  │  │  │  │  ├─ TextureChunk
│  │  │  │  │  ├─ DrawListChunk / CacheListChunk / BitsChunk variants
│  │  │  │  │  └─ VolumeChunk (if encountered)
│  │  │  │  └─ ChunkVertex / ChunkCorner / ChunkStrip
│  │  └─ Weighted conversion path
│  │     ├─ BufferMesh / BufferVertex / BufferCorner / BufferMaterial
│  │     └─ WeightedMesh / WeightedVertex
│  └─ Node.Enumerate
└─ EndianStackReader/Writer + EndianIOExtensions + BAMSFHelper

AnimationFile
├─ FileHeaders
├─ NJBlockUtility
├─ MetaData
├─ PointerLUT
├─ Motion
│  ├─ NodeMotion
│  ├─ Keyframes
│  ├─ Frame
│  ├─ Animation.Enums
│  └─ Utilities (KeyframeRead / KeyframeWrite / KeyframeRotationUtils)
└─ EndianStackReader/Writer + EndianIOExtensions
```

---

## 3) File-by-file dependencies (within scoped subset)

## A) Entry files

- `File/ModelFile.cs`
  - Depends on: `FileHeaders`, `NJBlockUtility`, `MetaData`, `Node`, `PointerLUT`, metadata structs, texture name list API, reader/writer API.
  - NJ critical branches: `CheckIsNJModelFile`, `ReadNJ`, `WriteNJ`.
- `File/AnimationFile.cs`
  - Depends on: `FileHeaders`, `NJBlockUtility`, `MetaData`, `Motion`, `PointerLUT`, reader/writer API.
  - NJ critical branches: `CheckIsNJAnimFile`, `ReadNJ`.

## B) File container + metadata utilities

- `File/FileHeaders.cs` -> constants for SA/NJ detection and block IDs.
- `File/NJBlockUtility.cs` -> linear NJ block table scan + lookup.
- `File/MetaData.cs` + `MetaBlockType.cs` + `File/Structs/MetaWeight*.cs` -> metadata parsing/writing and weld map payloads.

## C) Object graph core

- `ObjectData/Node.cs` (read/write recursion root).
- `ObjectData/Node.Tree.cs` (hierarchy link operations).
- `ObjectData/Node.Attributes.cs` + `Node.Transforms.cs` (attribute and transform interpretation).
- `ObjectData/Node.Attach.cs` (format conversion + `BufferMeshData` gateway used by SAIO pipeline style).
- `ObjectData/Node.Enumerate.cs` (tree traversal helpers).
- `ObjectData/Enums/ModelFormat.cs`, `NodeAttributes.cs`.

## D) Attach + CHUNK mesh path (NJ-CHUNK focus)

- `Mesh/Attach.cs` (dispatch by model format).
- `Mesh/Chunk/ChunkAttach.cs` (CHUNK attach container).
- `Mesh/Chunk/PolyChunk.cs` + `PolyChunkType.cs` + `ChunkTypeExtensions.cs`.
- Relevant poly chunk subclasses:
  - `StripChunk.cs`
  - `MaterialChunk.cs`
  - `TextureChunk.cs`
  - `BitsChunk.cs`
  - `CacheListChunk.cs`
  - `DrawListChunk.cs`
  - `BlendAlphaChunk.cs`
  - `MaterialBumpChunk.cs`
  - `MipmapDistanceMultiplierChunk.cs`
  - `SpecularExponentChunk.cs`
  - `SizedChunk.cs`
  - `VolumeChunk.cs` (keep as optional, content-dependent)
- CHUNK structs:
  - `ChunkVertex.cs`
  - `ChunkCorner.cs`
  - `ChunkStrip.cs`

## E) Required normalization path for SAIO-style downstream parity

- `Mesh/Buffer/BufferMesh.cs`
- `Mesh/Buffer/BufferVertex.cs`
- `Mesh/Buffer/BufferCorner.cs`
- `Mesh/Buffer/BufferMaterial.cs`
- `Mesh/Weighted/WeightedMesh.cs`
- `Mesh/Weighted/WeightedVertex.cs`

Port this alongside core parser work; do not defer in NJ-first scope.

## F) Animation stack

- `Animation/Motion.cs`
- `Animation/NodeMotion.cs`
- `Animation/Keyframes.cs`
- `Animation/Frame.cs`
- `Animation/Enums.cs`
- `Animation/Utilities/KeyframeRead.cs`
- `Animation/Utilities/KeyframeWrite.cs`
- `Animation/Utilities/KeyframeRotationUtils.cs`

## G) Shared binary/lookup helpers

- `Structs/EndianIOExtensions.cs`
- `Structs/PointerLUT.cs`
- `Structs/BAMSFHelper.cs`

---

## 4) Port ordering constraints implied by graph

1. Binary reader/writer semantics + constants must be stable first.
2. NJ block scanner before any NJ file reader.
3. LUT/label/pointer policy before Node/Attach recursion.
4. Node core before Attach dispatch.
5. Attach dispatch before CHUNK container/polychunk decode.
6. CHUNK chunk structs before strip/material chunk implementations.
7. Motion/keyframe primitives before `AnimationFile.ReadNJ`.
8. Buffer/Weighted conversion is in-scope and should be ported alongside CHUNK parity work.

---

## 5) Notes on SA1/SA2 exclusion strategy

- We are intentionally not planning BASIC-attach and GC-attach classes.
- For `ModelFile.ReadNJ`, the runtime still distinguishes BM vs CM blocks; this port plan targets CM/CHUNK first and treats BM/BASIC as out-of-scope fallback.
- SA container (`ReadSA` / `WriteSA`) paths are documented but marked non-goal for initial NJ-focused parity.
