#include <gtest/gtest.h>

#include <RngCore.h>
#include <ViewPlacementCacheModel.h>

#include <bit>
#include <cstdint>
#include <string>

namespace {

using namespace savor::predict;

ViewPlacementCacheKey make_key(float distance, float x, float y, float z) {
    return {
        .distance_bits = std::bit_cast<std::uint32_t>(distance),
        .center_x_bits = std::bit_cast<std::uint32_t>(x),
        .center_y_bits = std::bit_cast<std::uint32_t>(y),
        .center_z_bits = std::bit_cast<std::uint32_t>(z),
    };
}

ViewPlacementCacheSnapshot make_snapshot(
    std::uint16_t control,
    const ViewPlacementCacheKey& key,
    float angle) {
    return {
        .control = control,
        .key = key,
        .angle_bits = std::bit_cast<std::uint32_t>(angle),
    };
}

ViewPlacementCacheMutationResult publish_snapshot(
    ViewPlacementCacheRuntime& runtime,
    const ViewPlacementCacheSnapshot& snapshot,
    std::string provenance = "test publication") {
    return mutate_view_placement_cache(
        runtime,
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication,
        ViewPlacementCacheMutationRequest{
            .snapshot = snapshot,
            .provenance = std::move(provenance),
        });
}

void expect_key_bits_equal(
    const ViewPlacementCacheKey& actual,
    const ViewPlacementCacheKey& expected) {
    EXPECT_EQ(actual.distance_bits, expected.distance_bits);
    EXPECT_EQ(actual.center_x_bits, expected.center_x_bits);
    EXPECT_EQ(actual.center_y_bits, expected.center_y_bits);
    EXPECT_EQ(actual.center_z_bits, expected.center_z_bits);
}

TEST(SavorPredictViewPlacementCacheModel, DefaultRuntimeInstallsSemanticHooksWithoutAssumingState) {
    const auto runtime = make_default_view_placement_cache_runtime();

    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Uninitialized);
    EXPECT_EQ(runtime.state.revision, 0u);
    EXPECT_TRUE(runtime.history.empty());
    EXPECT_TRUE(runtime.hooks.has_read_hook(
        ViewPlacementCacheSemanticSource::PlacementLookup));
    EXPECT_TRUE(runtime.hooks.has_mutation_hook(
        ViewPlacementCacheSemanticSource::PlacementFunctionPublication));
    EXPECT_TRUE(runtime.hooks.has_mutation_hook(
        ViewPlacementCacheSemanticSource::RunnerPublication));
    EXPECT_TRUE(runtime.hooks.has_mutation_hook(
        ViewPlacementCacheSemanticSource::DirectViewPublication));
    EXPECT_TRUE(runtime.hooks.has_mutation_hook(
        ViewPlacementCacheSemanticSource::ActiveRecordReset));
    EXPECT_TRUE(runtime.hooks.has_mutation_hook(
        ViewPlacementCacheSemanticSource::WorkspaceSnapshotCopy));
}

TEST(SavorPredictViewPlacementCacheModel, ActiveRecordResetPublishesCoherentZeroSnapshot) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto prior = make_snapshot(
        11,
        make_key(75.0f, 10.0f, 0.0f, -15.0f),
        140.0f);
    ASSERT_EQ(publish_snapshot(runtime, prior).status, ViewPlacementCacheMutationStatus::Applied);

    const auto result = reset_active_record_view_placement_cache(
        runtime,
        {.frame_index = 17, .worker_sequence = 4},
        "SetActiveRecord mode 1 complete workspace clear");

    EXPECT_EQ(result.status, ViewPlacementCacheMutationStatus::Applied);
    EXPECT_EQ(result.revision_before, 1u);
    EXPECT_EQ(result.revision_after, 2u);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Coherent);
    EXPECT_EQ(runtime.state.control, 0u);
    expect_key_bits_equal(runtime.state.key, ViewPlacementCacheKey{});
    EXPECT_EQ(runtime.state.angle_bits, 0u);
    EXPECT_EQ(runtime.state.last_source, ViewPlacementCacheSemanticSource::ActiveRecordReset);
    EXPECT_EQ(result.event.context.frame_index, 17u);
    EXPECT_EQ(result.event.context.worker_sequence, 4);
    EXPECT_EQ(result.event.state_before.control, 11u);
    EXPECT_EQ(result.event.state_after.control, 0u);
    EXPECT_NE(result.event.provenance.find("complete active-record workspace reset"),
        std::string::npos);
}

