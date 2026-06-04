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

} // namespace

model::GeometryBuildResult GeometryBuilder::build(const ParseResult& parseResult) const {
    model::GeometryBuildResult out{};
    appendGrndGeometry(parseResult, out);
    out.diagnostics.push_back("GeometryBuilder produced " + std::to_string(out.objects.size()) + " objects.");
    return out;
}

} // namespace soasim::mld::parsing
