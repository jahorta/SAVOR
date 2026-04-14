# Blender IR JSON Readiness + Blender Plugin Reconstruction Plan

## Scope
This document assesses whether `blender_ir_scene.json` is sufficient as a **standalone artifact** to reconstruct a scene in Blender, and outlines a first-pass Blender Python importer plan (no implementation yet).

## What the JSON currently contains

Based on `BlenderIrModel`, the exported scene contains:

- `meshes[]` with:
  - identity/provenance (`label`, `sourceObjectAddress`, chunk/attach offsets)
  - `vertices[]` with `position` + `hasPosition`
  - `materials[]` with poly/chunk flags, texture metadata, and hashes
  - `triangleSets[]` with `materialIndex`, `polyType`, and `corners` (currently vertex indices)
  - per-mesh diagnostics (degenerate/out-of-range/cache-replay counts)
- `indexEntries[]` with instance metadata and transform (`position`, `rotationRaw`, quaternion `rotation`, `scale`), plus object-address and mesh-index links
- `textures[]` with optional decoded RGBA8 payload and encoded source payload, both base64
- scene-level diagnostics

## Is JSON standalone enough to build a Blender scene?

### Yes, for a useful **geometry + transforms + basic materials/textures** import

The JSON has enough to:

1. Build mesh geometry from vertex positions and triangle index triplets.
2. Build object instances from `indexEntries[].meshIndices` and per-entry transforms.
3. Create material slots and associate triangle sets with material indices.
4. Decode embedded textures from `pixelDataBase64` (`rgba8`) and attach to materials.

### Not yet, for a **full-fidelity** reconstruction

The model schema supports richer data (normals/UV/colors/weights), but the current exporter/builder path does not fully emit/populate it:

- Vertex normals and skin weights exist in the C++ model but are not exported in JSON vertices today.
- Corner UV/color flags exist in the C++ model but triangle-set export currently writes only corner `vertexIndex` values.
- Builder currently fills `position`/`hasPosition` for vertices and triangles from polygon indices, but does not populate corner UV/color or vertex normals/weights in the shown path.

Result: the JSON is standalone for **playable/inspectable scene reconstruction**, but not sufficient for final shading/animation fidelity without extending export content.

## Blender importer plan (Python, no code yet)

## 0) Add-on skeleton + operator

- Add Blender add-on metadata (`bl_info`), registration, and a file import operator (JSON file picker).
- Expose import options:
  - clear current scene / import into collection
  - create one object per mesh vs instantiate per index entry
  - prefer quaternion rotation over `rotationRaw`
  - optional diagnostic report panel

## 1) Parse + validate JSON

- Load JSON and validate required top-level keys: `meshes`, `indexEntries`, `textures`.
- Validate cross-references:
  - `triangleSet.materialIndex` in range of mesh materials
  - each corner vertex index in range of mesh vertices
  - `indexEntries.meshIndices` in range of scene meshes
- Surface warnings from `diagnostics` and local validation in a summary report.

## 2) Build texture library

- For each `textures[]` record:
  - if `pixelFormat == "rgba8"` and payload size == `width*height*4`, decode base64 and create `bpy.data.images.new(...)` then assign pixel buffer.
  - preserve metadata (name/source offset/format) as custom properties.
  - if RGBA payload missing/invalid, keep placeholder and record warning.
- Keep a `textureName -> image` map for material binding.

## 3) Build mesh datablocks

Per `mesh`:

- Create a Blender mesh data-block named from `label`.
- Build vertices from `vertices[].position`.
- Reconstruct faces from each `triangleSet.corners` in triplets.
- Create material slots in the same order as JSON `materials[]`.
- Assign face material index according to owning `triangleSet.materialIndex`.
- Write provenance + diagnostics as mesh custom properties.

Notes:

- No UV layer creation in v1 unless corner UVs are exported later.
- No vertex normals import in v1 unless exported later.

## 4) Build Blender materials

For each JSON material:

- Create/reuse a Blender material keyed by `materialHash`.
- Set principled defaults (base color/roughness), stash poly/chunk flags in custom props.
- If `textureName` resolves to an image, create an Image Texture node and connect to base color.

## 5) Build scene objects/instances

- Strategy A (recommended):
  - Create one hidden source object per mesh data-block.
  - For each `indexEntry`, instance linked objects for each referenced `meshIndex`.
- Apply transform:
  - location <- `transform.position`
  - rotation <- quaternion `transform.rotation` (document axis convention assumptions)
  - scale <- `transform.scale`
- Name instances with entry ID/fxnName for traceability.
- Store `sourceEntryId`, `tblId`, `fxnName`, `objectAddresses` as custom properties.

## 6) Coordinate/system handling

- Add importer options for axis conversion and uniform scale to align SoaSim coordinates with Blender.
- Keep defaults conservative and reversible.
- Record applied conversion in scene custom properties.

## 7) Diagnostics + QA pass

- Print import summary:
  - mesh count, instance count, missing textures, invalid triangles skipped
- Optionally create a text datablock report in Blender.
- Flag meshes with high degenerates/out-of-range counts.

## 8) Future-ready extensions (once JSON grows)

- UV support: import per-corner UVs into loop UV layers.
- Vertex colors: import to color attributes.
- Normals: import custom split normals.
- Skinning: create vertex groups from `weights[]` and optional armature binding.
- Better material mapping by `polyType/chunkFlags` semantics.

## Recommended immediate next steps in SoaSimMLD before Blender coding

1. Decide target fidelity for v1 importer (preview-quality vs production-quality).
2. If preview-quality is enough, proceed with importer now against current JSON.
3. If production-quality is desired, first extend exporter to include:
   - vertex normals
   - triangle corner UVs/colors (not just vertex indices)
   - any additional material state needed for shading parity
4. Add a small golden JSON fixture + expected import counts for regression checks.