TEST(SavorPredictViewPlacementCacheModel, CompleteWorkspaceCopyPublishesEvenInvalidControl) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto copied = make_snapshot(
        3,
        make_key(90.0f, -4.0f, 0.0f, 22.0f),
        280.0f);

    const auto result = copy_view_placement_workspace_snapshot(
        runtime,
        copied,
        {.frame_index = 31, .worker_sequence = 9},
        "complete 0x17C worksheet copy");

    EXPECT_EQ(result.status, ViewPlacementCacheMutationStatus::Applied);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Coherent);
    EXPECT_EQ(runtime.state.control, 3u);
    expect_key_bits_equal(runtime.state.key, copied.key);
    EXPECT_EQ(runtime.state.angle_bits, copied.angle_bits);
    EXPECT_EQ(runtime.state.revision, 1u);
    EXPECT_EQ(runtime.state.last_source, ViewPlacementCacheSemanticSource::WorkspaceSnapshotCopy);
    EXPECT_EQ(result.event.context.frame_index, 31u);
    EXPECT_EQ(result.event.context.worker_sequence, 9);
}

TEST(SavorPredictViewPlacementCacheModel, IncompleteWorkspaceCopyMarksStateIndeterminate) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto prior = make_snapshot(
        10,
        make_key(45.0f, 1.0f, 0.0f, 2.0f),
        70.0f);
    ASSERT_EQ(publish_snapshot(runtime, prior).status, ViewPlacementCacheMutationStatus::Applied);

    const auto result = copy_view_placement_workspace_snapshot(
        runtime,
        std::nullopt,
        {.frame_index = 44},
        "source control field was unreadable");

    EXPECT_EQ(result.status, ViewPlacementCacheMutationStatus::MarkedIndeterminate);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Indeterminate);
    EXPECT_EQ(runtime.state.revision, 2u);
    EXPECT_EQ(runtime.state.control, prior.control);
    expect_key_bits_equal(runtime.state.key, prior.key);
    EXPECT_EQ(runtime.state.angle_bits, prior.angle_bits);
    EXPECT_NE(result.event.provenance.find("did not provide every cache field"),
        std::string::npos);
    EXPECT_NE(result.event.provenance.find("source control field was unreadable"),
        std::string::npos);
}

TEST(SavorPredictViewPlacementCacheModel, HookRegistryRejectsEmptyAndDuplicateSemanticIds) {
    ViewPlacementCacheHookRegistry hooks;
    const ViewPlacementCacheHookRegistry::ReadHook read_hook = [](
        const ViewPlacementCacheState&,
        const ViewPlacementCacheReadRequest&) {
        return ViewPlacementCacheReadDecision{
            .status = ViewPlacementCacheReadStatus::Unknown,
        };
    };
    const ViewPlacementCacheHookRegistry::MutationHook mutation_hook = [](
        const ViewPlacementCacheState&,
        const ViewPlacementCacheMutationRequest&) {
        return ViewPlacementCacheMutationOperation::no_change();
    };

    EXPECT_FALSE(hooks.register_read_hook("", read_hook));
    EXPECT_FALSE(hooks.register_read_hook(
        "null.read",
        ViewPlacementCacheHookRegistry::ReadHook{}));
    ASSERT_TRUE(hooks.register_read_hook("semantic.read", read_hook));
    EXPECT_FALSE(hooks.register_read_hook("semantic.read", read_hook));
    EXPECT_FALSE(hooks.register_mutation_hook("semantic.read", mutation_hook));
    ASSERT_TRUE(hooks.register_mutation_hook("semantic.write", mutation_hook));
    EXPECT_FALSE(hooks.register_mutation_hook("semantic.write", mutation_hook));
}

