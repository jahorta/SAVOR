#include <QueuedInstructionParamModel.h>

#include <gtest/gtest.h>

namespace savor::predict {
namespace {

TEST(SavorPredictQueuedInstructionParam, InitializesAndResetsRowsToMinusOne) {
    for (const auto command : {
             QueuedInstructionCommandKind::Initialize,
             QueuedInstructionCommandKind::Reset}) {
        const auto result = model_queued_instruction_command({.command = command});
        EXPECT_EQ(result.instruction, std::optional<std::int16_t>{-1});
        EXPECT_EQ(result.parameter.raw_value, std::optional<std::int16_t>{-1});
        EXPECT_EQ(result.parameter.kind, QueuedInstructionParamKind::Unset);
        EXPECT_EQ(result.parameter.stage, QueuedInstructionParamStage::Initialized);
        EXPECT_EQ(result.parameter.status, QueuedInstructionParamStatus::Validated);
    }
}

TEST(SavorPredictQueuedInstructionParam, PcAttackMenuUsesMovementFlag40) {
    const auto direct = model_queued_instruction_command({
        .command = QueuedInstructionCommandKind::Attack,
        .movement_flags = 0x40,
    });
    EXPECT_EQ(direct.instruction, std::optional<std::int16_t>{3});
    EXPECT_EQ(direct.parameter.raw_value, std::optional<std::int16_t>{0});
    EXPECT_EQ(direct.parameter.kind, QueuedInstructionParamKind::BasicAttackRoute);

    const auto fallback = model_queued_instruction_command({
        .command = QueuedInstructionCommandKind::Attack,
        .movement_flags = 0,
    });
    EXPECT_EQ(fallback.parameter.raw_value, std::optional<std::int16_t>{1});

    const auto missing = model_queued_instruction_command({
        .command = QueuedInstructionCommandKind::Attack,
    });
    EXPECT_FALSE(missing.parameter.raw_value.has_value());
    EXPECT_EQ(missing.parameter.status, QueuedInstructionParamStatus::MissingInput);
}

TEST(SavorPredictQueuedInstructionParam, CommandIdsKeepTheirSemanticKinds) {
    struct Vector {
        QueuedInstructionCommandKind command;
        std::int16_t instruction;
        QueuedInstructionParamKind kind;
    };
    for (const auto& vector : {
             Vector{QueuedInstructionCommandKind::Magic, 1, QueuedInstructionParamKind::AbilityId},
             Vector{QueuedInstructionCommandKind::SMove, 2, QueuedInstructionParamKind::AbilityId},
             Vector{QueuedInstructionCommandKind::Crew, 8, QueuedInstructionParamKind::AbilityId},
             Vector{QueuedInstructionCommandKind::Item, 5, QueuedInstructionParamKind::ItemId}}) {
        const auto result = model_queued_instruction_command({
            .command = vector.command,
            .selected_id = 37,
        });
        EXPECT_EQ(result.instruction, std::optional<std::int16_t>{vector.instruction});
        EXPECT_EQ(result.parameter.raw_value, std::optional<std::int16_t>{37});
        EXPECT_EQ(result.parameter.kind, vector.kind);
        EXPECT_EQ(result.parameter.status, QueuedInstructionParamStatus::Validated);
    }

    const auto guard = model_queued_instruction_command({
        .command = QueuedInstructionCommandKind::Guard,
    });
    EXPECT_EQ(guard.instruction, std::optional<std::int16_t>{4});
    EXPECT_EQ(guard.parameter.raw_value, std::optional<std::int16_t>{-1});

    const auto focus = model_queued_instruction_command({
        .command = QueuedInstructionCommandKind::Focus,
    });
    EXPECT_EQ(focus.instruction, std::optional<std::int16_t>{0});
    EXPECT_EQ(focus.parameter.raw_value, std::optional<std::int16_t>{-1});
}

TEST(SavorPredictQueuedInstructionParam, ReliablePostMacroSnapshotAlwaysWins) {
    const auto missing_macro = reconcile_queued_instruction_macro_evidence({
        .reliable_pre_macro_value = -1,
        .reliable_post_macro_value = 0,
    });
    EXPECT_EQ(missing_macro.accepted_value, std::optional<std::int16_t>{0});
    EXPECT_FALSE(missing_macro.macro_observed);
    EXPECT_FALSE(missing_macro.macro_trusted);
    EXPECT_EQ(missing_macro.status, QueuedInstructionParamStatus::Validated);

    const auto partial_macro = reconcile_queued_instruction_macro_evidence({
        .reliable_pre_macro_value = -1,
        .reliable_post_macro_value = 0,
        .macro_observed_post_write_values = {3, 0},
        .macro_capture_complete = false,
    });
    EXPECT_EQ(partial_macro.accepted_value, std::optional<std::int16_t>{0});
    EXPECT_TRUE(partial_macro.macro_agrees_with_post_snapshot);
    EXPECT_FALSE(partial_macro.macro_trusted);

    const auto contradictory_macro = reconcile_queued_instruction_macro_evidence({
        .reliable_pre_macro_value = -1,
        .reliable_post_macro_value = 0,
        .macro_observed_post_write_values = {1},
        .macro_capture_complete = true,
    });
    EXPECT_EQ(contradictory_macro.accepted_value, std::optional<std::int16_t>{0});
    EXPECT_FALSE(contradictory_macro.macro_agrees_with_post_snapshot);
    EXPECT_FALSE(contradictory_macro.macro_trusted);
    EXPECT_EQ(contradictory_macro.status, QueuedInstructionParamStatus::Provisional);
}

TEST(SavorPredictQueuedInstructionParam, MacroObservationCannotReplaceReliableSnapshots) {
    const auto no_pre = reconcile_queued_instruction_macro_evidence({
        .reliable_post_macro_value = 0,
        .macro_observed_post_write_values = {0},
        .macro_capture_complete = true,
    });
    EXPECT_EQ(no_pre.status, QueuedInstructionParamStatus::MissingInput);

    const auto no_post = reconcile_queued_instruction_macro_evidence({
        .reliable_pre_macro_value = -1,
        .macro_observed_post_write_values = {0},
        .macro_capture_complete = true,
    });
    EXPECT_EQ(no_post.status, QueuedInstructionParamStatus::MissingInput);
    EXPECT_FALSE(no_post.accepted_value.has_value());
}

TEST(SavorPredictQueuedInstructionParam, MapsFinalRouteAndAttackResultThroughQueuedState) {
    const auto miss = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::DirectMelee,
        .attack_result = 0,
    });
    EXPECT_EQ(miss.queued_state, std::optional<QueuedStdActionState>{
        QueuedStdActionState::DirectNoncritical5});
    EXPECT_EQ(miss.instruction_mode, std::optional<std::int16_t>{4});

