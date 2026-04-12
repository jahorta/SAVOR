#pragma once

#include <cstdint>
#include <vector>

namespace soasim::qt3d::scene {

struct SceneVertex {
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
    float nx = 0.0f;
    float ny = 0.0f;
    float nz = 0.0f;
    float u = 0.0f;
    float v = 0.0f;
};

struct SceneMesh {
    std::vector<SceneVertex> vertices{};
    std::vector<std::uint32_t> indices{};
};

struct GroundSceneNode {
    std::uint32_t grndId = 0;
    SceneMesh mesh{};
};

struct TriggerSceneNode {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t fxn = 0;
};

struct UnknownSceneNode {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t fxn = 0;
};

struct SceneBuildResult {
    std::vector<GroundSceneNode> grounds{};
    std::vector<TriggerSceneNode> triggers{};
    std::vector<UnknownSceneNode> unknowns{};
};

} // namespace soasim::qt3d::scene