TEST(SavorPredictViewPlacementCacheModel, ReadHooksObserveImmutableStateWithoutChangingRevision) {
    ViewPlacementCacheRuntime runtime;
    runtime.state.control = 11;
    runtime.state.key = make_key(3.0f, 1.0f, 2.0f, 4.0f);
    runtime.state.angle_bits = std::bit_cast<std::uint32_t>(140.0f);
    runtime.state.knowledge = ViewPlacementCacheKnowledge::Coherent;
    runtime.state.revision = 7;
    ASSERT_TRUE(runtime.hooks.register_read_hook(
        "test.read",
        [](const ViewPlacementCacheState& state, const ViewPlacementCacheReadRequest&) {
            return ViewPlacementCacheReadDecision{
                .status = ViewPlacementCacheReadStatus::Hit,
                .angle_bits = state.angle_bits,
                .provenance = "const state observation",
            };
        }));

    const auto before = runtime.state;
    const auto result = read_view_placement_cache(
        runtime,
        "test.read",
        ViewPlacementCacheReadRequest{.key = runtime.state.key});

    EXPECT_EQ(result.status, ViewPlacementCacheReadStatus::Hit);
    EXPECT_EQ(runtime.state.control, before.control);
    expect_key_bits_equal(runtime.state.key, before.key);
    EXPECT_EQ(runtime.state.angle_bits, before.angle_bits);
    EXPECT_EQ(runtime.state.knowledge, before.knowledge);
    EXPECT_EQ(runtime.state.revision, before.revision);
    ASSERT_EQ(runtime.history.size(), 1u);
    EXPECT_EQ(runtime.history.front().kind, ViewPlacementCacheEventKind::Read);
}

TEST(SavorPredictViewPlacementCacheModel, CustomPublisherCanRecoverAfterIndeterminateWrite) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto unknown = mutate_view_placement_cache(
        runtime,
        "unresolved.overlapping_writer",
        ViewPlacementCacheMutationRequest{.provenance = "partial write observed"});
    ASSERT_EQ(unknown.status, ViewPlacementCacheMutationStatus::MarkedIndeterminate);
    ASSERT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Indeterminate);
    ASSERT_TRUE(runtime.hooks.register_mutation_hook(
        "test.recovery_publisher",
        [](const ViewPlacementCacheState&, const ViewPlacementCacheMutationRequest& request) {
            if (!request.snapshot.has_value()) {
                return ViewPlacementCacheMutationOperation::no_change("missing recovery snapshot");
            }
            return ViewPlacementCacheMutationOperation::publish_snapshot(
                *request.snapshot,
                "recovered from complete snapshot");
        }));

    const auto snapshot = make_snapshot(11, make_key(5.0f, 2.0f, 3.0f, 4.0f), 210.0f);
    const auto recovered = mutate_view_placement_cache(
        runtime,
        "test.recovery_publisher",
        ViewPlacementCacheMutationRequest{.snapshot = snapshot});

    EXPECT_EQ(recovered.status, ViewPlacementCacheMutationStatus::Applied);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Coherent);
    EXPECT_EQ(runtime.state.revision, 2u);
    EXPECT_EQ(runtime.state.control, snapshot.control);
    expect_key_bits_equal(runtime.state.key, snapshot.key);
    EXPECT_EQ(runtime.state.angle_bits, snapshot.angle_bits);
}

TEST(SavorPredictViewPlacementCacheModel, RuntimeCopiesOwnIndependentRegistryStateAndHistory) {
    auto first = make_default_view_placement_cache_runtime();
    auto second = first;
    ASSERT_TRUE(first.hooks.register_mutation_hook(
        "first.only",
        [](const ViewPlacementCacheState&, const ViewPlacementCacheMutationRequest&) {
            return ViewPlacementCacheMutationOperation::invalidate(0);
        }));
    ASSERT_FALSE(second.hooks.has_mutation_hook("first.only"));

    const auto mutation = mutate_view_placement_cache(
        first,
        "first.only",
        ViewPlacementCacheMutationRequest{.provenance = "first runtime only"});

    EXPECT_EQ(mutation.status, ViewPlacementCacheMutationStatus::Applied);
    EXPECT_EQ(first.state.knowledge, ViewPlacementCacheKnowledge::Coherent);
    EXPECT_EQ(first.state.revision, 1u);
    EXPECT_EQ(first.history.size(), 1u);
    EXPECT_EQ(second.state.knowledge, ViewPlacementCacheKnowledge::Uninitialized);
    EXPECT_EQ(second.state.revision, 0u);
    EXPECT_TRUE(second.history.empty());
}

