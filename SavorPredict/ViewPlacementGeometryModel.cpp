#include "ViewPlacementGeometryModel.h"

#include <bit>
#include <string_view>
#include <utility>

namespace savor::predict {
namespace {

constexpr std::uint32_t kMldSlotGate = 0x00000100u;
constexpr std::uint32_t kEnlargedExtent = 0x00200000u;

float decode_float(std::uint32_t bits) {
    return std::bit_cast<float>(bits);
}

std::uint32_t encode_float(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

float ppc_add_single(float lhs, float rhs) {
    volatile float result = lhs + rhs;
    return result;
}

float ppc_sub_single(float lhs, float rhs) {
    volatile float result = lhs - rhs;
    return result;
}

float ppc_mul_single(float lhs, float rhs) {
    volatile float result = lhs * rhs;
    return result;
}

std::string combine_provenance(std::string_view first, std::string_view second) {
    if (first.empty()) return std::string(second);
    if (second.empty() || first == second) return std::string(first);
    return std::string(first) + "; " + std::string(second);
}

struct IncludedCandidate {
    std::uint8_t slot = 0;
    std::uint32_t flags = 0;
    ViewPlacementGeometryVectorBits position{};
    std::uint32_t extent_bits = 0;
};

void append_missing(
    ViewPlacementGeometryResult& result,
    std::size_t slot,
    std::string_view field) {
    result.missing_inputs.push_back(
        "slot" + std::to_string(slot) + "." + std::string(field));
}

ViewPlacementResolutionResult blocked_resolution(
    const ViewPlacementCacheRuntime& runtime,
    std::uint32_t rng_state,
    ViewPlacementCacheReadStatus status,
    std::string provenance) {
    return {
        .status = status,
        .seed_before = rng_state,
        .seed_after = rng_state,
        .draws_consumed = 0,
        .revision_before = runtime.state.revision,
        .revision_after = runtime.state.revision,
        .provenance = std::move(provenance),
    };
}

} // namespace

ViewPlacementGeometryResult model_view_placement_geometry(
    const ViewPlacementGeometryInput& input) {
    ViewPlacementGeometryResult result;
    std::vector<IncludedCandidate> candidates;
    candidates.reserve(input.combatants.size());

    bool used_static_only_branch = input.use_saved_position;
    for (std::size_t slot = 0; slot < input.combatants.size(); ++slot) {
        const auto& optional_state = input.combatants[slot];
        if (!optional_state.has_value()) {
            continue;
        }
        const auto& state = *optional_state;
        if (!state.instruction_flags.has_value()) {
            append_missing(result, slot, "instruction_flags");
            continue;
        }

        const std::uint32_t flags = *state.instruction_flags;
        bool included = true;
        if ((flags & kMldSlotGate) != 0) {
            used_static_only_branch = true;
            if (!state.mld_slot_result.has_value()) {
                append_missing(result, slot, "mld_slot_result");
                continue;
            }
            included = *state.mld_slot_result >= 0;
        }
        if (!included) {
            result.excluded_slots.push_back(static_cast<std::uint8_t>(slot));
            continue;
        }

        const auto& position = input.use_saved_position
            ? state.saved_position
            : state.current_position;
        if (!position.has_value()) {
            append_missing(
                result,
                slot,
                input.use_saved_position ? "saved_position" : "current_position");
        }
        if (!state.geometry_extent_bits.has_value()) {
            append_missing(result, slot, "geometry_extent_bits");
        }
        if (!position.has_value() || !state.geometry_extent_bits.has_value()) {
            continue;
        }

        if ((flags & kEnlargedExtent) != 0) {
            used_static_only_branch = true;
        }
        candidates.push_back({
            .slot = static_cast<std::uint8_t>(slot),
            .flags = flags,
            .position = *position,
            .extent_bits = *state.geometry_extent_bits,
        });
        result.included_slots.push_back(static_cast<std::uint8_t>(slot));
    }

    if (!result.missing_inputs.empty()) {
        result.status = ViewPlacementGeometryStatus::MissingInput;
        result.provenance =
            "FUN_800114AC geometry input is incomplete; cache request was not constructed";
        return result;
    }

    float max_x = -500.0f;
    float min_x = 500.0f;
    float max_z = -500.0f;
    float min_z = 500.0f;
    float max_extent = 0.0f;
    std::uint32_t max_extent_bits = encode_float(max_extent);

    for (const auto& candidate : candidates) {
        const float radius = (candidate.flags & kEnlargedExtent) != 0
            ? 22.5f
            : 7.5f;
        const float x = decode_float(candidate.position.x_bits);
        const float z = decode_float(candidate.position.z_bits);

        const float candidate_max_x = ppc_add_single(x, radius);
        if (candidate_max_x > max_x) max_x = candidate_max_x;
        const float candidate_min_x = ppc_sub_single(x, radius);
        if (candidate_min_x < min_x) min_x = candidate_min_x;
        const float candidate_max_z = ppc_add_single(z, radius);
        if (candidate_max_z > max_z) max_z = candidate_max_z;
        const float candidate_min_z = ppc_sub_single(z, radius);
        if (candidate_min_z < min_z) min_z = candidate_min_z;

        const float extent = decode_float(candidate.extent_bits);
        if (extent > max_extent) {
            max_extent = extent;
            max_extent_bits = candidate.extent_bits;
        }
    }

    float width = ppc_sub_single(max_x, min_x);
    if (width < 0.0f) width = ppc_mul_single(-1.0f, width);
    float depth = ppc_sub_single(max_z, min_z);
    if (depth < 0.0f) depth = ppc_mul_single(-1.0f, depth);
    const float half_x = ppc_mul_single(width, 0.5f);
    const float half_z = ppc_mul_single(depth, 0.5f);
    const float center_x = ppc_add_single(min_x, half_x);
    const float center_z = ppc_add_single(min_z, half_z);
    const float largest_half_extent = half_x <= half_z ? half_z : half_x;
    const float distance = ppc_mul_single(2.0f, largest_half_extent);

    result.status = ViewPlacementGeometryStatus::Complete;
    result.confidence = !candidates.empty() && !used_static_only_branch
        ? ViewPlacementGeometryConfidence::RuntimeValidated
        : ViewPlacementGeometryConfidence::StaticContract;
    result.center = {
        .x_bits = encode_float(center_x),
        .y_bits = encode_float(0.0f),
        .z_bits = encode_float(center_z),
    };
    result.half_x_bits = encode_float(half_x);
    result.half_z_bits = encode_float(half_z);
    result.max_extent_bits = max_extent_bits;
    result.cache_key = {
        .distance_bits = encode_float(distance),
        .center_x_bits = result.center.x_bits,
        .center_y_bits = result.center.y_bits,
        .center_z_bits = result.center.z_bits,
    };
    result.provenance =
        "FUN_800114AC PPC-order single-precision geometry with caller distance scale 2.0";
    return result;
}

ViewPlacementResolutionResult resolve_geometry_backed_view_placement(
    ViewPlacementCacheRuntime& runtime,
    std::uint32_t& rng_state,
    const ViewPlacementGeometryResult& geometry,
    const GeometryBackedViewPlacementRequest& request) {
    if (request.readiness != ViewPlacementReadiness::Ready) {
        const auto reason = request.readiness == ViewPlacementReadiness::NotReady
            ? "view-placement readiness predicate is false"
            : "view-placement readiness predicate is unknown";
        return blocked_resolution(
            runtime,
            rng_state,
            ViewPlacementCacheReadStatus::Unknown,
            combine_provenance(request.provenance, reason));
    }
    if (geometry.status != ViewPlacementGeometryStatus::Complete) {
        return blocked_resolution(
            runtime,
            rng_state,
            ViewPlacementCacheReadStatus::MissingInput,
            combine_provenance(request.provenance, geometry.provenance));
    }

    return resolve_view_placement_request(
        runtime,
        rng_state,
        ViewPlacementRequest{
            .publisher_source_id = request.publisher_source_id,
            .key = geometry.cache_key,
            .context = request.context,
            .provenance = combine_provenance(request.provenance, geometry.provenance),
        });
}

} // namespace savor::predict
