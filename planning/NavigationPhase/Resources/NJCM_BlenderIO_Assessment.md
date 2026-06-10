# NJCM parsing/rendering assessment against SonicAdventureBlenderIO

Date: 2026-04-13

## Scope

This note maps:
1. SonicAdventureBlenderIO import data path (file import -> parse/process -> Blender mesh objects).
2. Where NJ chunk parsing logic actually lives (plugin vs external libraries).
3. A concrete "closer parity" plan for SAVOR NJCM rendering.

## SonicAdventureBlenderIO data path (model import -> Blender)

### 1) Blender operator entry point

- Model import starts in `SAIO_OT_Import_Model._execute` in `import_operators.py`.
- It loads the .NET bridge (`load_dotnet()`), then calls:
  - `SAIO_NET.MODEL.Import(filepath, optimize, flip_vertex_colors)`.
- The resulting `import_data` is passed to `NodeProcessor.process_model(...)`.

Implication: Python side does orchestration; file parsing and mesh semantics are delegated to .NET (`SAIO.NET`).

Relevant upstream files:
- `blender/source/register/operators/import_operators.py`
- `blender/source/dotnet/__init__.py`

### 2) .NET bridge loading and external dependency boundary

- `load_dotnet()` bootstraps pythonnet coreclr runtime and loads DLLs:
  - `SAIO.NET.dll`
  - `SA3D.Archival.dll`
  - `SA3D.SA2Event.dll`
  - `TextCopy.dll`
- `SAIO.NET.csproj` references NuGet packages, especially:
  - `SA3D.Modeling`
  - `SA3D.Modeling.JSON`
  - `SA3D.SA2Event`

Implication: parsing/format logic is not purely built-in Python; it heavily relies on external .NET assemblies and SA3D packages.

Relevant upstream files:
- `blender/source/dotnet/__init__.py`
- `dotnet/SAIO.NET.csproj`

### 3) File decode and model preprocessing in SAIO.NET

- `Model.Import` calls `ModelFile.ReadFromFile(filepath)` and then `Process(...)`.
- `Process(...)` performs:
  - `node.BufferMeshData(optimize)`
  - `WeightedMesh.FromModel(node, BufferMode.None)`
  - optional color channel flips
  - optional merge path for non-weighted meshes (`WeightedMesh.MergeAtRoots`)
- Data returned to Python contains:
  - node hierarchy (`Root`)
  - preprocessed weighted meshes (`Attaches`)
  - metadata/texture names

Implication: by the time Python sees mesh data, topology/material corner streams are already normalized into `WeightedMesh`/`BufferCorner` style structures.

Relevant upstream files:
- `dotnet/Model.cs`
- `dotnet/Structs.cs`

### 4) Python conversion to Blender-native geometry

- `NodeProcessor.process(...)` calls `MeshProcessor.process_multiple(import_data.Attaches, ...)`.
- `MeshProcessor` converts each weighted buffer to Blender arrays:
  - vertices: from `vert.Position` with axis mapping `(x, -z, y)`
  - normals: same axis mapping
  - cornersets (`TriangleSets`) expanded into triangle index triples
  - UV transform: `(u, 1-v)`
  - per-corner colors converted to linear
- Blender mesh creation:
  - `mesh.from_pydata(vertices, [], polygons)`
  - split normals via `normals_split_custom_set_from_vertices`
  - UV/color layers created in Blender corner domain
  - material slots assigned by cornerset length partitioning

Implication: final format used for render is standard Blender `Mesh` with already-triangulated polygon stream, plus per-corner UV/color and per-vertex normals.

Relevant upstream files:
- `blender/source/importing/i_mesh.py`
- `blender/source/importing/i_node.py`
- `blender/source/importing/i_matrix.py`

## Does SonicAdventureBlenderIO use external libraries?

Short answer: **yes, extensively**.

- Python addon side is mostly adapter/glue.
- Actual model/level parse and chunk semantics are handled by:
  - `SAIO.NET.dll`
  - `SA3D.*` assemblies (`SA3D.Modeling`, `SA3D.Archival`, etc.)
- Runtime hosting uses `pythonnet` and .NET coreclr.

So parsing is **not** fully built into the Python plugin.

## How this compares to current SAVOR path

Current SAVOR NJCM flow (parity branch) is:

- `decodeNjcmChunkSaToolsParity(...)` -> `satools_parity::decodeWithObjectModel(...)`.
- Object/attach traversal happens in C++ over decoded NJCM bytes.
- Poly parsing handles cache chunks with replay (`Bits_CachePolygonList`/`Bits_DrawPolygonList`) and emits semantic polygons/primitives.
- `GeometryBuilder` converts attach semantic vertices/polys to `GeometryObject`.
- Qt renderer consumes prebuilt vertex/index buffers (`QQuick3DGeometry`) as triangles/lines.

Key difference versus Blender path:
- Blender pipeline receives a higher-level "weighted mesh corner stream" representation after SA3D processing.
- SAVOR currently works closer to raw NJCM records and builds triangles directly from your own semantic decode.