TEST(SavorPredictViewPlacementCacheModel, CoherentMatchingKeyHitsAndRepublishesWithoutDraw) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto key = make_key(7.0f, -2.0f, 1.5f, 9.0f);
    const auto snapshot = make_snapshot(10, key, 280.0f);
    ASSERT_EQ(publish_snapshot(runtime, snapshot).status, ViewPlacementCacheMutationStatus::Applied);
    const auto before = runtime.state;
    std::uint32_t seed = 0x12345678u;

    const auto result = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{
            .key = key,
            .context = {.frame_index = 42, .worker_sequence = 7},
            .provenance = "matching request",
        });

    EXPECT_EQ(result.status, ViewPlacementCacheReadStatus::Hit);
    EXPECT_EQ(result.seed_before, 0x12345678u);
    EXPECT_EQ(result.seed_after, 0x12345678u);
    EXPECT_EQ(seed, 0x12345678u);
    EXPECT_EQ(result.draws_consumed, 0);
    EXPECT_FALSE(result.rand_value.has_value());
    ASSERT_TRUE(result.angle_bits.has_value());
    EXPECT_EQ(*result.angle_bits, snapshot.angle_bits);
    EXPECT_EQ(result.revision_before, 1u);
    EXPECT_EQ(result.revision_after, 2u);
    EXPECT_EQ(runtime.state.control, before.control);
    expect_key_bits_equal(runtime.state.key, before.key);
    EXPECT_EQ(runtime.state.angle_bits, before.angle_bits);
    ASSERT_EQ(result.events.size(), 2u);
    EXPECT_EQ(result.events[0].kind, ViewPlacementCacheEventKind::Read);
    EXPECT_EQ(result.events[1].kind, ViewPlacementCacheEventKind::Mutation);
    EXPECT_EQ(result.events[1].context.frame_index, 42u);
    EXPECT_EQ(result.events[1].context.worker_sequence, 7);
}

TEST(SavorPredictViewPlacementCacheModel, InvalidControlMissesAndConsumesExactlyOneDraw) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto old_key = make_key(4.0f, 1.0f, 2.0f, 3.0f);
    const auto request_key = make_key(4.0f, 1.0f, 2.0f, 3.0f);
    ASSERT_EQ(
        publish_snapshot(runtime, make_snapshot(0, old_key, 350.0f)).status,
        ViewPlacementCacheMutationStatus::Applied);
    std::uint32_t seed = 0x12345678u;
    const auto expected_draw = draw_rand15(seed);

    const auto result = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{.key = request_key});

    EXPECT_EQ(result.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(result.draws_consumed, 1);
    ASSERT_TRUE(result.rand_value.has_value());
    EXPECT_EQ(*result.rand_value, expected_draw.value);
    EXPECT_EQ(seed, expected_draw.next_state);
    EXPECT_EQ(result.seed_after, expected_draw.next_state);
    ASSERT_TRUE(result.angle_bits.has_value());
    EXPECT_EQ(*result.angle_bits, view_placement_angle_bits_from_rand15(expected_draw.value));
    EXPECT_EQ(runtime.state.control, 11);
    expect_key_bits_equal(runtime.state.key, request_key);
    EXPECT_EQ(runtime.state.angle_bits, *result.angle_bits);
}

TEST(SavorPredictViewPlacementCacheModel, ChangedKeyMissesAndConsumesExactlyOneDraw) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto cached_key = make_key(4.0f, 1.0f, 2.0f, 3.0f);
    const auto request_key = make_key(5.0f, 1.0f, 2.0f, 3.0f);
    ASSERT_EQ(
        publish_snapshot(runtime, make_snapshot(11, cached_key, 70.0f)).status,
        ViewPlacementCacheMutationStatus::Applied);
    std::uint32_t seed = 0x89ABCDEFu;
    const auto expected_draw = draw_rand15(seed);

    const auto result = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{.key = request_key});

    EXPECT_EQ(result.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(result.draws_consumed, 1);
    EXPECT_EQ(seed, expected_draw.next_state);
    expect_key_bits_equal(runtime.state.key, request_key);
}

