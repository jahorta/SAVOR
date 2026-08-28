#include <gtest/gtest.h>

#include "Execution/ProgramDB/WorksetObservationBinding.h"
#include "Runner/Runtime/Progress/ProgressTypes.h"
#include "Runner/Runtime/Worksets/WorksetWireCodec.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace savor::runtime::progress;

TEST(ProgressTypes, ProductionRegistryIsDeterministicAndComplete)
{
    const ProgressLibraryRegistry first;
    const ProgressLibraryRegistry second;
    EXPECT_EQ(first.libraries(), second.libraries());
    EXPECT_EQ(first.canonical_sha256(), second.canonical_sha256());
    EXPECT_EQ(first.libraries().size(), 5u);
    for (const auto& library : first.libraries())
        EXPECT_TRUE(static_cast<bool>(library));
}

TEST(ProgressTypes, ResolvesCanonicalPlansAndBindsRuntimeTriggers)
{
    constexpr std::array<std::string_view, 2> runtime_libraries{
        "soa.progress.runtime.vi/1",
        "soa.progress.soa.script_location/1"};
    constexpr std::array<std::uint32_t, 3> triggers{
        0x80101e48u,
        0x800715dcu,
        0x80101e48u};
    const ProgressPlanV1 runtime = ResolveProgressPlanV1(
        runtime_libraries,
        triggers);
    ASSERT_TRUE(ValidateProgressPlanV1(runtime));
    ASSERT_EQ(runtime.points.size(), 2u);
    for (const auto& point : runtime.points)
    {
        EXPECT_EQ(
            point.runtime_sample_trigger_pcs,
            (std::vector<std::uint32_t>{
                0x800715dcu,
                0x80101e48u}));
    }

    constexpr std::array<std::string_view, 2> battle_libraries{
        "soa.progress.battle.events/1",
        "soa.progress.predicate.evaluations/1"};
    const ProgressPlanV1 battle = ResolveProgressPlanV1(
        battle_libraries);
    ASSERT_TRUE(ValidateProgressPlanV1(battle));
    EXPECT_EQ(battle.points.size(), 6u);
    EXPECT_TRUE(std::ranges::all_of(
        battle.points,
        [](const ProgressPointBindingV1& point) {
            return point.runtime_sample_trigger_pcs.empty();
        }));

    auto tampered = battle;
    tampered.points.front().library_sha256.front() = '0';
    EXPECT_FALSE(ValidateProgressPlanV1(tampered));
}

TEST(ProgressTypes, ResolvesProgramDefaultsWithExplicitPlanningDeltas)
{
    const std::vector<std::string> defaults{
        "soa.progress.battle.events/1",
        "soa.progress.predicate.evaluations/1",
    };
    ProgressPlanSelectionV1 selection{
        .disabled_default_library_ids = {
            "soa.progress.predicate.evaluations/1",
        },
        .disabled_points = {{
            .library_id = "soa.progress.battle.events/1",
            .point_id = "item_drop",
        }},
        .added_library_ids = {
            "soa.progress.runtime.vi/1",
        },
    };
    const std::array<std::uint32_t, 1> triggers{0x80101e48u};
    const auto resolved = ResolveProgressPlanSelectionV1(
        defaults,
        triggers,
        selection);
    ASSERT_TRUE(resolved) << resolved.message;
    ASSERT_TRUE(resolved.plan.has_value());
    EXPECT_TRUE(ValidateProgressPlanV1(*resolved.plan));
    EXPECT_FALSE(std::ranges::any_of(
        resolved.plan->points,
        [](const ProgressPointBindingV1& point) {
            return point.library_id ==
                "soa.progress.predicate.evaluations/1" ||
                point.point_id == "item_drop";
        }));
    const auto vi = std::ranges::find_if(
        resolved.plan->points,
        [](const ProgressPointBindingV1& point) {
            return point.library_id == "soa.progress.runtime.vi/1";
        });
    ASSERT_NE(vi, resolved.plan->points.end());
    EXPECT_EQ(
        vi->runtime_sample_trigger_pcs,
        (std::vector<std::uint32_t>{0x80101e48u}));

    auto invalid = selection;
    invalid.disabled_default_library_ids.push_back(
        "soa.progress.runtime.vi/1");
    EXPECT_FALSE(ResolveProgressPlanSelectionV1(
        defaults,
        triggers,
        invalid));
}

TEST(ProgressTypes, CoordinatorResolvesAndEncodesOneExactWorksetPlan)
{
    using namespace savor::db::execution::programdb;
    ProgramJobMaterializationContext context;
    context.graph.emplace();
    context.graph->arguments = {
        {
            .argument_key =
                std::string(kProgressDisableDefaultLibrariesArgument),
            .value_type = "text",
            .text_value = "soa.progress.predicate.evaluations/1",
        },
        {
            .argument_key =
                std::string(kProgressAddLibrariesArgument),
            .value_type = "text",
            .text_value = "soa.progress.runtime.vi/1",
        },
    };
    const WorksetObservationDefaultsV1 defaults{
        .progress_library_ids = {
            "soa.progress.battle.events/1",
            "soa.progress.predicate.evaluations/1",
        },
        .runtime_sample_trigger_pcs = {0x80101e48u},
    };
    ResolvedWorksetObservationBindingV1 binding;
    std::string error;
    ASSERT_TRUE(ResolveWorksetObservationBindingV1(
        context,
        defaults,
        std::filesystem::path("capture-output"),
        &binding,
        &error)) << error;
    EXPECT_FALSE(binding.capture.has_value());
    EXPECT_FALSE(binding.encoded_capture_binding.empty());
    EXPECT_FALSE(binding.encoded_progress_plan.empty());
    EXPECT_EQ(
        binding.capture_binding_sha256,
        savor::runtime::EmptyWorksetCaptureBindingHashV1());

    savor::runtime::progress::ProgressPlanV1 decoded;
    ASSERT_TRUE(savor::runtime::DecodeProgressPlanV1(
        binding.encoded_progress_plan,
        decoded));
    EXPECT_EQ(decoded, binding.progress_plan);
    EXPECT_FALSE(std::ranges::any_of(
        decoded.points,
        [](const ProgressPointBindingV1& point) {
            return point.library_id ==
                "soa.progress.predicate.evaluations/1";
        }));
}

