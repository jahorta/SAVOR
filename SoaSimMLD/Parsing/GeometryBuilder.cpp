#include "GeometryBuilder.h"

#include <string>
#include <utility>

namespace soasim::mld::parsing {
namespace {

void appendGrndGeometry(const ParseResult& parseResult, model::GeometryBuildResult& out) {
    out.objects.reserve(out.objects.size() + parseResult.world.grndSurfaces.size());
    for (const auto& grnd : parseResult.world.grndSurfaces) {
        model::GeometryObject obj{};
        obj.sourceKind = model::GeometrySourceKind::Grnd;
        obj.sourceId = grnd.id;
        obj.label = "GRND_" + std::to_string(grnd.id);

        obj.mesh.vertices.reserve(grnd.mesh.vertices.size());
        for (const auto& vtx : grnd.mesh.vertices) {
            model::SemanticVertex sv{};
            sv.position = vtx.position;
            sv.normal = vtx.normal;
            sv.u = vtx.u;
            sv.v = vtx.v;
            sv.hasPosition = true;
            sv.hasNormal = true;
            sv.hasUv = true;
            obj.mesh.vertices.push_back(sv);
        }

        for (std::size_t ii = 0; ii + 2 < grnd.mesh.indices.size(); ii += 3) {
            model::SemanticPolygon poly{};
            poly.primitiveType = 3;
            poly.estimatedTriangleCount = 1;
            poly.indices.push_back(grnd.mesh.indices[ii]);
            poly.indices.push_back(grnd.mesh.indices[ii + 1]);
            poly.indices.push_back(grnd.mesh.indices[ii + 2]);
            obj.mesh.polygons.push_back(std::move(poly));
        }

        out.objects.push_back(std::move(obj));
    }
}

void appendNjcmGeometry(const ParseResult& parseResult, model::GeometryBuildResult& out) {
    for (const auto& objectRange : parseResult.decodedObjectChunkRanges) {
        for (std::size_t chunkIdx = objectRange.decodedChunkBegin;
            chunkIdx < objectRange.decodedChunkEnd && chunkIdx < parseResult.decodedNjcmChunks.size();
            ++chunkIdx) {
            const auto& chunk = parseResult.decodedNjcmChunks[chunkIdx];
            for (const auto& attach : chunk.attaches) {
            model::GeometryObject obj{};
            obj.sourceKind = model::GeometrySourceKind::Njcm;
            obj.sourceId = objectRange.objectAddress;
            obj.label = "NJCM_obj_" + std::to_string(objectRange.objectAddress) +
                "_attach_" + std::to_string(attach.offset);

            obj.mesh.vertices.reserve(attach.semanticVertices.size());
            for (const auto& vtx : attach.semanticVertices) {
                model::SemanticVertex sv{};
                sv.position = vtx.position;
                sv.hasPosition = vtx.hasPosition;
                obj.mesh.vertices.push_back(sv);
            }

            if (!attach.semanticPrimitives.empty()) {
                obj.mesh.polygons.reserve(attach.semanticPrimitives.size());
                for (const auto& prim : attach.semanticPrimitives) {
                    model::SemanticPolygon sp{};
                    sp.indices = prim.indices;
                    sp.primitiveType = static_cast<std::uint8_t>(prim.kind);
                    switch (prim.kind) {
                    case model::NjPrimitiveKind::Triangle:
                        sp.estimatedTriangleCount = 1;
                        break;
                    case model::NjPrimitiveKind::Quad:
                        sp.estimatedTriangleCount = 2;
                        break;
                    case model::NjPrimitiveKind::Strip:
                        sp.estimatedTriangleCount = (prim.indices.size() > 2U) ? (prim.indices.size() - 2U) : 0U;
                        break;
                    default:
                        sp.estimatedTriangleCount = 0;
                        break;
                    }
                    obj.mesh.polygons.push_back(std::move(sp));
                }
            } else {
                obj.mesh.polygons.reserve(attach.semanticPolygons.size());
                for (const auto& poly : attach.semanticPolygons) {
                    model::SemanticPolygon sp{};
                    sp.indices = poly.indices;
                    sp.primitiveType = poly.type;
                    sp.estimatedTriangleCount = poly.estimatedTriangleCount;
                    obj.mesh.polygons.push_back(std::move(sp));
                }
            }

            if (obj.mesh.vertices.empty() && attach.decodedVertexCount > 0) {
                out.diagnostics.push_back("NJCM attach " + std::to_string(attach.offset) +
                    " has vertex count metadata but no semantic vertices decoded.");
            }
            out.objects.push_back(std::move(obj));
            }
        }
    }
}

} // namespace

model::GeometryBuildResult GeometryBuilder::build(const ParseResult& parseResult) const {
    model::GeometryBuildResult out{};
    appendGrndGeometry(parseResult, out);
    appendNjcmGeometry(parseResult, out);
    out.diagnostics.push_back("GeometryBuilder produced " + std::to_string(out.objects.size()) + " objects.");
    return out;
}

} // namespace soasim::mld::parsing