TEST(SavorPredictViewPlacementCacheModel, UnknownAndMissingStatesDoNotAdvanceRngOrPublish) {
    const auto key = make_key(1.0f, 2.0f, 3.0f, 4.0f);

    auto uninitialized = make_default_view_placement_cache_runtime();
    std::uint32_t uninitialized_seed = 0x10203040u;
    const auto unknown = resolve_view_placement_request(
        uninitialized,
        uninitialized_seed,
        ViewPlacementRequest{.key = key});
    EXPECT_EQ(unknown.status, ViewPlacementCacheReadStatus::Unknown);
    EXPECT_EQ(unknown.draws_consumed, 0);
    EXPECT_EQ(uninitialized_seed, 0x10203040u);
    EXPECT_EQ(uninitialized.state.revision, 0u);

    auto missing_runtime = make_default_view_placement_cache_runtime();
    std::uint32_t missing_seed = 0x50607080u;
    const auto missing = resolve_view_placement_request(
        missing_runtime,
        missing_seed,
        ViewPlacementRequest{});
    EXPECT_EQ(missing.status, ViewPlacementCacheReadStatus::MissingInput);
    EXPECT_EQ(missing.draws_consumed, 0);
    EXPECT_EQ(missing_seed, 0x50607080u);
    EXPECT_EQ(missing_runtime.state.revision, 0u);

    auto indeterminate = make_default_view_placement_cache_runtime();
    ASSERT_EQ(
        mutate_view_placement_cache(
            indeterminate,
            "unresolved.writer",
            ViewPlacementCacheMutationRequest{}).status,
        ViewPlacementCacheMutationStatus::MarkedIndeterminate);
    const auto revision_before = indeterminate.state.revision;
    std::uint32_t indeterminate_seed = 0x90A0B0C0u;
    const auto unresolved = resolve_view_placement_request(
        indeterminate,
        indeterminate_seed,
        ViewPlacementRequest{.key = key});
    EXPECT_EQ(unresolved.status, ViewPlacementCacheReadStatus::Unknown);
    EXPECT_EQ(unresolved.draws_consumed, 0);
    EXPECT_EQ(indeterminate_seed, 0x90A0B0C0u);
    EXPECT_EQ(indeterminate.state.revision, revision_before);
}

TEST(SavorPredictViewPlacementCacheModel, UnsupportedHooksAreTransactionalAndDoNotConsumeRng) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto key = make_key(1.0f, 2.0f, 3.0f, 4.0f);
    std::uint32_t seed = 0x12345678u;

    const auto direct_read = read_view_placement_cache(
        runtime,
        "unknown.reader",
        ViewPlacementCacheReadRequest{.key = key});
    EXPECT_EQ(direct_read.status, ViewPlacementCacheReadStatus::Unsupported);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Uninitialized);
    EXPECT_EQ(runtime.state.revision, 0u);

    const auto before_history = runtime.history.size();
    const auto unsupported_publisher = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{
            .publisher_source_id = "unknown.publisher",
            .key = key,
        });
    EXPECT_EQ(unsupported_publisher.status, ViewPlacementCacheReadStatus::Unsupported);
    EXPECT_EQ(unsupported_publisher.draws_consumed, 0);
    EXPECT_EQ(seed, 0x12345678u);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Uninitialized);
    EXPECT_EQ(runtime.state.revision, 0u);
    EXPECT_EQ(runtime.history.size(), before_history);
}

