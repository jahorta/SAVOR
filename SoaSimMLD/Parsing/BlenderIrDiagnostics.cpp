#include "BlenderIrDiagnostics.h"

namespace soasim::mld::parsing {

void BlenderIrDiagnostics::finalizeMesh(model::BlenderIrMesh& mesh) {
    mesh.diagnostics = {};

    for (const auto& triSet : mesh.triangleSets) {
        const auto& corners = triSet.corners;
        for (std::size_t ii = 0; ii + 2 < corners.size(); ii += 3) {
            const auto ia = static_cast<std::size_t>(corners[ii].vertexIndex);
            const auto ib = static_cast<std::size_t>(corners[ii + 1].vertexIndex);
            const auto ic = static_cast<std::size_t>(corners[ii + 2].vertexIndex);

            if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() || ic >= mesh.vertices.size()) {
                ++mesh.diagnostics.outOfRangeIndexCount;
                continue;
            }

            if (ia == ib || ib == ic || ia == ic) {
                ++mesh.diagnostics.degenerateTriangleCount;
            }

            if (triSet.fromCacheReplay) {
                ++mesh.diagnostics.cacheReplayTriangleCount;
            }
        }
    }
}

} // namespace soasim::mld::parsing