    const auto hit = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::DirectMelee,
        .attack_result = 1,
    });
    EXPECT_EQ(hit.queued_state, std::optional<QueuedStdActionState>{
        QueuedStdActionState::DirectNoncritical5});
    EXPECT_EQ(hit.instruction_mode, std::optional<std::int16_t>{4});

    const auto critical = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::DirectMelee,
        .attack_result = 2,
    });
    EXPECT_EQ(critical.queued_state, std::optional<QueuedStdActionState>{
        QueuedStdActionState::DirectCritical6});
    EXPECT_EQ(critical.instruction_mode, std::optional<std::int16_t>{8});

    const auto ranged = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::FallbackRanged,
        .attack_result = 1,
    });
    EXPECT_EQ(ranged.queued_state, std::optional<QueuedStdActionState>{
        QueuedStdActionState::FallbackRanged7});
    EXPECT_EQ(ranged.instruction_mode, std::optional<std::int16_t>{5});
}

TEST(SavorPredictQueuedInstructionParam, RejectsImpossibleAndUnsupportedTransitions) {
    const auto fallback_critical = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::FallbackRanged,
        .attack_result = 2,
    });
    EXPECT_EQ(fallback_critical.status, QueuedInstructionParamStatus::Inconsistent);
    EXPECT_FALSE(fallback_critical.instruction_mode.has_value());

    const auto counter = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::DirectMelee,
        .attack_result = 1,
        .counter_follow_up = true,
    });
    EXPECT_EQ(counter.status, QueuedInstructionParamStatus::Unsupported);

    const auto missing_route = model_basic_attack_queued_state({
        .attack_result = 1,
    });
    EXPECT_EQ(missing_route.status, QueuedInstructionParamStatus::MissingInput);

    const auto missing_result = model_basic_attack_queued_state({
        .route = BasicAttackExecutionRoute::DirectMelee,
    });
    EXPECT_EQ(missing_result.status, QueuedInstructionParamStatus::MissingInput);
}

} // namespace
} // namespace savor::predict