TEST(SavorPredictViewPlacementCacheModel, SoaFloatComparisonHandlesZerosNanAndFiniteValues) {
    const auto finite = make_key(3.5f, -2.0f, 0.25f, 100.0f);
    EXPECT_TRUE(view_placement_cache_keys_equal(finite, finite));

    auto negative_zero = finite;
    negative_zero.center_y_bits = 0x80000000u;
    auto positive_zero = finite;
    positive_zero.center_y_bits = 0x00000000u;
    EXPECT_TRUE(view_placement_cache_keys_equal(positive_zero, negative_zero));

    auto nan_key = finite;
    nan_key.center_x_bits = 0x7FC00000u;
    EXPECT_FALSE(view_placement_cache_keys_equal(nan_key, nan_key));

    auto unequal = finite;
    unequal.distance_bits = std::bit_cast<std::uint32_t>(3.75f);
    EXPECT_FALSE(view_placement_cache_keys_equal(finite, unequal));
}

TEST(SavorPredictViewPlacementCacheModel, LiveEvidenceRandVectorsMapToValidatedAngles) {
    EXPECT_EQ(view_placement_angle_bits_from_rand15(0x2252), 0x430C0000u);
    EXPECT_EQ(view_placement_angle_bits_from_rand15(0x4D4D), 0x428C0000u);
    EXPECT_EQ(view_placement_angle_bits_from_rand15(0x651C), 0x42340000u);
}

TEST(SavorPredictViewPlacementCacheModel, MissMissRepeatedKeyHitConsumesOneOneZeroDraws) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto first_key = make_key(10.0f, 1.0f, 2.0f, 3.0f);
    const auto second_key = make_key(20.0f, 4.0f, 5.0f, 6.0f);
    ASSERT_EQ(
        publish_snapshot(runtime, make_snapshot(0, first_key, 45.0f)).status,
        ViewPlacementCacheMutationStatus::Applied);
    std::uint32_t seed = 0xCAFEBABEu;
    const auto expected_first = draw_rand15(seed);
    const auto expected_second = draw_rand15(expected_first.next_state);

    const auto first = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{.key = first_key});
    const auto second = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{.key = second_key});
    const auto repeated = resolve_view_placement_request(
        runtime,
        seed,
        ViewPlacementRequest{.key = second_key});

    EXPECT_EQ(first.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(second.status, ViewPlacementCacheReadStatus::Miss);
    EXPECT_EQ(repeated.status, ViewPlacementCacheReadStatus::Hit);
    EXPECT_EQ(first.draws_consumed, 1);
    EXPECT_EQ(second.draws_consumed, 1);
    EXPECT_EQ(repeated.draws_consumed, 0);
    EXPECT_EQ(seed, expected_second.next_state);
    EXPECT_EQ(runtime.state.revision, 4u);
}

TEST(SavorPredictViewPlacementCacheModel, UnknownWriterRetainsRawFieldsAndRecordsIndeterminateProvenance) {
    auto runtime = make_default_view_placement_cache_runtime();
    const auto snapshot = make_snapshot(
        11,
        make_key(8.0f, -1.0f, -2.0f, -3.0f),
        350.0f);
    ASSERT_EQ(publish_snapshot(runtime, snapshot).status, ViewPlacementCacheMutationStatus::Applied);

    const auto result = mutate_view_placement_cache(
        runtime,
        "unresolved.overlapping_writer",
        ViewPlacementCacheMutationRequest{
            .context = {.frame_index = 91, .worker_sequence = 12},
            .provenance = "control write overlapped publication",
        });

    EXPECT_EQ(result.status, ViewPlacementCacheMutationStatus::MarkedIndeterminate);
    EXPECT_EQ(runtime.state.knowledge, ViewPlacementCacheKnowledge::Indeterminate);
    EXPECT_EQ(runtime.state.revision, 2u);
    EXPECT_EQ(runtime.state.control, snapshot.control);
    expect_key_bits_equal(runtime.state.key, snapshot.key);
    EXPECT_EQ(runtime.state.angle_bits, snapshot.angle_bits);
    EXPECT_EQ(runtime.state.last_source, "unresolved.overlapping_writer");
    EXPECT_NE(runtime.state.provenance.find("unknown mutation source"), std::string::npos);
    EXPECT_NE(runtime.state.provenance.find("control write overlapped"), std::string::npos);
    EXPECT_EQ(result.event.context.frame_index, 91u);
    EXPECT_EQ(result.event.context.worker_sequence, 12);
}

} // namespace
