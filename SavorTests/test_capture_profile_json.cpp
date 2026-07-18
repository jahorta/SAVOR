#include <gtest/gtest.h>

#include "CaptureProfileJson.h"
#include "LiveCaptureProfile.h"
#include "ProbeProfile.h"

#include <algorithm>
#include <array>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace savor::predict;
using namespace savor::probe;

TEST(SavorCaptureProfileJson, EveryTrackedProfileBuildsAsTheNewSchema)
{
    const std::vector<std::pair<std::string, std::function<std::string()>>> builders{
        { "rng", [] { return build_first_battle_capture_profile_ini(); } },
        { "predictor_validation", [] { return build_first_battle_predictor_validation_profile_ini(); } },
        { "turn_order", [] { return build_first_battle_turn_order_validation_profile_ini(); } },
        { "field6", [] { return build_first_battle_field6_watch_profile_ini(); } },
        { "view_eligibility", [] { return build_first_battle_view_eligibility_profile_ini(); } },
        { "view_cache", [] { return build_first_battle_view_placement_cache_profile_ini(); } },
        { "view_frame_thread", [] { return build_first_battle_view_placement_frame_thread_profile_ini(); } },
        { "view_semantic_hooks", [] { return build_first_battle_view_placement_semantic_hooks_profile_ini(); } },
        { "predictor_comparison", [] { return build_first_battle_predictor_live_comparison_profile_ini(); } },
        { "movement_destination", [] { return build_first_battle_movement_destination_stop_profile_ini(); } },
        { "action_view_lifecycle", [] { return build_first_battle_action_view_service_lifecycle_profile_ini(); } },
        { "visual_publication", [] { return build_first_battle_visual_publication_order_profile_ini(); } },
        { "pc_worker", [] { return build_first_battle_pc_worker_selector_lifetime_profile_ini(); } },
        { "queued_instruction", [] { return build_first_battle_queued_instruction_param_profile_ini(); } },
        { "mode1_pathing", [] { return build_first_battle_mode1_pathing_lifetime_profile_ini(); } },
        { "mode1_state6", [] { return build_first_battle_mode1_state6_progress_profile_ini(); } },
        { "pathing_loop", [] { return build_first_battle_action_view_pathing_loop_profile_ini(); } },
        { "thread_pathing", [] { return build_first_battle_thread_pathing_timing_profile_ini(); } },
        { "thread_producer", [] { return build_battle_thread_producer_profile_ini(); } },
        { "action_resource", [] { return build_first_battle_action_view_resource_profile_ini(); } },
        { "selector_coverage", [] { return build_first_battle_action_view_selector_coverage_profile_ini(); } },
        { "thread_list", [] { return build_first_battle_thread_list_profile_ini(); } },
        { "pre_handler_pathing", [] { return build_first_battle_pre_handler_frame_pathing_profile_ini(); } },
        { "float_motion", [] { return build_first_battle_float_motion_profile_ini(); } },
        { "move_increment", [] { return build_first_battle_move_increment_read_watch_profile_ini(); } },
        { "probe_layer_validation", [] { return build_first_battle_probe_layer_validation_profile_ini(); } },
        { "action_motion_invocation", [] { return build_first_battle_action_motion_invocation_profile_ini(); } },
    };

    for (const auto& [name, build] : builders) {
        SCOPED_TRACE(name);
        std::string json;
        ASSERT_NO_THROW(json = build());
        const auto parsed = parse_profile_json(json);
        ASSERT_TRUE(parsed.profile.has_value()) << format_profile_errors(parsed);
        EXPECT_EQ(parsed.profile->schema, "savor.capture.profile/1");
        EXPECT_FALSE(parsed.profile->name.empty());
        EXPECT_FALSE(parsed.profile->probes.empty());
    }
}