Relevant local files:
- `SPICE MLD/Parsing/NJCMParityPath.cpp`
- `SPICE MLD/Parsing/SaToolsParityParser.cpp`
- `SPICE MLD/Parsing/SaToolsParityPolyParser.cpp`
- `SPICE MLD/Parsing/GeometryBuilder.cpp`
- `SavorQt3D/GUI/RuntimeSceneConverter.cpp`
- `SavorQt3D/GUI/StaticMeshGeometry.cpp`

## Recommendations to move SAVOR rendering closer to Blender/SA3D behavior

1. **Introduce a "WeightedBuffer-like" intermediate format in SAVOR**
   - Match SAIO concepts: per-vertex position/normal/weights + per-corner UV/color + per-material triangle sets.
   - Keep this as the sole handoff to renderer and diagnostic tools.

2. **Separate polygon decode from triangulation/material batching**
   - Stage A: decode NJ chunks and preserve chunk semantics/events.
   - Stage B: normalize to corner-based triangle sets per material.
   - Stage C: renderer consumes only normalized triangle sets.
   - This mirrors how Blender receives already-normalized weighted buffers.

3. **Adopt parity transforms explicitly and centrally**
   - Enforce axis conversion and UV flip in one canonical place (equivalent to `i_mesh.py` + `i_matrix.py` behavior).
   - Add a toggle to inspect pre/post transform data.

4. **Record mesh partition metadata at build time**
   - Keep per-material triangle-run lengths (like `poly_material_lengths`) so both render and debug tools can verify segmentation.

5. **Add a debug export path to compare with Blender-side inputs**
   - Emit JSON of normalized vertices/corners/material runs from SAVOR.
   - Optionally write a converter script to feed that into Blender directly for A/B visual checks.

6. **Improve parity diagnostics where your screenshots suggest issues**
   - Track and report: thin/degenerate triangles, winding flips, duplicate/replayed cache chunks, and outlier normal vectors.
   - Attach-object labels already exist; expand diagnostics per attach and per chunk type.

## Commands used for this assessment

- `rg --files -g 'AGENTS.md'`
- `rg -n "Njcm|NJCM|sa_tools|Object block|object16|chunk model|chunk mesh" --glob '!third-party/**'`
- `curl -L https://api.github.com/repos/X-Hax/SonicAdventureBlenderIO/git/trees/main?recursive=1`
- `curl -L https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/...` (multiple files)
- `sed -n ...` for local SAVOR parsing/rendering files

## External source URLs inspected

- https://github.com/X-Hax/SonicAdventureBlenderIO
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/blender/source/register/operators/import_operators.py
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/blender/source/dotnet/__init__.py
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/dotnet/SAIO.NET.csproj
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/dotnet/Model.cs
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/dotnet/Structs.cs
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/blender/source/importing/i_mesh.py
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/blender/source/importing/i_node.py
- https://raw.githubusercontent.com/X-Hax/SonicAdventureBlenderIO/main/blender/source/importing/i_matrix.py

## Exact call path and DLL API endpoints (function-level trace)

This section is an explicit call-by-call trace for the model-import path used by the Blender addon.

### A) Python entry and bridge loading

1. `SAIO_OT_Import_Model._execute(context)`
2. `load_dotnet()`
   1. `pythonnet.load("coreclr", runtime_config=...)`
   2. `clr.AddReference(...)` for:
      - `SAIO.NET.dll`
      - `SA3D.Archival.dll`
      - `SA3D.SA2Event.dll`
      - `TextCopy.dll`
   3. `SAIO_NET.load()` -> binds managed types from namespace `SAIO.NET`.
3. `SAIO_NET.MODEL.Import(filepath, optimize, flip_vertex_colors)`

### B) Managed (.NET) import path in SAIO.NET.dll

1. `SAIO.NET.Model.Import(string filepath, bool optimize, bool flipVertexColors)`
2. `SA3D.Modeling.File.ModelFile.ReadFromFile(filepath)`
3. `SAIO.NET.Model.Process(Node node, TextureNameList? textureNames, bool optimize, ... )`
4. `node.BufferMeshData(optimize)`
5. `SA3D.Modeling.Mesh.Weighted.WeightedMesh.FromModel(node, BufferMode.None)`
6. Optional post-processing in `Process`:
   - non-weighted merge: `WeightedMesh.MergeAtRoots(meshes)`
   - vertex color channel flip on `BufferCorner` streams
7. Return `SAIO.NET.Model` object with:
   - `Root` (`SA3D.Modeling.ObjectData.Node`)
   - `Attaches` (`SA3D.Modeling.Mesh.Weighted.WeightedMesh[]`)
   - metadata / texture names

### C) Python conversion to Blender-native mesh

