# Function Port Scope by File (NJ blocks only)

Date: 2026-04-18  
Goal: list functions to port first, constrained to:
- anything touched by `ModelFile.cs` / `AnimationFile.cs`
- no `LevelFile.cs`
- prioritize NJ behavior
- ignore SA1/SA2/BASIC/GC-unique behaviors unless they are unavoidable shared plumbing

Decision update: 2026-04-18
- Write-side APIs are deferred (`WriteNJ`, motion/animation write helpers).
- Read/parse parity is the only milestone target in this phase.

---

## 1) `File/ModelFile.cs`

## Port now (NJ path)
- `CheckIsModelFile(...)` overloads (through NJ detection branch).
- `CheckIsNJModelFile(...)`.
- `ReadFromFile`, `ReadFromBytes`, `Read(...)` wrappers.
- `ReadNJ(...)`.

## Shared helpers to port if using writes/metadata
- `CreateWeldings(...)` (only if metadata weld import is consumed).
- `CreateMetaWeights(...)` (only for write parity).

## Defer / ignore for NJ-first milestone
- `CheckIsSAModelFile(...)`.
- `ReadSA(...)`, `WriteSA(...)`.
- `WriteNJ(...)`.

---

## 2) `File/AnimationFile.cs`

## Port now (NJ path)
- `CheckIsAnimationFile(...)` overloads (through NJ detection branch).
- `CheckIsNJAnimFile(...)`.
- `ReadFromFile`, `ReadFromBytes`, `Read(...)` wrappers.
- `ReadNJ(...)`.

## Likely needed anyway
- `Write(...)` is deferred until an explicit round-trip milestone.

## Defer / ignore for NJ-first milestone
- `CheckIsSAAnimFile(...)`.
- `ReadSA(...)` and SA version handling branches.
- write-side animation entry and helpers.

---

## 3) `File/NJBlockUtility.cs`

## Port now
- `GetBlockAddresses(...)`.
- `FindBlockAddress(Dictionary<...>, ...)`.
- `FindBlockAddress(EndianStackReader, ...)`.

Rationale: both `ModelFile.ReadNJ` and `AnimationFile.ReadNJ` depend on block discovery.

---

## 4) `File/FileHeaders.cs`

## Port now (subset)
- NJ block constants used by `ModelFile` and `AnimationFile` NJ branches:
  - `NJ`, block masks/headers for model and animation blocks.
  - sets: `ModelBlockHeaders`, `AnimationBlockHeaders`, and related constants.

## Defer
- SA-only header constants not required by NJ parse path.

---

## 5) `ObjectData/Node.cs` (+ required partials)

## Port now
- `Node.Read(...)` recursion (including attach read dispatch).
- `Node.Write(...)` only if write parity is required.
- Tree/link methods required by recursion correctness:
  - `SetChild`, `SetNext`, parent/sibling wiring from `Node.Tree.cs`.
- Transform update helpers from `Node.Transforms.cs` used during read.
- Attribute setters from `Node.Attributes.cs` used in `Node.Read`.

## Defer
- clone/copy convenience methods not needed for initial parser parity.

---

## 6) `Mesh/Attach.cs`

## Port now (NJ-CHUNK path only)
- `Attach.Read(...)` dispatch.
- CHUNK branch behavior.
- `CanWrite(...)`/`Write(...)` only if write parity is in milestone.

## Defer
- BASIC and GC dispatch implementations.
- Buffer attach write path unless needed for downstream conversion.

---

## 7) `Mesh/Chunk/ChunkAttach.cs`

## Port now
- `Read(...)`.
- `WriteInternal(...)` (if writing needed).
- `RecalculateBounds(...)` (if bounds are consumed by rendering/diagnostics).

## Defer
- convenience helpers not used by initial decode parity (e.g., broad model traversal helpers).

---

## 8) `Mesh/Chunk/PolyChunk.cs` + `PolyChunkType.cs` + `ChunkTypeExtensions.cs`

## Port now
- `PolyChunk.Read(...)` dispatcher.
- `PolyChunk.ReadArray(...)`.
- type enum values and termination semantics.

## Defer
- writing helpers until write parity is requested.

---

## 9) `Mesh/Chunk/PolyChunks/*` (NJ-relevant subset)

## Port now (priority order)
1. `StripChunk` (core primitive generation).
2. `MaterialChunk` and `TextureChunk` (material/texture state for segmentation).
3. `DrawListChunk` + `CacheListChunk` + base `BitsChunk` (cache/draw semantics).
4. `BlendAlphaChunk`, `MaterialBumpChunk`, `MipmapDistanceMultiplierChunk`, `SpecularExponentChunk`, `SizedChunk`.

## Keep optional
- `VolumeChunk` unless seen in active fixture corpus.

---

## 10) `Mesh/Chunk/Structs/*`

## Port now
- `ChunkVertex`.
- `ChunkCorner`.
- `ChunkStrip`.

Rationale: these back the strip/poly decode core.

---

## 11) Animation core (`Animation/*`)

## Port now
- `Motion.Read(...)` and dependent shape:
  - `NodeMotion`
  - `Keyframes`
  - `Frame`
  - `Enums` flags/interpolation
  - `KeyframeRead` + `KeyframeRotationUtils`

## Defer
- optimization and write-side helpers (`Optimize`, advanced writes) until needed.

---

## 12) Shared infrastructure

## Port now
- `PointerLUT` read path behavior (memoization + labels).
- endian + primitive IO semantics from `EndianIOExtensions`.
- BAMS conversion helper needed by transforms/keyframes.

## Defer
- debug/utility helpers not used by parser path.

---

## 13) Required normalization path for SAIO-like parity

Port in initial scope for parity against Blender-side intermediate representation:
- `Node.BufferMeshData(...)` from `Node.Attach.cs`.
- `BufferMesh`, `BufferVertex`, `BufferCorner`, `BufferMaterial`.
- `WeightedMesh`, `WeightedVertex`.

Do not defer this to phase-2; include it in the main NJ-first port sequence.