TEST(SavorCaptureProfileJson, ActionMotionInvocationProfileCoversAttributionAndLifetime)
{
    const auto parsed = parse_profile_json(
        build_first_battle_action_motion_invocation_profile_ini(128));
    ASSERT_TRUE(parsed.profile.has_value()) << format_profile_errors(parsed);
    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_action_motion_invocation");
    EXPECT_EQ(profile.limits.queue_bytes, 128ull * 1024ull * 1024ull);
    EXPECT_EQ(profile.limits.max_events, 2048u);
    EXPECT_EQ(profile.limits.chunk_events, 2048u);

    const auto rng = std::ranges::find(
        profile.probes, std::string("rng_seed_write_803469A8"), &ProbeDefinition::id);
    ASSERT_NE(rng, profile.probes.end());
    EXPECT_EQ(rng->address, 0x803469A8u);
    EXPECT_FALSE(rng->activate_on_pc.has_value());
    EXPECT_TRUE(rng->owns_rng_draw);
    const auto rng_stack = std::ranges::find_if(rng->samples, [](const auto& sample) {
        return sample.kind == SampleKind::StackTrace;
    });
    ASSERT_NE(rng_stack, rng->samples.end());
    EXPECT_EQ(rng_stack->max_frames, 8u);

    const auto dynamic_count = std::ranges::count_if(profile.probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Memory
            && probe.id.starts_with("root") && probe.id.find("_iw_") != std::string::npos;
    });
    EXPECT_EQ(dynamic_count, 40);
    for (const auto root_index : { 0, 1, 2, 3 }) {
        const auto prefix = "root" + std::to_string(root_index) + "_iw_";
        EXPECT_EQ(std::ranges::count_if(profile.probes, [&](const auto& probe) {
            return probe.id.starts_with(prefix);
        }), 10) << "root " << root_index;
    }
    for (const auto& probe : profile.probes) {
        if (!(probe.kind == ProbeKind::Memory
              && probe.id.starts_with("root")
              && probe.id.find("_iw_") != std::string::npos)) {
            continue;
        }
        ASSERT_TRUE(probe.activate_on_pc.has_value());
        EXPECT_EQ(*probe.activate_on_pc, 0x800715ECu);
        EXPECT_EQ(probe.address_trace, AddressTracePolicy::OnFailure);
        EXPECT_TRUE(validate_address_program(probe.address_program).valid);
        EXPECT_EQ(probe.window_id, "instruction_lifetime");
        EXPECT_TRUE(probe.size == 2 || probe.size == 4 || probe.size == 8);
        const auto stack = std::ranges::find_if(probe.samples, [](const auto& sample) {
            return sample.kind == SampleKind::StackTrace;
        });
        ASSERT_NE(stack, probe.samples.end());
        EXPECT_EQ(stack->max_frames, 8u);
    }

    EXPECT_EQ(std::ranges::count_if(profile.probes, [](const auto& probe) {
        return probe.id.starts_with("combatant_thread_roots_");
    }), 6);
    EXPECT_EQ(std::ranges::count_if(profile.probes, [](const auto& probe) {
        return probe.id.starts_with("action_motion_resolver_return_");
    }), 24);
    EXPECT_EQ(std::ranges::count_if(profile.probes, [](const auto& probe) {
        return probe.id.starts_with("action_motion_install_caller_return_");
    }), 17);

    const auto macro = std::ranges::find(
        profile.probes, std::string("input_macro_command_diagnostic_800798C4"),
        &ProbeDefinition::id);
    ASSERT_NE(macro, profile.probes.end());
    EXPECT_TRUE(has_subscription(macro->subscriptions, Subscription::Capture));
    EXPECT_FALSE(has_subscription(macro->subscriptions, Subscription::Control));

    const auto window = std::ranges::find(
        profile.windows, std::string("instruction_lifetime"), &WindowDefinition::id);
    ASSERT_NE(window, profile.windows.end());
    EXPECT_EQ(window->open_probe, "setup_turn_end_800715EC");
    EXPECT_FALSE(window->initially_open);

    const auto frames = std::ranges::count_if(profile.probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Pc && probe.address == 0x8000A2FCu;
    });
    EXPECT_EQ(frames, 3);
    const auto frame_clock = std::ranges::find_if(profile.probes, [](const auto& probe) {
        return probe.address == 0x8000A2FCu && probe.frame_clock;
    });
    ASSERT_NE(frame_clock, profile.probes.end());
    ASSERT_TRUE(frame_clock->max_hits.has_value());
    EXPECT_EQ(*frame_clock->max_hits, 2400u);
    const auto changed_list = std::ranges::find(
        profile.probes, std::string("battle_case5_thread_list_changed_8000A2FC"),
        &ProbeDefinition::id);
    ASSERT_NE(changed_list, profile.probes.end());
    EXPECT_EQ(changed_list->sampling.mode, SamplingMode::ChangedOnly);
    const auto list_samples = std::ranges::count_if(profile.probes, [](const auto& probe) {
        return std::ranges::any_of(probe.samples, [](const auto& sample) {
            return sample.kind == SampleKind::LinkedList && sample.max_nodes == 128;
        });
    });
    EXPECT_EQ(list_samples, 2);

    ASSERT_EQ(profile.flight_recorders.size(), 1u);
    const auto& recorder = profile.flight_recorders.front();
    EXPECT_EQ(recorder.pre_events, 3u);
    EXPECT_EQ(recorder.post_events, 4u);
    EXPECT_EQ(recorder.member_probes,
        std::vector<std::string>{ "battle_case5_thread_list_flight_8000A2FC" });
    EXPECT_NE(std::ranges::find(
        recorder.trigger_probes, "action_motion_install_entry_8001EBA4"),
        recorder.trigger_probes.end());
}

