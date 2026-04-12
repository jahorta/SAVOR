# GameCube MLD Parsing and Qt Viewer Plan

## Purpose

This document plans the first implementation pass for parsing **GameCube Skies of Arcadia Legends MLD files** and transforming their contents into a format that can be displayed in a Qt-based 3D viewer for SOASim's Navigation Phase.

This document is intentionally scoped to **GameCube / Legends only**. It does not attempt to generalize across Dreamcast or other Sonic Adventure-family Ninja dialects except where comparison helps clarify likely behavior.

It also separates three categories of knowledge:

- **Confirmed from SOASim NavigationPhase planning docs**
- **Confirmed from the existing C# `SoAMLDs` research code**
- **Open items that still require validation during implementation**

## Scope

The initial implementation should support the following:

- Parse GameCube Legends MLD data after extraction from the disc through Dolphin-integrated file access. [#1] [#2]
- Build a viewer-ready representation of walkable space from **GRND** objects.
- Build connectivity between GRND objects using **ground_links**.
- Parse entry records that dispatch by **`fxn`** and convert the subset we understand into collision, trigger, and debug-view objects.
- Convert parsed geometry and transforms into an engine-neutral world representation in C++.
- Adapt that world representation into Qt scene objects for visualization.
- Keep unknown or partially understood entries visible in the viewer rather than dropping them silently.

The initial implementation does **not** need to support:

- Placement files external to the MLD. Placement is already embedded in each MLD entry, per project notes.
- Full visual fidelity.
- Every handler type.
- Every possible GameCube chunk or object class.
- Other platform-specific archive variants.

## Confirmed constraints

### Confirmed from project notes

The following constraints are treated as project requirements:

1. We only care about **GameCube (Legends)** archives.
2. **GRND** objects represent the ground collision mesh.
3. **ground_links** link GRND objects into a complete walking mesh.
4. Collision and trigger entries are interpreted by handlers selected from an entry's **`fxn`** field.
5. Placement files are not required because placement is already embedded in the MLD entry data.
6. MLD files can be extracted directly from the disc by patching into Dolphin, as outlined in the NavigationPhase planning docs.
7. This planning resource should live in `planning/NavigationPhase/Resources`.

### Confirmed from NavigationPhase docs

The existing NavigationPhase planning documents establish several requirements and assumptions for the world model:

- The Navigation Phase needs a true 3D world representation rather than a simplified 2D tile abstraction, especially to preserve overlapping vertical layers and transitions. [#1]
- GRND and GRND_Link data are expected to be central to constructing the walkable world model. [#1] [#2]
- The phase needs collision and trigger modeling, but an early pass may use coarse approximations while reverse engineering continues. [#1] [#2]
- A viewer is part of the intended workflow so that reconstructed geometry, transitions, and route overlays can be visually inspected. [#1]
- The current extraction direction is to obtain files through Dolphin/DiscIO rather than maintaining a separate external extractor pipeline. [#2]

### Confirmed from the C# `SoAMLDs` codebase

The existing C# research code is the best current reference for how to interpret relevant binary structures. What is clearly supported by the visible code includes:

- A Ninja-style chunk reader that recognizes chunk IDs such as `NJCM`, `GJCM`, `NJTL`, `GJTL`, and `POF0`. [#3] [#4] [#5]
- A pointer-fixup path where `POF0` data is applied to the preceding chunk payload. [#3] [#4]
- A note in the reader that GameCube chunk sizes may need mixed-endian handling in practice. [#3]
- Logic for generating and interpreting POF0 pointer-delta streams. [#4]

However, one important correction is needed here:

The current publicly visible `NJReader` code is clearly useful for **Ninja chunk mechanics**, but it should **not** be treated as proof that the entire Legends MLD container is just a generic Ninja chunk stream. In fact, the code comments indicate that some Skies-specific chunk IDs such as `GRND` and `GOBJ` are not handled in that generic reader path. [#3]

That means the implementation plan should treat:

- **Ninja chunk parsing** as one reusable parsing subsystem, and
- **MLD container / GRND / entry handling** as a separate, higher-level subsystem that still needs to be mirrored from the dedicated MLD logic in your C# research.

## High-level architecture

The correct architectural split is:

```text
Disc / Dolphin extraction
  -> raw MLD bytes
  -> MLD parser core
  -> engine-neutral WorldModel / NavWorldIR
  -> Qt adapter
  -> Qt viewer scene
```

This separation is important because the viewer should not know anything about raw binary parsing, pointer fixups, chunk endianness, or MLD-specific handler details.

The parser should produce a reusable C++ representation that can later support:

- visualization,
- nav graph generation,
- trigger simulation,
- route analysis,
- serialization into later NavigationPhase artifacts.

## Proposed module split

### `MldCore`

Pure C++ parsing and transformation logic.

Suggested responsibilities:

- raw byte reading
- AKLZ decompression hook
- MLD container parsing
- GRND parsing
- ground_links parsing
- entry parsing by `fxn`
- Ninja chunk parsing for embedded chunk data where needed
- pointer fixups via POF0 where needed
- transform decoding
- diagnostics and unknown-entry capture

### `NavWorld`

Engine-neutral data model.

Suggested responsibilities:

- world geometry objects
- walk surfaces
- adjacency links
- collision volumes
- trigger volumes
- unknown entry placeholders
- diagnostics
- optional serialization

### `QtWorldView`

Qt adaptation layer.

Suggested responsibilities:

- convert world meshes into Qt mesh buffers
- build scene graph
- attach materials / debug colors
- expose selection metadata
- render overlays for links, triggers, and route candidates

## Engine-neutral world model

The parser should not emit Qt types directly. The intermediate representation should be independent of the viewer.

A reasonable initial design is:

```cpp
struct Vec3 {
    float x;
    float y;
    float z;
};

struct Quat {
    float x;
    float y;
    float z;
    float w;
};

struct Transform {
    Vec3 position;
    Quat rotation;
    Vec3 scale;
};

struct MeshVertex {
    Vec3 position;
    Vec3 normal;
    float u;
    float v;
};

struct MeshData {
    std::vector<MeshVertex> vertices;
    std::vector<uint32_t> indices;
};

struct GrndSurface {
    uint32_t id;
    MeshData mesh;
    Transform transform;
    std::vector<uint32_t> linked_grnd_ids;
};

struct CollisionVolume {
    uint32_t source_entry_id;
    Transform transform;
    MeshData debug_mesh;
};

struct TriggerVolume {
    uint32_t source_entry_id;
    uint32_t fxn;
    Transform transform;
    MeshData debug_mesh;
};

struct UnknownEntry {
    uint32_t source_entry_id;
    uint32_t fxn;
    Transform transform;
    std::vector<uint8_t> raw_payload;
};

struct WorldModel {
    std::vector<GrndSurface> grnd_surfaces;
    std::vector<CollisionVolume> collisions;
    std::vector<TriggerVolume> triggers;
    std::vector<UnknownEntry> unknown_entries;
};
```

This should evolve as the format becomes clearer, but it is already sufficient to support a first viewer pass and later search-facing world extraction.

## SearchExplorer-facing data model

The NavigationPhase work eventually needs a world representation that can support route finding and event reasoning. The viewer representation alone is not enough.

The parser should therefore also expose a search-oriented view of the world:

```cpp
struct WalkSurfaceNode {
    uint32_t grnd_id;
    std::vector<uint32_t> neighbor_grnd_ids;
    MeshData mesh;
};

struct EncounterOrTriggerRegion {
    uint32_t source_entry_id;
    uint32_t fxn;
    Transform transform;
};

struct SearchWorldModel {
    std::vector<WalkSurfaceNode> surfaces;
    std::vector<EncounterOrTriggerRegion> regions;
};
```

The important point is that **GRND objects plus ground_links** should become the first explicit walk graph. That gives you a correct foundation for later refinements such as polygon-level adjacency, portals, and movement constraints.

## Parsing strategy

## 1. File acquisition

The current system plan is to obtain MLD bytes directly from the disc through Dolphin-integrated file access rather than relying on a separate offline extraction process. [#2]

That should remain the source of truth for SOASim runtime integration.

The MLD parser itself should therefore accept a raw byte span or vector and not care whether those bytes came from:

- Dolphin-based extraction,
- test fixtures,
- an offline research dump.

## 2. Compression

Legends archives are known to use AKLZ in at least part of the asset pipeline. [#6] [#7]

The parser should therefore begin with:

1. Detect whether the incoming file is compressed.
2. If compressed, decompress to a raw byte buffer.
3. Parse only the decompressed form.

This decompression layer should be separate from the MLD parser itself.

## 3. MLD container parsing

This is the area where the plan must stay conservative.

We know from your research code and project notes that MLD contains:

- GRND-related structures,
- entry records with embedded placement,
- handler dispatch through `fxn`,
- and likely embedded model or chunk-like structures in some cases.

What we should **not** assume yet is that the top-level MLD can be described purely as a generic Ninja chunk file.

So the implementation plan should be:

- Port the dedicated MLD container and object parsing logic from your C# research into C++.
- Reuse Ninja chunk parsing only for substructures that are actually chunk-based.
- Treat all currently uncertain container fields as explicit TODOs in code comments and diagnostics.

## 4. GRND parsing

GRND objects are the highest priority object type because they represent the walkable collision surface.

The GRND parser should produce:

- a unique GRND identifier,
- the polygon or mesh geometry for that GRND object,
- the transform embedded in the object,
- the list of linked GRND IDs from the associated ground_links data.

The first milestone does not need every possible semantic flag on the GRND object. It only needs enough to:

- reconstruct the mesh,
- link surfaces together,
- display them in the viewer,
- and expose adjacency to the search layer.

## 5. Entry parsing by `fxn`

Entries that are not GRND should be parsed into a common entry structure that at minimum captures:

- entry identifier,
- `fxn`,
- transform,
- raw parameter block,
- any known subtype fields.

The parser should then dispatch to a handler registry.

Suggested pattern:

```cpp
class EntryHandler {
public:
    virtual ~EntryHandler() = default;
    virtual bool can_handle(uint32_t fxn) const = 0;
    virtual void parse(const RawEntry& entry, WorldModel& out) const = 0;
};
```

That gives you an incremental path where unknown `fxn` values still remain visible in the world model and in diagnostics.

## Initial supported `fxn` strategy

The early implementation should not wait for full handler coverage.

Instead, define an initial support matrix:

1. **Required immediately**
   - GRND-related records
   - collision-like records needed to block movement or visualize obstacles
   - trigger-like records that visibly affect route planning or transitions

2. **Supported as debug placeholders**
   - entries with known transform but unknown behavior

3. **Unsupported but preserved**
   - entries whose payload is not yet understood

The first implementation should also produce a histogram of all `fxn` values encountered across test MLDs. That will let you prioritize the next handlers by frequency and by gameplay importance.

## Transform handling

This is one of the most important technical risks for viewer correctness.

The MLD-to-viewer pipeline may need one or more of the following conversions:

- axis remapping,
- sign inversion on one axis,
- matrix transpose,
- winding reversal,
- unit scaling,
- rotation reinterpretation,
- hierarchy flattening.

The correct approach is to make these transformations explicit and testable rather than burying them in rendering code.

### Coordinate system

Qt 3D and Qt Quick 3D operate in a right-handed 3D scene convention. [#8]

GameCube-era data is often also effectively right-handed, but file formats and imported transforms can still differ in axis ordering or sign conventions.

Therefore, the parser should preserve raw transform data first, and the world-building step should apply a named conversion policy.

Suggested policy object:

```cpp
struct CoordinatePolicy {
    bool swap_yz = false;
    bool negate_x = false;
    bool negate_y = false;
    bool negate_z = false;
    float uniform_scale = 1.0f;
    bool reverse_triangle_winding = false;
    bool transpose_matrices = false;
};
```

### Rotation encoding

An open item is whether relevant rotation fields are stored as floats, integers, BAMS-style angles, or matrices. Binary angular measurement is common enough that it should remain on the watch list, but this should not be declared as confirmed format behavior unless verified from your MLD-specific C# parser path. [#9]

So the plan should be:

- port the actual rotation interpretation from your existing MLD research,
- only fall back to BAMS conversion if that is what the C# parser or sample validation confirms.

### Triangle winding

If faces render inside out in Qt, the first likely cause is winding mismatch. `QQuick3DGeometry` follows standard front-face conventions and expects correct winding in the provided index order. [#10]

The mesh conversion path should therefore allow index reversal as a named conversion switch rather than hard-coding a guess.

### Matrix layout

If any MLD object stores full matrices, the implementation should explicitly validate whether they are:

- row-major or column-major,
- local or world transforms,
- parent-relative or already flattened.

This should be determined from the C# parser or by visual comparison with known areas, not by assumption.

## Qt viewer integration

### Decision record (2026-04-12)

The SOASim implementation decision is now locked to **Qt Quick 3D** for the first viewer implementation pass.

- Primary scene technology: **Qt Quick 3D**
- Preferred camera interaction: **OrbitCameraController** (mouse orbit/pan/zoom workflow)
- Integration strategy with existing widget shell: embed the Quick 3D scene in the Qt Widgets host for incremental adoption
- Architectural guardrail: parser and world model remain renderer-agnostic; only the Qt adapter is renderer-specific

Qt3D remains acceptable only as a temporary compatibility bridge if needed for migration work, but it is not the target path for new feature development in this NavigationPhase viewer track.

## Recommended rendering path

For the first implementation, the cleaner path is:

- use an engine-neutral mesh representation,
- convert that into Qt geometry buffers,
- render through either:
  - **Qt Quick 3D**, preferably, or
  - **Qt3D** if that better matches the current GUI architecture.

A key correction to the earlier generic plan is this:

Qt Quick 3D is a reasonable primary target for new work, but that should not be treated as a hard requirement if your existing application structure already aligns better with a widget-based or Qt3D-based embedding path. Qt3D has deprecations in Qt 6, so it should not be the long-term strategic default, but it can still be a pragmatic bridge if your current viewer plumbing is closer to it. [#11] [#12]

So the practical recommendation is:

- keep the parser and WorldModel renderer-agnostic,
- define one Qt adapter interface,
- allow the first implementation to target the viewer technology that best fits your existing GUI.

## Qt adapter responsibilities

The Qt adapter should:

- convert `MeshData` to interleaved vertex buffers,
- create scene objects for GRND meshes,
- create line or arrow overlays for ground_links,
- create semi-transparent meshes or primitives for collision volumes,
- create distinct debug markers for triggers and unknown entries,
- preserve a mapping from scene object back to `source_entry_id` or `grnd_id` for inspection.

## Viewer scene layers

A clean scene breakdown is:

- **Ground layer**: GRND surfaces
- **Link layer**: lines between linked GRND objects
- **Collision layer**: coarse obstacle meshes or prisms
- **Trigger layer**: trigger volumes or markers
- **Unknown layer**: magenta debug markers or boxes for unsupported `fxn`
- **Overlay layer**: route preview, selected node highlight, search annotations

## Implementation phases

## Phase 1: Minimum viewer path

Goal: show walkable space and links.

Deliverables:

- byte acquisition from Dolphin extraction path
- decompression hook
- minimal MLD parser sufficient to locate GRND objects and ground_links
- WorldModel population for GRND surfaces
- viewer rendering for GRND meshes
- link overlay for ground_links
- object selection and inspection panel

This is the first milestone because it gives immediate validation that:

- extraction works,
- parsing works,
- transforms are mostly correct,
- and the world model is structurally useful.

## Phase 2: Collision and trigger entries

Goal: begin interpreting non-GRND entries.

Deliverables:

- common raw entry parser
- handler registry keyed by `fxn`
- first supported collision handlers
- first supported trigger handlers
- debug visualization for unsupported entries
- `fxn` frequency report from sample areas

## Phase 3: Navigation-facing integration

Goal: make the parsed world useful outside the viewer.

Deliverables:

- export SearchExplorer-facing walk graph
- expose GRND adjacency graph to nav systems
- expose trigger/collision regions to route analysis
- integrate parse diagnostics into NavigationPhase reporting

## Testing and validation

The most reliable validation is visual and comparative.

Each implementation step should be checked by:

1. parsing a known MLD file,
2. rendering the result in the viewer,
3. comparing it against expected area structure from the game,
4. confirming that linked GRND regions behave like a plausible walk graph.

The first regression corpus should include:

- an area with simple ground geometry,
- an area with multiple linked GRND surfaces,
- an area with overlap in vertical space,
- an area with obvious trigger or transition behavior,
- an area with unsupported or unknown entries.

The parser should also log:

- unknown `fxn` values,
- unknown record types,
- unresolved transform assumptions,
- chunk endianness decisions,
- pointer-fixup anomalies.

## Open research items

The following should remain explicitly open in the plan rather than being silently guessed:

1. The exact top-level MLD container structure in GameCube Legends.
2. Which sub-objects are plain MLD-native structures versus embedded Ninja chunk data.
3. The exact transform representation and axis conventions used by the MLD entry types you care about.
4. The complete list of meaningful `fxn` handlers.
5. Which entries represent collision volumes versus gameplay triggers versus purely visual helpers.
6. Whether any relevant rotation fields use BAMS or another compact angle encoding.
7. Whether there are per-area exceptions or special cases in GRND or entry interpretation.

## Recommended implementation rule

Do not let unknowns block progress.

The parser should always prefer:

- preserve unknown raw data,
- attach diagnostics,
- render a debug placeholder,
- move on.

That approach matches the project need to iteratively grow handler support while still getting value from the viewer and from GRND-based navigation early.

## References

- [#1] SOASim NavigationPhase state and world model planning: `planning/NavigationPhase/02-state-and-world-model.md`  
  https://github.com/jahorta/SOASim/blob/DBMigrate/planning/NavigationPhase/02-state-and-world-model.md

- [#2] SOASim NavigationPhase open implementation questions: `planning/NavigationPhase/05-open-implementation-questions.md`  
  https://github.com/jahorta/SOASim/blob/DBMigrate/planning/NavigationPhase/05-open-implementation-questions.md

- [#3] `NJReader.cs` from `sa_tools/SoAMLDs`, showing Ninja chunk handling, GameCube chunk-size endianness notes, and Skies-specific unknown chunk mentions  
  https://github.com/jahorta/sa_tools/blob/SoAMLDs/Libraries/SAModel/Ninja%20Binary/NJReader.cs

- [#4] `POF0Helper.cs` from `sa_tools/SoAMLDs`, showing pointer-delta decode and fixup behavior  
  https://github.com/jahorta/sa_tools/blob/SoAMLDs/Libraries/SAModel/Ninja%20Binary/POF0Helper.cs

- [#5] `NJTLHelper.cs` from `sa_tools/SoAMLDs`, relevant to texture-list chunk handling in the Ninja subsystem  
  https://github.com/jahorta/sa_tools/blob/SoAMLDs/Libraries/SAModel/Ninja%20Binary/NJTLHelper.cs

- [#6] AuroraLib.Compression reference listing AKLZ and identifying it as used in Skies of Arcadia Legends  
  https://github.com/Venomalia/AuroraLib.Compression

- [#7] Skies of Arcadia archive extraction guide noting AKLZ usage in Legends asset work  
  https://github-wiki-see.page/m/ItsEasyActually/IEA_Guides/wiki/Skies-of-Arcadia-Archive-Extraction-Guide

- [#8] Qt Quick 3D `Node` documentation  
  https://doc.qt.io/qt-6/qml-qtquick3d-node.html

- [#9] Binary angular measurement overview  
  https://en.wikipedia.org/wiki/Binary_angular_measurement

- [#10] Qt `QQuick3DGeometry` documentation  
  https://doc.qt.io/qt-6/qquick3dgeometry.html

- [#11] Qt 3D `QTransform` documentation  
  https://doc.qt.io/qt-6/qt3dcore-qtransform.html

- [#12] Qt 3D `GeometryRenderer` QML documentation  
  https://doc.qt.io/qt-6/qml-qt3d-render-geometryrenderer.html
