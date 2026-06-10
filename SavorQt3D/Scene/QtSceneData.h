#pragma once

#include <cstdint>
#include <vector>

namespace savor::qt3d::scene {

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
    std::vector<std::uint32_t> linkedGrndIds{};
    SceneMesh mesh{};
};

struct NjcmSceneNode {
    std::uint32_t objectAddress = 0;
    SceneMesh mesh{};
};

struct GroundLinkSceneNode {
    std::uint32_t fromGrndId = 0;
    std::uint32_t toGrndId = 0;
};

struct TriggerSceneNode {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t fxn = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
};

struct UnknownSceneNode {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t fxn = 0;
    float px = 0.0f;
    float py = 0.0f;
    float pz = 0.0f;
};

struct SceneBuildResult {
    std::vector<GroundSceneNode> grounds{};
    std::vector<NjcmSceneNode> njcmObjects{};
    std::vector<GroundLinkSceneNode> links{};
    std::vector<TriggerSceneNode> triggers{};
    std::vector<UnknownSceneNode> unknowns{};
};

} // namespace savor::qt3d::scene