TEST(SavorCaptureProfileJson, ValidationProfileExercisesDynamicRootsSharedSubscriptionsAndFlightRecorder)
{
    const auto parsed = parse_profile_json(build_first_battle_probe_layer_validation_profile_ini());
    ASSERT_TRUE(parsed.profile.has_value()) << format_profile_errors(parsed);
    const auto& profile = *parsed.profile;
    const auto frame = std::ranges::find(
        profile.probes, std::string("battle_case5_after_threads_8000A2FC"),
        &ProbeDefinition::id);
    ASSERT_NE(frame, profile.probes.end());
    EXPECT_TRUE(frame->frame_clock);
    const auto list = std::ranges::find_if(frame->samples, [](const auto& sample) {
        return sample.kind == SampleKind::LinkedList;
    });
    ASSERT_NE(list, frame->samples.end());
    EXPECT_FALSE(list->address_provided);
    EXPECT_FALSE(list->address_program.empty());
    EXPECT_EQ(list->max_nodes, 128u);

    const auto rng = std::ranges::find(
        profile.probes, std::string("rng_seed_write_803469A8"), &ProbeDefinition::id);
    ASSERT_NE(rng, profile.probes.end());
    EXPECT_EQ(rng->address_trace, AddressTracePolicy::OnFailure);
    ASSERT_TRUE(rng->activate_on_pc.has_value());
    EXPECT_EQ(*rng->activate_on_pc, 0x80070A54u);
    EXPECT_EQ(rng->size, 4u);
    EXPECT_EQ(rng->memory_access, MemoryAccess::Write);
    EXPECT_FALSE(rng->address_program.empty());
    EXPECT_TRUE(validate_address_program(rng->address_program).valid);
    const auto stack = std::ranges::find_if(rng->samples, [](const auto& sample) {
        return sample.kind == SampleKind::StackTrace;
    });
    ASSERT_NE(stack, rng->samples.end());
    EXPECT_EQ(stack->max_frames, 8u);

    const auto shared = std::ranges::find(
        profile.probes, std::string("turn_input_shared_80070A54"), &ProbeDefinition::id);
    ASSERT_NE(shared, profile.probes.end());
    EXPECT_TRUE(has_subscription(shared->subscriptions, Subscription::Capture));
    EXPECT_TRUE(has_subscription(shared->subscriptions, Subscription::Progress));
    EXPECT_TRUE(has_subscription(shared->subscriptions, Subscription::Control));
    ASSERT_EQ(profile.flight_recorders.size(), 1u);
    EXPECT_EQ(profile.flight_recorders.front().pre_events, 2u);
    EXPECT_EQ(profile.flight_recorders.front().post_events, 4u);
    EXPECT_NE(std::ranges::find(
        profile.flight_recorders.front().member_probes,
        "battle_case5_after_threads_8000A2FC"),
        profile.flight_recorders.front().member_probes.end());
    EXPECT_EQ(std::ranges::find(
        profile.flight_recorders.front().member_probes,
        "rng_seed_write_803469A8"),
        profile.flight_recorders.front().member_probes.end());
}

