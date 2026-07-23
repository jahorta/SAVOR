#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace savor::navigation {

enum class NavigationDiagnosticSeverity {
    Info,
    Warning,
    Error,
};

struct NavigationDiagnostic {
    NavigationDiagnosticSeverity severity = NavigationDiagnosticSeverity::Info;
    std::string message{};
};

struct NavigationVec3 {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
};

struct NavigationQuat {
    float x = 0.0F;
    float y = 0.0F;
    float z = 0.0F;
    float w = 1.0F;
};

struct NavigationTransform {
    NavigationVec3 position{};
    NavigationQuat rotation{};
    NavigationVec3 scale{ 1.0F, 1.0F, 1.0F };
};

struct NavigationBounds {
    NavigationVec3 minimum{};
    NavigationVec3 maximum{};
    bool valid = false;
};

struct NavigationMeshVertex {
    NavigationVec3 position{};
    NavigationVec3 normal{ 0.0F, 1.0F, 0.0F };
    bool hasSourceNormal = false;
    std::optional<std::uint32_t> rawUserAttributesU32{};
};

struct NavigationTriangleMetadata {
    std::array<std::uint16_t, 3> rawU16{};
    bool present = false;
};

struct NavigationMesh {
    std::vector<NavigationMeshVertex> vertices{};
    std::vector<std::uint32_t> indices{};
    std::vector<NavigationTriangleMetadata> triangleMetadata{};
};

struct NavigationRegionMesh {
    std::uint32_t sourceObjectAddress = 0;
    std::size_t sourceChunkOffset = 0;
    std::size_t sourceNodeOffset = 0;
    std::size_t sourceAttachOffset = 0;
    NavigationMesh mesh{};
};

enum class NavigationSurfaceSourceKind {
    Grnd,
    Gobj,
};

enum class NavigationSurfaceTraversalAvailability {
    Static,
    RequiresRuntimeState,
};

struct NavigationSurfaceSourceKey {
    std::uint32_t sourceEntryId = 0;
    std::uint32_t sourceBlockOffset = 0;
    std::uint32_t sourceNodeOffset = 0;

    [[nodiscard]] bool operator==(const NavigationSurfaceSourceKey&) const = default;
};

struct NavigationSurface {
    NavigationSurfaceSourceKey sourceKey{};
    NavigationSurfaceSourceKind sourceKind = NavigationSurfaceSourceKind::Grnd;
    NavigationSurfaceTraversalAvailability traversalAvailability =
        NavigationSurfaceTraversalAvailability::Static;
    std::size_t sourceTableIndex = 0;
    std::int32_t tblId = 0;
    std::string fxnName{};
    NavigationMesh mesh{};
};

enum class NavigationAuthoredGroundFallbackTargetStatus {
    Resolved,
    MissingEntry,
    MissingGeometry,
    SuppressedAfterMissingEntry,
};

struct NavigationAuthoredGroundFallbackTarget {
    std::uint32_t targetEntryId = 0;
    std::size_t authoredOrdinal = 0;
    std::optional<std::size_t> targetTableIndex{};
    NavigationAuthoredGroundFallbackTargetStatus status =
        NavigationAuthoredGroundFallbackTargetStatus::MissingEntry;
    std::vector<NavigationSurfaceSourceKey> targetSurfaces{};
};

struct NavigationAuthoredGroundFallbackChain {
    std::size_t sourceTableIndex = 0;
    std::uint32_t sourceEntryId = 0;
    std::vector<NavigationSurfaceSourceKey> sourceSurfaces{};
    std::vector<NavigationAuthoredGroundFallbackTarget> targets{};
};

enum class NavigationRegionKind {
    Collision,
    Trigger,
    MovingObject,
    Unknown,
};

struct NavigationRegion {
    NavigationRegionKind kind = NavigationRegionKind::Unknown;
    std::uint32_t sourceEntryId = 0;
    std::string fxnName{};
    std::int32_t tblId = 0;
    NavigationTransform transform{};
    std::vector<std::uint32_t> objectAddresses{};
    std::size_t rawPayloadSize = 0;
    std::vector<NavigationRegionMesh> meshes{};
};

struct NavigationAreaSourceIdentity {
    std::filesystem::path path{};
    std::uintmax_t byteSize = 0;
};

struct NavigationAreaModel {
    NavigationAreaSourceIdentity source{};
    std::vector<NavigationSurface> surfaces{};
    std::vector<NavigationAuthoredGroundFallbackChain> authoredGroundFallbackChains{};
    std::vector<NavigationRegion> regions{};
    NavigationBounds bounds{};
    std::vector<NavigationDiagnostic> diagnostics{};
    std::size_t unknownEntryCount = 0;
    std::size_t skippedObjectRoleGobjCount = 0;
    std::size_t failedGroundResourceCount = 0;
    std::size_t failedWallRegionCount = 0;
    std::size_t failedTriggerRegionCount = 0;
    std::size_t failedMovingObjectRegionCount = 0;
    bool hasCompleteGroundGeometry = false;
    bool hasCompleteWallGeometry = false;
    bool hasCompleteTriggerGeometry = false;
    bool hasCompleteMovingObjectGeometry = false;
};

} // namespace savor::navigation
