#pragma once

#include "ViewPlacementCacheModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace savor::predict {

struct ViewPlacementGeometryVectorBits {
    std::uint32_t x_bits = 0;
    std::uint32_t y_bits = 0;
    std::uint32_t z_bits = 0;
};

struct ViewPlacementGeometryCombatantState {
    std::optional<std::uint32_t> instruction_flags;
    std::optional<ViewPlacementGeometryVectorBits> current_position;
    std::optional<ViewPlacementGeometryVectorBits> saved_position;
    std::optional<std::uint32_t> geometry_extent_bits;
    std::optional<std::int8_t> mld_slot_result;
};

struct ViewPlacementGeometryInput {
    std::array<std::optional<ViewPlacementGeometryCombatantState>, 12> combatants{};
    bool use_saved_position = false;
};

enum class ViewPlacementGeometryStatus {
    Complete,
    MissingInput,
};

enum class ViewPlacementGeometryConfidence {
    RuntimeValidated,
    StaticContract,
};

struct ViewPlacementGeometryResult {
    ViewPlacementGeometryStatus status = ViewPlacementGeometryStatus::MissingInput;
    ViewPlacementGeometryConfidence confidence =
        ViewPlacementGeometryConfidence::StaticContract;
    ViewPlacementGeometryVectorBits center{};
    std::uint32_t half_x_bits = 0;
    std::uint32_t half_z_bits = 0;
    std::uint32_t max_extent_bits = 0;
    ViewPlacementCacheKey cache_key{};
    std::vector<std::uint8_t> included_slots;
    std::vector<std::uint8_t> excluded_slots;
    std::vector<std::string> missing_inputs;
    std::string provenance;
};

ViewPlacementGeometryResult model_view_placement_geometry(
    const ViewPlacementGeometryInput& input);

enum class ViewPlacementReadiness {
    Ready,
    NotReady,
    Unknown,
};

struct GeometryBackedViewPlacementRequest {
    ViewPlacementReadiness readiness = ViewPlacementReadiness::Ready;
    std::string publisher_source_id{
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication};
    ViewPlacementCacheEventContext context{};
    std::string provenance;
};

ViewPlacementResolutionResult resolve_geometry_backed_view_placement(
    ViewPlacementCacheRuntime& runtime,
    std::uint32_t& rng_state,
    const ViewPlacementGeometryResult& geometry,
    const GeometryBackedViewPlacementRequest& request);

} // namespace savor::predict