TEST(ProgressTypes, ExpertProgressProbeRequiresRequestedRegisteredFormatter)
{
    const auto& registry = ProductionProgressRegistry();
    const auto* point = registry.FindPoint(
        "soa.progress.battle.events/1",
        1,
        "attack_damage");
    ASSERT_NE(point, nullptr);

    constexpr std::array<std::string_view, 1> libraries{
        "soa.progress.battle.events/1"};
    const ProgressPlanV1 requested = ResolveProgressPlanV1(libraries);
    const auto binding = std::ranges::find_if(
        requested.points,
        [](const ProgressPointBindingV1& candidate) {
            return candidate.point_id == "attack_damage";
        });
    ASSERT_NE(binding, requested.points.end());
    auto registered_probe = BuildBreakpointProgressProbeV1(*binding);
    ASSERT_TRUE(registered_probe.has_value());

    savor::probe::Profile profile;
    profile.name = "expert-progress";
    registered_probe->id = "attack-damage";
    profile.probes.push_back(std::move(*registered_probe));
    EXPECT_TRUE(ValidateCaptureProgressFormatters(
        profile,
        &requested));

    auto missing_field = profile;
    missing_field.probes.front().samples.pop_back();
    EXPECT_FALSE(ValidateCaptureProgressFormatters(
        missing_field,
        &requested));

    const ProgressPlanV1 empty;
    EXPECT_FALSE(ValidateCaptureProgressFormatters(profile, &empty));
    profile.probes.front().progress_formatter = "unregistered.formatter";
    EXPECT_FALSE(ValidateCaptureProgressFormatters(
        profile,
        &requested));
}

TEST(ProgressTypes, BattleProvidersCarryTheResearchedTypedObservations)
{
    constexpr std::array<std::string_view, 1> libraries{
        "soa.progress.battle.events/1"};
    const ProgressPlanV1 plan = ResolveProgressPlanV1(libraries);
    const auto binding = [&](std::string_view id) {
        return std::ranges::find_if(
            plan.points,
            [&](const ProgressPointBindingV1& point) {
                return point.point_id == id;
            });
    };

    const auto attack = binding("attack_damage");
    ASSERT_NE(attack, plan.points.end());
    const auto attack_probe = BuildBreakpointProgressProbeV1(*attack);
    ASSERT_TRUE(attack_probe.has_value());
    ASSERT_EQ(attack_probe->samples.size(), 5u);
    EXPECT_EQ(attack_probe->samples[0].name, "attacker_slot");
    EXPECT_EQ(attack_probe->samples[4].name, "target_id");

    savor::capture_format::Event event;
    event.pc = 0x80081d04u;
    event.fields = {
        {.name = "attacker_slot", .value = 0},
        {.name = "target_slot", .value = 4},
        {.name = "damage", .value = 123},
        {.name = "attacker_id", .value = 0},
        {.name = "target_id", .value = 0},
    };
    const auto* descriptor = ProductionProgressRegistry().FindPoint(
        attack->library_id,
        attack->library_revision,
        attack->point_id);
    ASSERT_NE(descriptor, nullptr);
    const std::string text = FormatCaptureProgressText(*descriptor, event);
    EXPECT_NE(text.find(" attacks "), std::string::npos);
    EXPECT_NE(text.find("123 damage"), std::string::npos);

    const auto instructions = binding("instruction_turn_state");
    ASSERT_NE(instructions, plan.points.end());
    const auto instruction_probe =
        BuildBreakpointProgressProbeV1(*instructions);
    ASSERT_TRUE(instruction_probe.has_value());
    EXPECT_EQ(instruction_probe->samples.size(), 60u);
}

TEST(ProgressTypes, SeedCallProviderCapturesSynchronousCallEvidence)
{
    constexpr std::array<std::string_view, 1> libraries{
        "soa.progress.soa.seed_calls/1"};
    const ProgressPlanV1 plan = ResolveProgressPlanV1(libraries);
    ASSERT_EQ(plan.points.size(), 1u);
    const auto probe = BuildBreakpointProgressProbeV1(plan.points.front());
    ASSERT_TRUE(probe.has_value());
    EXPECT_EQ(probe->address, 0x8025ecbcu);
    ASSERT_EQ(probe->samples.size(), 4u);
    EXPECT_EQ(probe->samples[0].kind, savor::probe::SampleKind::Gpr);
    EXPECT_EQ(probe->samples[1].kind, savor::probe::SampleKind::Memory);
    EXPECT_EQ(probe->samples[2].kind, savor::probe::SampleKind::RoutedSample);
    EXPECT_EQ(probe->samples[3].kind, savor::probe::SampleKind::StackTrace);
    EXPECT_EQ(probe->samples[3].max_frames, 4u);
}

} // namespace
