#pragma once

#include <cstdint>
#include <vector>

namespace soasim::mld::model {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Quat {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 1.0f;
};

struct Transform {
    Vec3 position{};
    Vec3 rotationRaw{};
    Quat rotation{};
    Vec3 scale{ 1.0f, 1.0f, 1.0f };
};

struct MeshVertex {
    Vec3 position{};
    Vec3 normal{};
    float u = 0.0f;
    float v = 0.0f;
};

struct MeshData {
    std::vector<MeshVertex> vertices{};
    std::vector<std::uint32_t> indices{};
};

} // namespace soasim::mld::model
