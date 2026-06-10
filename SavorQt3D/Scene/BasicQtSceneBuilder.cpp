#include "BasicQtSceneBuilder.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

namespace savor::qt3d::scene {

SceneBuildResult BasicQtSceneBuilder::buildScene(const savor::mld::model::GeometryBuildResult& geometry) const {
    SceneBuildResult out{};
    out.grounds.reserve(geometry.objects.size());
    out.njcmObjects.reserve(geometry.objects.size());
    for (const auto& object : geometry.objects) {
        SceneMesh mesh{};
        mesh.vertices.reserve(object.mesh.vertices.size());
        for (const auto& vtx : object.mesh.vertices) {
            mesh.vertices.push_back(SceneVertex{
                .px = vtx.position.x,
                .py = vtx.position.y,
                .pz = vtx.position.z,
                .nx = vtx.normal.x,
                .ny = vtx.normal.y,
                .nz = vtx.normal.z,
                .u = vtx.u,
                .v = vtx.v,
            });
        }
        for (const auto& poly : object.mesh.polygons) {
            for (const auto idx : poly.indices) {
                if (idx < mesh.vertices.size()) {
                    mesh.indices.push_back(idx);
                }
            }
        }
        if (object.sourceKind == savor::mld::model::GeometrySourceKind::Grnd) {
            GroundSceneNode node{};
            node.grndId = object.sourceId;
            node.mesh = std::move(mesh);
            out.grounds.push_back(std::move(node));
            continue;
        }
        if (object.sourceKind == savor::mld::model::GeometrySourceKind::Njcm) {
            NjcmSceneNode node{};
            node.objectAddress = object.sourceId;
            node.mesh = std::move(mesh);
            out.njcmObjects.push_back(std::move(node));
            continue;
        }
    }

    std::set<std::pair<std::uint32_t, std::uint32_t>> dedup{};
    for (const auto& ground : out.grounds) {
        for (const auto linkedId : ground.linkedGrndIds) {
            const auto key = std::minmax(ground.grndId, linkedId);
            if (!dedup.insert(key).second) {
                continue;
            }
            out.links.push_back(GroundLinkSceneNode{
                .fromGrndId = key.first,
                .toGrndId = key.second,
            });
        }
    }

    return out;
}

} // namespace savor::qt3d::scene
