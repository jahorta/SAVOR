# SA3D Source Inventory for NJ Port

Date: 2026-04-18

This inventory is the concrete file set used to build the dependency graph/checklist/function scope docs.

---

## Included source groups

## Entry points
- `File/ModelFile.cs`
- `File/AnimationFile.cs`

## File utilities and metadata
- `File/FileHeaders.cs`
- `File/NJBlockUtility.cs`
- `File/MetaData.cs`
- `File/MetaBlockType.cs`
- `File/Structs/MetaWeight.cs`
- `File/Structs/MetaWeightNode.cs`
- `File/Structs/MetaWeightVertex.cs`

## Object graph
- `ObjectData/Node.cs`
- `ObjectData/Node.Tree.cs`
- `ObjectData/Node.Attach.cs`
- `ObjectData/Node.Attributes.cs`
- `ObjectData/Node.Transforms.cs`
- `ObjectData/Node.Enumerate.cs`
- `ObjectData/Enums/ModelFormat.cs`
- `ObjectData/Enums/NodeAttributes.cs`

## Mesh / CHUNK path
- `Mesh/Attach.cs`
- `Mesh/Chunk/ChunkAttach.cs`
- `Mesh/Chunk/PolyChunk.cs`
- `Mesh/Chunk/PolyChunkType.cs`
- `Mesh/Chunk/ChunkTypeExtensions.cs`
- `Mesh/Chunk/PolyChunks/BitsChunk.cs`
- `Mesh/Chunk/PolyChunks/BlendAlphaChunk.cs`
- `Mesh/Chunk/PolyChunks/CacheListChunk.cs`
- `Mesh/Chunk/PolyChunks/DrawListChunk.cs`
- `Mesh/Chunk/PolyChunks/MaterialBumpChunk.cs`
- `Mesh/Chunk/PolyChunks/MaterialChunk.cs`
- `Mesh/Chunk/PolyChunks/MipmapDistanceMultiplierChunk.cs`
- `Mesh/Chunk/PolyChunks/SizedChunk.cs`
- `Mesh/Chunk/PolyChunks/SpecularExponentChunk.cs`
- `Mesh/Chunk/PolyChunks/StripChunk.cs`
- `Mesh/Chunk/PolyChunks/TextureChunk.cs`
- `Mesh/Chunk/PolyChunks/VolumeChunk.cs`
- `Mesh/Chunk/Structs/ChunkCorner.cs`
- `Mesh/Chunk/Structs/ChunkStrip.cs`
- `Mesh/Chunk/Structs/ChunkVertex.cs`

## Required normalized mesh path (in-scope)
- `Mesh/Buffer/BufferMesh.cs`
- `Mesh/Buffer/BufferCorner.cs`
- `Mesh/Buffer/BufferVertex.cs`
- `Mesh/Buffer/BufferMaterial.cs`
- `Mesh/Weighted/WeightedMesh.cs`
- `Mesh/Weighted/WeightedVertex.cs`

## Animation internals
- `Animation/Motion.cs`
- `Animation/Keyframes.cs`
- `Animation/Frame.cs`
- `Animation/NodeMotion.cs`
- `Animation/Enums.cs`
- `Animation/Utilities/KeyframeRead.cs`
- `Animation/Utilities/KeyframeWrite.cs`
- `Animation/Utilities/KeyframeRotationUtils.cs`

## Shared structs
- `Structs/EndianIOExtensions.cs`
- `Structs/PointerLUT.cs`
- `Structs/BAMSFHelper.cs`

---

## Explicitly excluded

- `File/LevelFile.cs`
- Landtable and level-only object paths
- BASIC/GC attach families as first-pass targets
- SA container branches not needed for NJ-first milestone

---

## Notes for future expansion

If future requirements include SA container support or level parsing, add:
- `File/LevelFile.cs`
- Landtable classes
- BASIC and GC mesh/attach branches
- full SA metadata version compatibility matrix