1. `NodeProcessor.process_model(...)`
2. `NodeProcessor.process(import_data, ...)`
3. `MeshProcessor.process_multiple(import_data.Attaches, ...)`
4. For each `WeightedMesh`:
   - `MeshProcessor.process(...)`
   - `_process_vertices()` reads `weighted_buffer.Vertices[*].Position/Normal/Weights`
   - `_process_polygons()` reads `weighted_buffer.TriangleSets` (`BufferCorner` triplets)
   - `_process_materials()` maps `weighted_buffer.Materials`
   - `_create_mesh()` -> `bpy.data.meshes.new` + `mesh.from_pydata(...)`
   - `_setup_mesh_normals()` -> `normals_split_custom_set_from_vertices(...)`
   - `_setup_mesh_uvs()` / `_setup_mesh_colors()`
5. `NodeProcessor` then instantiates Blender objects / armature objects and assigns transformed meshes.

### D) DLL/API endpoint list directly invoked by the addon

#### 1) Endpoints called from Python into SAIO.NET (managed API surface)

- `SAIO.NET.Model.Import(...)`
- `SAIO.NET.Model.Process(...)` (landtable animation path)
- `SAIO.NET.LandTableWrapper.Import(...)` (level import path)
- `SAIO.NET.*` type access via pythonnet dynamic binding in `saio_net.py`

#### 2) Endpoints SAIO.NET then calls in external SA3D libraries

- `SA3D.Modeling.File.ModelFile.ReadFromFile(...)`
- `SA3D.Modeling.File.LevelFile.ReadFromFile(...)` (landtable path)
- `SA3D.Modeling.ObjectData.Node.BufferMeshData(...)`
- `SA3D.Modeling.Mesh.Weighted.WeightedMesh.FromModel(...)`
- `SA3D.Modeling.Mesh.Weighted.WeightedMesh.MergeAtRoots(...)`
- `SA3D.Modeling.Mesh.Weighted.WeightedMesh.ToModel(...)` (debug/rebuild path)
- `SA3D.Modeling.Mesh.Attach / AttachFormat` conversion helpers (various export/import paths)

### E) Practical interpretation for NJCM/object-block parity

For your specific parity case (old sa_tools object-block extraction): the Blender addon is effectively consuming the already-parsed SA model representation returned by SA3D/SAIO.NET, then performing coordinate/material/corner mapping into Blender mesh data. In other words, NJ chunk semantics are resolved before `i_mesh.py` starts creating Blender geometry.

## Internal path in `jahorta/sa_tools` (`MLDDetailedOutput`) for model-file read

Note: this branch does not expose `SA3D.Modeling.File.ModelFile.ReadFromFile(...)` by that exact namespace/signature; the equivalent read path is `new SAModel.ModelFile(filename)` (or `new SAModel.ModelFile(byte[], filename)`).

### Internal call flow (equivalent of read-from-file)

1. `new ModelFile(string filename)`
   - Calls `File.ReadAllBytes(filename)`
   - Delegates to `new ModelFile(byte[] file, string filename)`
2. `ModelFile(byte[] file, string filename)`
   - Reads header magic/version.
   - Parses metadata chunks (`Label`, `Animation`, `Author`, `Description`, `Weights`, etc.).
   - Resolves `ModelFormat` from magic (`SA1MDL`, `SA2MDL`, `SA2BMDL`, `XJMDL`).
   - Builds root node graph via:
     - `new NJS_OBJECT(file, modelPointer, imageBase, format, labels, attaches)`
3. `NJS_OBJECT` recursive parse
   - Reads object flags, transform, child/sibling pointers.
   - For non-null attach pointer, calls:
     - `Attach.Load(file, attachPtr, imageBase, format, labels)`
4. Attach dispatch (format-specific)
   - `ModelFormat.Chunk` -> `new ChunkAttach(file, address, imageBase, labels)`
   - `ChunkAttach` then parses:
     - vertex chunk list (`new VertexChunk(file, tmpaddr)` loop until `ChunkType.End`)
     - polygon chunk list (`PolyChunk.Load(file, tmpaddr)` loop until `ChunkType.End`)
5. Optional weight metadata application
   - If `Weights` chunk exists, maps node labels -> attaches and populates `Attach.VertexWeights`.
6. Optional animation file loading (if filename is provided)
   - Loads listed animations into `Animations` (binary motions or JSON motions).

### What is returned / resulting object state

The result is a fully-populated `SAModel.ModelFile` instance containing:

- `Format` (`ModelFormat.Basic|Chunk|GC|XJ`)
- `Model` (root `NJS_OBJECT` tree)
- `Animations` (`ReadOnlyCollection<NJS_MOTION>`)
- `Author` / `Description`
- `Metadata` (`Dictionary<uint, byte[]>` for unhandled chunk payloads)
- internal `animationFiles` backing array

### Mapping to the SAIO.NET/SA3D call in the Blender addon

In the Blender addon path we traced earlier, Python calls `SAIO.NET.Model.Import(...)`, which calls `SA3D.Modeling.File.ModelFile.ReadFromFile(filepath)`. In this `sa_tools` branch, the closest equivalent implementation detail is the `SAModel.ModelFile` constructor flow above. Conceptually both produce a model-file object containing parsed model hierarchy + side metadata before later conversion to weighted/buffered mesh structures.
