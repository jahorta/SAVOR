#pragma once

#include "Types.h"

#include <cstdint>
#include <string>
#include <vector>

namespace soasim::mld::model {

enum class GeometrySourceKind {
    Grnd,
    Njcm,
};

struct SemanticVertex {
    Vec3 position{};
    Vec3 normal{};
    float u = 0.0f;
    float v = 0.0f;
    bool hasPosition = false;
    bool hasNormal = false;
    bool hasUv = false;
};

struct SemanticPolygon {
    std::vector<std::uint32_t> indices{};
    std::uint8_t primitiveType = 0;
    std::size_t estimatedTriangleCount = 0;
};

struct SemanticMesh {
    std::vector<SemanticVertex> vertices{};
    std::vector<SemanticPolygon> polygons{};
};

struct GeometryObject {
    GeometrySourceKind sourceKind = GeometrySourceKind::Grnd;
    std::uint32_t sourceId = 0;
    std::string label{};
    SemanticMesh mesh{};
};

struct GeometryBuildResult {
    std::vector<GeometryObject> objects{};
    std::vector<std::string> diagnostics{};
};

} // namespace soasim::mld::model
