#include <gtest/gtest.h>

#include <RngCore.h>
#include <ViewPlacementGeometryModel.h>

#include <bit>
#include <cstdint>

namespace {

using namespace savor::predict;

std::uint32_t bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

ViewPlacementGeometryVectorBits vector_bits(float x, float y, float z) {
    return {
        .x_bits = bits(x),
        .y_bits = bits(y),
        .z_bits = bits(z),
    };
}

ViewPlacementGeometryCombatantState combatant(
    float current_x,
    float current_z,
    float saved_x,
    float saved_z,
    std::uint32_t flags = 0,
    float extent = 0.0f) {
    return {
        .instruction_flags = flags,
        .current_position = vector_bits(current_x, 0.0f, current_z),
        .saved_position = vector_bits(saved_x, 0.0f, saved_z),
        .geometry_extent_bits = bits(extent),
    };
}

ViewPlacementGeometryInput direct_view_captured_geometry_input() {
    ViewPlacementGeometryInput input;
    input.combatants[0] = combatant(-15.0f, -45.0f, 0.0f, 0.0f, 0, 4.0f);
    input.combatants[4] = combatant(15.0f, 15.0f, 0.0f, 0.0f, 0, 12.5f);
    return input;
}

ViewPlacementGeometryInput placement_function_captured_geometry_input() {
    ViewPlacementGeometryInput input;
    input.combatants[1] = combatant(15.0f, 0.0f, 0.0f, 0.0f, 0, 3.0f);
    input.combatants[5] = combatant(30.0f, 30.0f, 0.0f, 0.0f, 0, 9.0f);
    return input;
}

TEST(SavorPredictViewPlacementGeometryModel, EmptyTwelveSlotInputUsesStaticBounds) {
    const auto result = model_view_placement_geometry(ViewPlacementGeometryInput{});

    ASSERT_EQ(result.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(result.confidence, ViewPlacementGeometryConfidence::StaticContract);
    EXPECT_TRUE(result.included_slots.empty());
    EXPECT_TRUE(result.excluded_slots.empty());
    EXPECT_EQ(result.half_x_bits, bits(500.0f));
    EXPECT_EQ(result.half_z_bits, bits(500.0f));
    EXPECT_EQ(result.center.x_bits, bits(1000.0f));
    EXPECT_EQ(result.center.y_bits, bits(0.0f));
    EXPECT_EQ(result.center.z_bits, bits(1000.0f));
    EXPECT_EQ(result.max_extent_bits, bits(0.0f));
    EXPECT_EQ(result.cache_key.distance_bits, bits(1000.0f));
}

TEST(SavorPredictViewPlacementGeometryModel, SelectsCurrentOrSavedPositionExplicitly) {
    ViewPlacementGeometryInput current_input;
    current_input.combatants[3] = combatant(1.0f, 3.0f, 10.0f, 30.0f);
    const auto current = model_view_placement_geometry(current_input);
    ASSERT_EQ(current.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(current.center.x_bits, bits(1.0f));
    EXPECT_EQ(current.center.z_bits, bits(3.0f));
    EXPECT_EQ(current.half_x_bits, bits(7.5f));
    EXPECT_EQ(current.half_z_bits, bits(7.5f));

    auto saved_input = current_input;
    saved_input.use_saved_position = true;
    const auto saved = model_view_placement_geometry(saved_input);
    ASSERT_EQ(saved.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(saved.confidence, ViewPlacementGeometryConfidence::StaticContract);
    EXPECT_EQ(saved.center.x_bits, bits(10.0f));
    EXPECT_EQ(saved.center.z_bits, bits(30.0f));
}

TEST(SavorPredictViewPlacementGeometryModel, MldGateRequiresLookupAndCanExcludeCandidate) {
    ViewPlacementGeometryInput missing_input;
    missing_input.combatants[2] = combatant(8.0f, 12.0f, 0.0f, 0.0f, 0x100u);
    const auto missing = model_view_placement_geometry(missing_input);
    ASSERT_EQ(missing.status, ViewPlacementGeometryStatus::MissingInput);
    ASSERT_EQ(missing.missing_inputs.size(), 1u);
    EXPECT_EQ(missing.missing_inputs[0], "slot2.mld_slot_result");

    auto excluded_input = missing_input;
    excluded_input.combatants[2]->mld_slot_result = -1;
    const auto excluded = model_view_placement_geometry(excluded_input);
    ASSERT_EQ(excluded.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_TRUE(excluded.included_slots.empty());
    ASSERT_EQ(excluded.excluded_slots.size(), 1u);
    EXPECT_EQ(excluded.excluded_slots[0], 2u);

    auto included_input = missing_input;
    included_input.combatants[2]->mld_slot_result = 0;
    const auto included = model_view_placement_geometry(included_input);
    ASSERT_EQ(included.status, ViewPlacementGeometryStatus::Complete);
    ASSERT_EQ(included.included_slots.size(), 1u);
    EXPECT_EQ(included.included_slots[0], 2u);
    EXPECT_EQ(included.center.x_bits, bits(8.0f));
    EXPECT_EQ(included.center.z_bits, bits(12.0f));
}

TEST(SavorPredictViewPlacementGeometryModel, EnlargedRadiusAndMaximumExtentFollowStaticOrder) {
    ViewPlacementGeometryInput input;
    input.combatants[0] = combatant(0.0f, 0.0f, 0.0f, 0.0f, 0, 4.5f);
    input.combatants[1] = combatant(30.0f, 0.0f, 0.0f, 0.0f, 0x200000u, 18.0f);

    const auto result = model_view_placement_geometry(input);

    ASSERT_EQ(result.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(result.confidence, ViewPlacementGeometryConfidence::StaticContract);
    EXPECT_EQ(result.half_x_bits, bits(30.0f));
    EXPECT_EQ(result.center.x_bits, bits(22.5f));
    EXPECT_EQ(result.half_z_bits, bits(22.5f));
    EXPECT_EQ(result.center.z_bits, bits(0.0f));
    EXPECT_EQ(result.max_extent_bits, bits(18.0f));
    EXPECT_EQ(result.cache_key.distance_bits, bits(60.0f));
}

TEST(SavorPredictViewPlacementGeometryModel, DirectViewCapturedVectorIsBitExact) {
    const auto result = model_view_placement_geometry(
        direct_view_captured_geometry_input());

    ASSERT_EQ(result.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(result.confidence, ViewPlacementGeometryConfidence::RuntimeValidated);
    EXPECT_EQ(result.half_x_bits, 0x41B40000u);
    EXPECT_EQ(result.half_z_bits, 0x42160000u);
    EXPECT_EQ(result.center.x_bits, 0x00000000u);
    EXPECT_EQ(result.center.y_bits, 0x00000000u);
    EXPECT_EQ(result.center.z_bits, 0xC1700000u);
    EXPECT_EQ(result.cache_key.distance_bits, 0x42960000u);
    EXPECT_EQ(result.cache_key.center_x_bits, 0x00000000u);
    EXPECT_EQ(result.cache_key.center_y_bits, 0x00000000u);
    EXPECT_EQ(result.cache_key.center_z_bits, 0xC1700000u);
    EXPECT_EQ(result.max_extent_bits, bits(12.5f));
}

TEST(SavorPredictViewPlacementGeometryModel, PlacementFunctionCapturedVectorIsBitExact) {
    const auto result = model_view_placement_geometry(
        placement_function_captured_geometry_input());

    ASSERT_EQ(result.status, ViewPlacementGeometryStatus::Complete);
    EXPECT_EQ(result.half_x_bits, 0x41700000u);
    EXPECT_EQ(result.half_z_bits, 0x41B40000u);
    EXPECT_EQ(result.center.x_bits, 0x41B40000u);
    EXPECT_EQ(result.center.y_bits, 0x00000000u);
    EXPECT_EQ(result.center.z_bits, 0x41700000u);
    EXPECT_EQ(result.cache_key.distance_bits, 0x42340000u);
}

TEST(SavorPredictViewPlacementGeometryModel, GeometryBackedDirectViewMissMissHitUsesExistingResolver) {
    auto runtime = make_default_view_placement_cache_runtime();
    ASSERT_EQ(
        reset_active_record_view_placement_cache(runtime).status,
        ViewPlacementCacheMutationStatus::Applied);
    std::uint32_t seed = 0xCAFEBABEu;
    const auto first_draw = draw_rand15(seed);
    const auto second_draw = draw_rand15(first_draw.next_state);
    const auto first_geometry = model_view_placement_geometry(
        direct_view_captured_geometry_input());
    const auto second_geometry = model_view_placement_geometry(
        placement_function_captured_geometry_input());
    const GeometryBackedViewPlacementRequest request{
        .readiness = ViewPlacementReadiness::Ready,
        .publisher_source_id = std::string(
            ViewPlacementCacheSemanticSource::DirectViewPublication),
        .context = {.frame_index = 85, .worker_sequence = 3},
        .provenance = "direct-view geometry request",
    };

    const auto first = resolve_geometry_backed_view_placement(
        runtime, seed, first_geometry, request);
    const auto second = resolve_geometry_backed_view_placement(
        runtime, seed, second_geometry, request);
    const auto repeated = resolve_geometry_backed_view_placement(
        runtime, seed, second_geometry, request);

    EXPECT_EQ(first.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(second.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(repeated.status, ViewPlacementCacheReadStatus::Hit);
    EXPECT_EQ(first.draws_consumed, 1);
    EXPECT_EQ(second.draws_consumed, 1);
    EXPECT_EQ(repeated.draws_consumed, 0);
    EXPECT_EQ(seed, second_draw.next_state);
    EXPECT_EQ(runtime.state.key.distance_bits, 0x42340000u);
    EXPECT_EQ(runtime.state.key.center_x_bits, 0x41B40000u);
    EXPECT_EQ(runtime.state.key.center_z_bits, 0x41700000u);
    EXPECT_EQ(runtime.state.last_source,
        ViewPlacementCacheSemanticSource::DirectViewPublication);
}

TEST(SavorPredictViewPlacementGeometryModel, PlacementFunctionRequestUsesItsSemanticPublisher) {
    auto runtime = make_default_view_placement_cache_runtime();
    ASSERT_EQ(
        reset_active_record_view_placement_cache(runtime).status,
        ViewPlacementCacheMutationStatus::Applied);
    std::uint32_t seed = 0x10203040u;
    const auto geometry = model_view_placement_geometry(
        placement_function_captured_geometry_input());

    const auto result = resolve_geometry_backed_view_placement(
        runtime,
        seed,
        geometry,
        GeometryBackedViewPlacementRequest{
            .publisher_source_id = std::string(
                ViewPlacementCacheSemanticSource::PlacementFunctionPublication),
            .provenance = "placement-function captured vector",
        });

    EXPECT_EQ(result.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_EQ(runtime.state.last_source,
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication);
    EXPECT_EQ(runtime.state.key.distance_bits, 0x42340000u);
}

TEST(SavorPredictViewPlacementGeometryModel, UnknownReadinessAndMissingGeometryDoNotConsumeRng) {
    auto runtime = make_default_view_placement_cache_runtime();
    ASSERT_EQ(
        reset_active_record_view_placement_cache(runtime).status,
        ViewPlacementCacheMutationStatus::Applied);
    const auto revision_before = runtime.state.revision;
    std::uint32_t seed = 0x55667788u;
    const auto complete = model_view_placement_geometry(
        direct_view_captured_geometry_input());

    const auto unknown = resolve_geometry_backed_view_placement(
        runtime,
        seed,
        complete,
        GeometryBackedViewPlacementRequest{
            .readiness = ViewPlacementReadiness::Unknown,
        });
    EXPECT_EQ(unknown.status, ViewPlacementCacheReadStatus::Unknown);
    EXPECT_EQ(unknown.draws_consumed, 0);
    EXPECT_EQ(seed, 0x55667788u);
    EXPECT_EQ(runtime.state.revision, revision_before);

    ViewPlacementGeometryInput incomplete_input;
    incomplete_input.combatants[0] = ViewPlacementGeometryCombatantState{
        .instruction_flags = 0,
    };
    const auto incomplete = model_view_placement_geometry(incomplete_input);
    ASSERT_EQ(incomplete.status, ViewPlacementGeometryStatus::MissingInput);
    const auto missing = resolve_geometry_backed_view_placement(
        runtime,
        seed,
        incomplete,
        GeometryBackedViewPlacementRequest{});
    EXPECT_EQ(missing.status, ViewPlacementCacheReadStatus::MissingInput);
    EXPECT_EQ(missing.draws_consumed, 0);
    EXPECT_EQ(seed, 0x55667788u);
    EXPECT_EQ(runtime.state.revision, revision_before);
}

} // namespace