TEST(SavorCaptureProfileJson, PreservesBoundedFrameListAndRngStackSemantics)
{
    const auto frame_result = parse_profile_json(
        build_first_battle_view_placement_frame_thread_profile_ini(128));
    ASSERT_TRUE(frame_result.profile.has_value()) << format_profile_errors(frame_result);

    const auto frame = std::ranges::find_if(frame_result.profile->probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Pc && probe.address == 0x8000A2FCu;
    });
    ASSERT_NE(frame, frame_result.profile->probes.end());
    EXPECT_TRUE(frame->frame_clock);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    const auto list = std::ranges::find_if(frame->samples, [](const auto& sample) {
        return sample.kind == SampleKind::LinkedList;
    });
    ASSERT_NE(list, frame->samples.end());
    EXPECT_EQ(list->max_nodes, 128u);
    EXPECT_TRUE(list->address != 0 || !list->address_program.empty());

    const auto rng_result = parse_profile_json(build_first_battle_capture_profile_ini());
    ASSERT_TRUE(rng_result.profile.has_value()) << format_profile_errors(rng_result);
    const auto rng_probe = std::ranges::find_if(rng_result.profile->probes, [](const auto& probe) {
        return probe.owns_rng_draw;
    });
    ASSERT_NE(rng_probe, rng_result.profile->probes.end());
    const auto stack = std::ranges::find_if(rng_probe->samples, [](const auto& sample) {
        return sample.kind == SampleKind::StackTrace;
    });
    ASSERT_NE(stack, rng_probe->samples.end());
    EXPECT_EQ(stack->max_frames, 8u);
}

TEST(SavorCaptureProfileJson, ConvertsMacroWindowsDeferredActivationAndDynamicRegisterRoots)
{
    const auto result = parse_profile_json(build_first_battle_queued_instruction_param_profile_ini());
    ASSERT_TRUE(result.profile.has_value()) << format_profile_errors(result);
    const auto window = std::ranges::find(
        result.profile->windows, std::string("input_macro"), &WindowDefinition::id);
    ASSERT_NE(window, result.profile->windows.end());
    EXPECT_EQ(window->open_marker, "macro.begin");
    EXPECT_EQ(window->close_marker, "macro.end");

    const auto macro_watch = std::ranges::find_if(result.profile->probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Memory && probe.window_id == "input_macro";
    });
    ASSERT_NE(macro_watch, result.profile->probes.end());
    const auto deferred = std::ranges::find_if(result.profile->probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Pc && probe.activate_on_pc.has_value();
    });
    ASSERT_NE(deferred, result.profile->probes.end());

    const auto field6 = parse_profile_json(build_first_battle_field6_watch_profile_ini());
    ASSERT_TRUE(field6.profile.has_value()) << format_profile_errors(field6);
    const auto dynamic = std::ranges::find_if(field6.profile->probes, [](const auto& probe) {
        return probe.kind == ProbeKind::Memory && probe.activate_on_pc.has_value()
            && !probe.address_program.empty();
    });
    ASSERT_NE(dynamic, field6.profile->probes.end());
}

TEST(SavorCaptureProfileJson, PinsAndValidatesTheWorkerModuleIdentity)
{
    constexpr std::string_view hash =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    const auto pinned = pin_capture_profile_module_hash(
        build_first_battle_capture_profile_ini(), hash);
    const auto result = parse_profile_json(pinned);
    ASSERT_TRUE(result.profile.has_value()) << format_profile_errors(result);
    EXPECT_EQ(result.profile->expected_module_sha256, hash);

    EXPECT_THROW(
        pin_capture_profile_module_hash(build_first_battle_capture_profile_ini(), "short"),
        std::runtime_error);
}

TEST(SavorCaptureProfileJson, RejectsTheRemovedBuilderTraceBoolean)
{
    EXPECT_THROW(
        build_capture_profile_json(R"(
          [profile]
          name=removed-trace
          [checkpoint.test]
          pc=0x80001000
          address_program_trace=true
        )"),
        std::runtime_error);
}

} // namespace
