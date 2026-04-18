# SA3D C# -> C++ Naming and Namespace Mapping

Date: 2026-04-18
Status: Locked for Phase 0

## Goals

- Preserve source traceability from SA3D C# types to SoaSim C++ implementation.
- Minimize naming drift so parity debugging can jump between languages quickly.

## Namespace mapping rules

1. Source namespace root `SA3D.Modeling` maps to `SoaSimMLD/SA3DPort` path root.
2. Namespace segments map 1:1 to directories.
3. File names map to source type names when feasible.

Examples:

- `SA3D.Modeling.File.ModelFile` -> `SoaSimMLD/SA3DPort/File/ModelFile.*`
- `SA3D.Modeling.Animation.Keyframes` -> `SoaSimMLD/SA3DPort/Animation/Keyframes.*`
- `SA3D.Modeling.Mesh.Chunk.PolyChunks` -> `SoaSimMLD/SA3DPort/Mesh/Chunk/PolyChunks/*`

## Type and member naming rules

- Class/struct names: preserve original C# type casing.
- Enum type names and enum value names: preserve 1:1.
- Method names: preserve PascalCase unless conflicting with reserved keywords.
- Properties: map to `Get*`/`Set*` accessors or explicit fields only when needed by C++ constraints.
- Constants: preserve identifier spelling and semantic meaning.

## Allowed divergence table

| C# shape | C++ adaptation | Rationale |
|---|---|---|
| auto-properties | explicit private field + getter/setter | C++ has no direct property syntax |
| `byte[]` | `std::vector<uint8_t>` | ownership and bounds safety |
| nullable refs | pointer or `std::optional<T>` | explicit nullability modeling |
| extension methods | free/static utility functions | language feature mismatch |

## Traceability expectations

- Every ported file should include a short header comment naming the original C# type path.
- Any naming divergence beyond the table above must be documented in this file before merge.
