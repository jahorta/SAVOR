#include <gtest/gtest.h>

#include "../SavorCore/Phases/Programs/BattleCompletion/BattleCompletionContracts.h"
#include "../SavorCore/Phases/Programs/BattleCompletion/BattleCompletionModule.h"
#include "../SavorCore/Phases/Programs/BattleRecord/BattleRecordModule.h"
#include "../SavorCore/Phases/Programs/BattleRecord/BattleReplayModule.h"
#include "../SavorCore/Phases/Programs/TasMovieCheckpoint/TasMovieCheckpointModule.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Codec/ProgramCodecV1.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Composition/BattleCompletionComposition.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Composition/BattleResultsHandler.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Actions/CanonicalActionPayload.h"
#include "../SavorCore/Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "../SavorCore/Runner/Runtime/Services/Savestate/SavestateTypes.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace completion = savor::runtime::battlecompletion;
namespace recording = savor::runtime::battlerecord;
namespace replaying = savor::runtime::battlereplay;
namespace tasmovie = savor::runtime::tasmovie;
namespace capabilities = savor::runtime::program::capabilities;
namespace composition = savor::runtime::program::composition;
using namespace savor::runtime::program;
using savor::runtime::battlesingleturn::BattleSingleTurnOutcomeV1;
using soa::battle::actions::ActionParameters;
using soa::battle::actions::BattleAction;
using soa::battle::actions::BattleCommand;

completion::FieldTransitionContextV1 Transition(std::uint32_t pc)
{
    return {
        .area = 123,
        .raw_suffix = static_cast<std::uint8_t>('b'),
        .effective_suffix = static_cast<std::uint8_t>('b'),
        .used_area_99_suffix = false,
        .sct_filename = "me123b.sct",
        .rng_seed = 0x12345678,
        .provenance = {.pc = pc, .vi_count = 456, .workset_epoch = 7},
    };
}

completion::BattleCompletionSnapshotV1 Snapshot(
    std::uint32_t rng,
    std::uint32_t reward_phase)
{
    completion::BattleCompletionSnapshotV1 result{};
    for (std::size_t character = 0;
         character < completion::CharacterCount;
         ++character)
    {
        for (std::size_t byte = 0;
             byte < completion::CharacterRecordSize;
             ++byte)
        {
            result.character_records[character][byte] =
                static_cast<std::uint8_t>(character * 17 + byte);
        }
        // Keep the fields interpreted by BuildBattleCompletionManifestV1 in
        // their valid ranges.
        result.character_records[character][0x0b] = 10;
        for (std::size_t element = 0; element < 6; ++element)
            result.character_records[character][0x34 + element] = 2;
    }
    result.normal_experience_reward = 100;
    result.magic_experience_reward = 12;
    result.gold_reward = 300;
    result.reward_items = {{{273, 1, 9}, {-1, 0, 0}, {42, 2, 7}}};
    result.rng_seed = rng;
    result.battle_input_state = 2;
    result.reward_phase = reward_phase;
    return result;
}

completion::BattleCompletionManifestV1 Manifest(
    std::uint32_t transition_pc = 0x80101894u)
{
    auto before = Snapshot(0x11111111, 1);
    before.battle_input_state = 0;
    auto after = Snapshot(0x22222222, 6);
    after.character_records[0][0x0b] = 11;
    after.character_records[1][0x34] = 3;

    completion::BattleCompletionManifestV1 result;
    std::string diagnostic;
    EXPECT_TRUE(completion::BuildBattleCompletionManifestV1(
        {.battle_set_id = 11,
         .wave_id = 12,
         .turn_job_id = 13,
         .execution_job_id = 14},
        before,
        after,
        {.pc = 0x800706d8u, .vi_count = 100, .workset_epoch = 7},
        {.pc = 0x8006f598u, .vi_count = 110, .workset_epoch = 7},
        {.pc = 0x8006fd58u, .vi_count = 120, .workset_epoch = 7},
        Transition(transition_pc),
        result,
        &diagnostic)) << diagnostic;
    return result;
}

BattleCommand Attack(std::uint8_t actor, std::uint8_t target)
{
    return {
        .actor_slot = actor,
        .macro = BattleAction::Attack,
        .params = ActionParameters{.target_slot = target},
    };
}

recording::BattleReplayPlanV1 ReplayPlan()
{
    recording::BattleReplayPlanV1 result;
    result.selected_lineage = {
        .battle_set_id = 11,
        .wave_id = 12,
        .turn_job_id = 13,
        .execution_job_id = 14,
    };
    result.battle_completion_id = 15;
    result.confirmed_seed_frame = {
        .buttons = 0x100,
        .main_x = 127,
        .main_y = 128,
        .c_x = 126,
        .c_y = 129,
        .trig_l = 3,
        .trig_r = 4,
    };
    result.turns = {
        {.turn_index = 1,
         .plan = {.fake_attack_count = 2,
                  .commands = {Attack(0, 4), Attack(1, 4)}},
         .expected_outcome = BattleSingleTurnOutcomeV1::ReachedNextTurn,
         .expected_ending_rng = 0xaaaa0001},
        {.turn_index = 2,
         .plan = {.fake_attack_count = 3,
                  .commands = {Attack(0, 4), Attack(1, 4)}},
         .expected_outcome = BattleSingleTurnOutcomeV1::Victory,
         .expected_ending_rng = 0xaaaa0002},
    };
    result.expected_completion = Manifest();
    result.canonical_sha256 =
        recording::ComputeBattleReplayPlanHashV1(result);
    return result;
}

recording::BattleReplaySourceBindingV1 SourceBinding(bool paired)
{
    recording::BattleReplaySourceBindingV1 result;
    result.source_savestate_id = 21;
    result.source_savestate_sha256 = std::string(64, '1');
    result.source_itinerary_artifact_id = 23;
    result.source_itinerary_sha256 = std::string(64, '3');
    if (paired)
    {
        result.source_dtm_artifact_id = 22;
        result.source_dtm_sha256 = std::string(64, '2');
    }
    result.canonical_sha256 =
        recording::ComputeBattleReplaySourceBindingHashV1(result);
    return result;
}

std::vector<const Instruction*> Instructions(const ProgramModule& module)
{
    std::vector<const Instruction*> output;
    for (const auto& function : module.functions)
        for (const auto& block : function.blocks)
            for (const auto& instruction : block.instructions)
                output.push_back(&instruction);
    return output;
}

TEST(BattleCompletionContracts, SnapshotCodecRoundTripsAndRejectsDrift)
{
    const auto original = Snapshot(0xfeedbeef, 6);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(completion::EncodeBattleCompletionSnapshotV1(original, bytes));
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "BCS1");

    completion::BattleCompletionSnapshotV1 decoded;
    ASSERT_TRUE(completion::DecodeBattleCompletionSnapshotV1(bytes, decoded));
    std::vector<std::uint8_t> reencoded;
    ASSERT_TRUE(completion::EncodeBattleCompletionSnapshotV1(decoded, reencoded));
    EXPECT_EQ(reencoded, bytes);

    bytes.push_back(0);
    EXPECT_FALSE(completion::DecodeBattleCompletionSnapshotV1(bytes, decoded));
    bytes.resize(3);
    EXPECT_FALSE(completion::DecodeBattleCompletionSnapshotV1(bytes, decoded));
}

TEST(BattleCompletionContracts,
     VictoryEntryStateIsUnconstrainedButCommittedStateIsRequired)
{
    auto before = Snapshot(0x11111111, 1);
    before.battle_input_state = 0;
    auto after = Snapshot(0x22222222, 6);

    const auto build = [&](completion::BattleCompletionManifestV1& output,
                           std::string& diagnostic)
    {
        diagnostic.clear();
        return completion::BuildBattleCompletionManifestV1(
            {.battle_set_id = 11,
             .wave_id = 12,
             .turn_job_id = 13,
             .execution_job_id = 14},
            before,
            after,
            {.pc = 0x800706d8u, .vi_count = 100, .workset_epoch = 7},
            {.pc = 0x8006f598u, .vi_count = 110, .workset_epoch = 7},
            {.pc = 0x8006fd58u, .vi_count = 120, .workset_epoch = 7},
            Transition(0x80101894u),
            output,
            &diagnostic);
    };

    completion::BattleCompletionManifestV1 manifest;
    std::string diagnostic;
    EXPECT_TRUE(build(manifest, diagnostic)) << diagnostic;

    after.battle_input_state = 1;
    EXPECT_FALSE(build(manifest, diagnostic));

    after.battle_input_state = 2;
    after.reward_phase = 5;
    EXPECT_FALSE(build(manifest, diagnostic));
}

TEST(BattleCompletionContracts, ManifestCodecPreservesEvidenceAndSemanticHash)
{
    const auto original = Manifest();
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(completion::EncodeBattleCompletionManifestV1(original, bytes));
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "BCM1");

    completion::BattleCompletionManifestV1 decoded;
    ASSERT_TRUE(completion::DecodeBattleCompletionManifestV1(bytes, decoded));
    EXPECT_EQ(decoded.lineage, original.lineage);
    EXPECT_EQ(decoded.entry, original.entry);
    EXPECT_EQ(decoded.reward_entry, original.reward_entry);
    EXPECT_EQ(decoded.reward_commit, original.reward_commit);
    EXPECT_EQ(decoded.transition, original.transition);
    EXPECT_TRUE(completion::SemanticallyEqualBattleCompletionManifestV1(
        original, decoded));
    EXPECT_EQ(completion::SemanticBattleCompletionManifestHashV1(original),
              completion::SemanticBattleCompletionManifestHashV1(decoded));

    auto different_provenance = decoded;
    different_provenance.entry.vi_count += 1000;
    different_provenance.lineage.execution_job_id += 1000;
    different_provenance.transition.provenance.vi_count += 1000;
    EXPECT_TRUE(completion::SemanticallyEqualBattleCompletionManifestV1(
        original, different_provenance));

    different_provenance.gold_reward += 1;
    EXPECT_FALSE(completion::SemanticallyEqualBattleCompletionManifestV1(
        original, different_provenance));
    EXPECT_NE(completion::SemanticBattleCompletionManifestHashV1(original),
              completion::SemanticBattleCompletionManifestHashV1(
                  different_provenance));

    bytes[0] = 'X';
    EXPECT_FALSE(completion::DecodeBattleCompletionManifestV1(bytes, decoded));
}

TEST(BattleCompletionContracts, TransitionCodecAcceptsFastAndDeferredPreseed)
{
    for (const auto pc : {0x80101894u, 0x801018acu})
    {
        const auto original = Transition(pc);
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(completion::EncodeFieldTransitionContextV1(original, bytes));
        ASSERT_GE(bytes.size(), 4u);
        EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "FTC1");

        completion::FieldTransitionContextV1 decoded;
        ASSERT_TRUE(completion::DecodeFieldTransitionContextV1(bytes, decoded));
        EXPECT_EQ(decoded, original);

        const auto manifest = Manifest(pc);
        EXPECT_EQ(manifest.transition.provenance.pc, pc);
    }
}

TEST(BattleCompletionContracts, TimingAnchorCodecIsStrict)
{
    const completion::BattleTimingAdjustmentAnchorV1 original{
        .turn_index = 2,
        .actor_slot = 1,
        .command_ordinal = 2,
        .semantic_role = "final_player_command_commitment",
        .dtm_input_index = 9876,
    };
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(completion::EncodeBattleTimingAdjustmentAnchorV1(
        original, bytes));
    ASSERT_GE(bytes.size(), 4u);
    EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "BTA1");

    completion::BattleTimingAdjustmentAnchorV1 decoded;
    ASSERT_TRUE(completion::DecodeBattleTimingAdjustmentAnchorV1(
        bytes, decoded));
    EXPECT_EQ(decoded, original);

    auto invalid = original;
    invalid.dtm_input_index = 0;
    EXPECT_FALSE(completion::EncodeBattleTimingAdjustmentAnchorV1(
        invalid, bytes));
    invalid = original;
    invalid.actor_slot = 4;
    EXPECT_FALSE(completion::EncodeBattleTimingAdjustmentAnchorV1(
        invalid, bytes));

    ASSERT_TRUE(completion::EncodeBattleTimingAdjustmentAnchorV1(
        original, bytes));
    bytes.push_back(0);
    EXPECT_FALSE(completion::DecodeBattleTimingAdjustmentAnchorV1(
        bytes, decoded));
}

TEST(BattleCompletionContracts, ReconstructsAndClassifiesCanonicalSctNames)
{
    std::uint8_t suffix = 0;
    std::string filename;
    std::string diagnostic;
    ASSERT_TRUE(completion::ReconstructFieldSctFilenameV1(
        99, static_cast<std::uint8_t>('z'), 2, suffix, filename, &diagnostic))
        << diagnostic;
    EXPECT_EQ(suffix, static_cast<std::uint8_t>('c'));
    EXPECT_EQ(filename, "me099c.sct");
    EXPECT_EQ(tasmovie::ClassifyTasMovieNextPhaseV1(
                  tasmovie::FieldFastPreseedPc, filename),
              tasmovie::TasMovieNextPhaseV1::OverworldNavigation);

    struct Case
    {
        std::uint32_t area;
        tasmovie::TasMovieNextPhaseV1 kind;
    };
    constexpr std::array cases{
        Case{0, tasmovie::TasMovieNextPhaseV1::FieldNavigation},
        Case{199, tasmovie::TasMovieNextPhaseV1::FieldNavigation},
        Case{200, tasmovie::TasMovieNextPhaseV1::Cutscene},
        Case{499, tasmovie::TasMovieNextPhaseV1::Cutscene},
        Case{500, tasmovie::TasMovieNextPhaseV1::ShipBattle},
        Case{999, tasmovie::TasMovieNextPhaseV1::ShipBattle},
    };
    for (const auto& test : cases)
    {
        ASSERT_TRUE(completion::ReconstructFieldSctFilenameV1(
            test.area,
            static_cast<std::uint8_t>('a'),
            std::nullopt,
            suffix,
            filename,
            &diagnostic)) << diagnostic;
        EXPECT_EQ(tasmovie::ClassifyTasMovieNextPhaseV1(
                      tasmovie::FieldFastPreseedPc, filename), test.kind);
    }

    EXPECT_FALSE(completion::ReconstructFieldSctFilenameV1(
        99, static_cast<std::uint8_t>('a'), std::nullopt,
        suffix, filename, &diagnostic));
    EXPECT_FALSE(completion::ReconstructFieldSctFilenameV1(
        99, static_cast<std::uint8_t>('a'), 26,
        suffix, filename, &diagnostic));
    EXPECT_FALSE(completion::ReconstructFieldSctFilenameV1(
        1000, static_cast<std::uint8_t>('a'), std::nullopt,
        suffix, filename, &diagnostic));
    EXPECT_EQ(tasmovie::ClassifyTasMovieNextPhaseV1(
                  tasmovie::FieldFastPreseedPc, "battle"),
              tasmovie::TasMovieNextPhaseV1::Unknown);
}

TEST(BattleReplayPlan, CodecRoundTripsAndHashIsDeterministic)
{
    const auto original = ReplayPlan();
    std::string diagnostic;
    ASSERT_TRUE(recording::ValidateBattleReplayPlanV1(original, &diagnostic))
        << diagnostic;
    EXPECT_EQ(recording::ComputeBattleReplayPlanHashV1(original),
              recording::ComputeBattleReplayPlanHashV1(original));
    auto other_completion = original;
    ++other_completion.battle_completion_id;
    other_completion.canonical_sha256.clear();
    EXPECT_NE(recording::ComputeBattleReplayPlanHashV1(original),
              recording::ComputeBattleReplayPlanHashV1(other_completion));

    const auto bytes = recording::EncodeBattleReplayPlanV1(
        original, &diagnostic);
    ASSERT_FALSE(bytes.empty()) << diagnostic;
    recording::BattleReplayPlanV1 decoded;
    ASSERT_TRUE(recording::DecodeBattleReplayPlanV1(
        bytes, decoded, &diagnostic)) << diagnostic;
    EXPECT_EQ(decoded.canonical_sha256, original.canonical_sha256);
    EXPECT_EQ(decoded.selected_lineage, original.selected_lineage);
    EXPECT_EQ(decoded.battle_completion_id, original.battle_completion_id);
    EXPECT_EQ(decoded.confirmed_seed_frame.buttons,
              original.confirmed_seed_frame.buttons);
    ASSERT_EQ(decoded.turns.size(), original.turns.size());
    for (std::size_t index = 0; index < decoded.turns.size(); ++index)
    {
        EXPECT_EQ(decoded.turns[index].turn_index,
                  original.turns[index].turn_index);
        EXPECT_EQ(decoded.turns[index].plan.fake_attack_count,
                  original.turns[index].plan.fake_attack_count);
        EXPECT_EQ(decoded.turns[index].plan.commands.size(),
                  original.turns[index].plan.commands.size());
        EXPECT_EQ(decoded.turns[index].expected_outcome,
                  original.turns[index].expected_outcome);
        EXPECT_EQ(decoded.turns[index].expected_ending_rng,
                  original.turns[index].expected_ending_rng);
    }
    EXPECT_TRUE(completion::SemanticallyEqualBattleCompletionManifestV1(
        decoded.expected_completion, original.expected_completion));

    auto corrupt = bytes;
    corrupt.back() ^= 1;
    EXPECT_FALSE(recording::DecodeBattleReplayPlanV1(
        corrupt, decoded, &diagnostic));
}

TEST(BattleReplaySourceBinding, PairedAndInactiveRoundTripIndependently)
{
    for (const bool paired : {false, true})
    {
        const auto original = SourceBinding(paired);
        std::string diagnostic;
        ASSERT_TRUE(recording::ValidateBattleReplaySourceBindingV1(
            original, &diagnostic)) << diagnostic;
        const auto bytes = recording::EncodeBattleReplaySourceBindingV1(
            original, &diagnostic);
        ASSERT_FALSE(bytes.empty()) << diagnostic;
        recording::BattleReplaySourceBindingV1 decoded;
        ASSERT_TRUE(recording::DecodeBattleReplaySourceBindingV1(
            bytes, decoded, &diagnostic)) << diagnostic;
        EXPECT_EQ(decoded.source_savestate_id,
                  original.source_savestate_id);
        EXPECT_EQ(decoded.source_dtm_artifact_id,
                  original.source_dtm_artifact_id);
        EXPECT_EQ(decoded.source_itinerary_artifact_id,
                  original.source_itinerary_artifact_id);
        EXPECT_EQ(decoded.canonical_sha256, original.canonical_sha256);
    }
}

TEST(BattleReplayPlan, RejectsGapsTerminalMismatchLineageMismatchAndHashDrift)
{
    std::string diagnostic;

    auto plan = ReplayPlan();
    plan.turns[1].turn_index = 3;
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));
    EXPECT_NE(diagnostic.find("contiguous"), std::string::npos);

    plan = ReplayPlan();
    plan.turns[0].expected_outcome = BattleSingleTurnOutcomeV1::Victory;
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));

    plan = ReplayPlan();
    plan.turns.back().expected_outcome =
        BattleSingleTurnOutcomeV1::ReachedNextTurn;
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));

    plan = ReplayPlan();
    plan.expected_completion.lineage.execution_job_id += 1;
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));
    EXPECT_NE(diagnostic.find("lineage"), std::string::npos);

    plan = ReplayPlan();
    plan.battle_completion_id = 0;
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));

    plan = ReplayPlan();
    plan.canonical_sha256 = std::string(64, 'f');
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));
    EXPECT_NE(diagnostic.find("hash"), std::string::npos);

    plan = ReplayPlan();
    plan.turns.clear();
    plan.canonical_sha256.clear();
    EXPECT_FALSE(recording::ValidateBattleReplayPlanV1(plan, &diagnostic));
}

TEST(BattleCompletionModules, ShareOneTypedCompletionSequence)
{
    const auto completion_definition =
        completion::BattleCompletionFullPhaseDefinitionV1();
    ASSERT_NE(completion_definition, nullptr);

    std::string diagnostic;
    const auto record_definition = recording::PrepareBattleRecordFullPhaseV1(
        ReplayPlan(), &diagnostic);
    ASSERT_NE(record_definition, nullptr) << diagnostic;
    const auto replay_definition = replaying::PrepareBattleReplayFullPhaseV1(
        ReplayPlan(), &diagnostic);
    ASSERT_NE(replay_definition, nullptr) << diagnostic;

    const auto verify_module = [](
        const savor::runtime::fullphase::IFullPhaseProgramDefinition& definition)
    {
        const auto decoded = DecodeProgramModuleV1(
            definition.module_envelope().payload);
        ASSERT_TRUE(decoded) << decoded.status.message;
        const auto& module = *decoded.value;

        const auto sequence = std::ranges::find(
            module.functions,
            std::string_view("soa.battle.completion.sequence.v1"),
            [](const ProgramFunction& function)
            {
                return std::string_view(function.name);
            });
        ASSERT_NE(sequence, module.functions.end());
        EXPECT_EQ(std::ranges::count(
            module.functions,
            std::string_view("soa.battle.completion.sequence.v1"),
            [](const ProgramFunction& function)
            {
                return std::string_view(function.name);
            }), 1);
        ASSERT_EQ(sequence->arguments.size(), 8u);
        EXPECT_EQ(sequence->output_type,
            TypeRef::Named(
                composition::BattleCompletionSequenceReceiptSchemaIdentity()));

        std::size_t calls = 0;
        for (const auto& function : module.functions)
        {
            for (const auto& block : function.blocks)
            {
                for (const auto& instruction : block.instructions)
                {
                    if (instruction.opcode == InstructionOpcode::CallLocal &&
                        instruction.target.local_function == sequence->id)
                    {
                        ++calls;
                    }
                }
            }
        }
        EXPECT_EQ(calls, 1u);
    };

    verify_module(*completion_definition);
    verify_module(*record_definition);
    verify_module(*replay_definition);
}

TEST(BattleReplayModule,
     ObservesRestoredStateAndSupportsInactiveOrAdoptedPlayback)
{
    std::string diagnostic;
    const auto plan = ReplayPlan();
    const auto record = recording::PrepareBattleRecordFullPhaseV1(
        plan, &diagnostic);
    const auto replay = replaying::PrepareBattleReplayFullPhaseV1(
        plan, &diagnostic);
    ASSERT_NE(record, nullptr) << diagnostic;
    ASSERT_NE(replay, nullptr) << diagnostic;
    EXPECT_EQ(recording::EncodeBattleReplayPlanV1(record->replay_plan()),
              recording::EncodeBattleReplayPlanV1(replay->replay_plan()));
    EXPECT_TRUE(replay->runtime_contract().execution.allow_movie_playback);
    EXPECT_FALSE(replay->runtime_contract().execution.allow_movie_recording);

    const auto decoded = DecodeProgramModuleV1(
        replay->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    const auto& module = *decoded.value;
    for (const CanonicalAction required : {
             CanonicalAction::MovieObserveState,
             CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
             CanonicalAction::ExecutionRequirePausedPc,
             CanonicalAction::MovieStopPlayback})
        EXPECT_NE(std::ranges::find(module.action_imports,
            CanonicalActionIdentity(required)), module.action_imports.end());
    for (const CanonicalAction forbidden : {
             CanonicalAction::MovieStartRecording,
             CanonicalAction::MovieStopRecording,
             CanonicalAction::SavestateSaveImmutableArtifact})
        EXPECT_EQ(std::ranges::find(module.action_imports,
            CanonicalActionIdentity(forbidden)), module.action_imports.end());
    ASSERT_EQ(module.entrypoints.size(), 1u);
    EXPECT_TRUE(module.entrypoints.front().artifact_schemas.empty());
    EXPECT_EQ(module.budgets.maximum_artifacts, 1u);

    const auto function = std::ranges::find(
        module.functions, replaying::Entrypoint, &ProgramFunction::name);
    ASSERT_NE(function, module.functions.end());
    const auto entry = std::ranges::find(
        function->blocks, function->entry_block, &BasicBlock::id);
    ASSERT_NE(entry, function->blocks.end());
    const auto find = [&](std::string_view selector) {
        for (const auto& block : function->blocks)
        {
            const auto instruction = std::ranges::find(
                block.instructions, selector, &Instruction::selector);
            if (instruction != block.instructions.end())
                return std::pair{&block, &*instruction};
        }
        return std::pair<const BasicBlock*, const Instruction*>{nullptr, nullptr};
    };
    const auto observe = find("movie/observe-restored-state");
    const auto adopt = find("movie/adopt-restored-playback");
    const auto paired_qualify = find("entry/paired/require");
    const auto inactive_qualify = find("entry/inactive/require");
    const auto stop = find("movie/stop-playback");
    const auto verify = find("movie/verify-inactive");
    const auto advance = find("seed/stop");
    ASSERT_NE(observe.second, nullptr);
    ASSERT_NE(adopt.second, nullptr);
    ASSERT_NE(paired_qualify.second, nullptr);
    ASSERT_NE(inactive_qualify.second, nullptr);
    ASSERT_NE(stop.second, nullptr);
    ASSERT_NE(verify.second, nullptr);
    ASSERT_NE(advance.second, nullptr);
    EXPECT_EQ(entry->terminator.kind, TerminatorKind::EnumSwitch);
    EXPECT_EQ(entry->terminator.enum_cases.size(), 2u);
    EXPECT_EQ(adopt.first, paired_qualify.first);
    EXPECT_EQ(adopt.first, stop.first);
    EXPECT_EQ(stop.first, verify.first);
    EXPECT_NE(inactive_qualify.first, adopt.first);
    EXPECT_NE(advance.first, adopt.first);

    replaying::BattleReplayRequestV1 request;
    auto input = replaying::EncodeBattleReplayExecutionInputV1(request);
    replaying::BattleReplayRequestV1 decoded_request;
    EXPECT_TRUE(replaying::DecodeBattleReplayExecutionInputV1(
        input, decoded_request, &diagnostic)) << diagnostic;
    input.push_back(0);
    EXPECT_FALSE(replaying::DecodeBattleReplayExecutionInputV1(
        input, decoded_request, &diagnostic));
}

TEST(BattleRecordModule,
     AdoptsRestoredPlaybackBeforeRecordingAndGuestAdvancement)
{
    std::string diagnostic;
    const auto definition = recording::PrepareBattleRecordFullPhaseV1(
        ReplayPlan(), &diagnostic);
    ASSERT_NE(definition, nullptr) << diagnostic;
    EXPECT_TRUE(
        definition->runtime_contract().execution.allow_movie_playback);
    EXPECT_TRUE(
        definition->runtime_contract().execution.allow_movie_recording);
    const auto decoded = DecodeProgramModuleV1(
        definition->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    const auto& module = *decoded.value;

    for (const CanonicalAction required : {
             CanonicalAction::MovieAdoptRestoredReadOnlyPlayback,
             CanonicalAction::ExecutionRequirePausedPc,
             CanonicalAction::MovieStartRecording,
             CanonicalAction::ExecutionContinueUntilInputObserved,
         })
    {
        EXPECT_NE(
            std::ranges::find(
                module.action_imports,
                CanonicalActionIdentity(required)),
            module.action_imports.end());
    }
    for (const CanonicalAction forbidden : {
             CanonicalAction::MoviePrepareReadOnlyPlayback,
             CanonicalAction::MovieStartPlayback,
             CanonicalAction::MovieObserveState,
         })
    {
        EXPECT_EQ(
            std::ranges::find(
                module.action_imports,
                CanonicalActionIdentity(forbidden)),
            module.action_imports.end());
    }

    const auto function = std::ranges::find(
        module.functions,
        recording::Entrypoint,
        &ProgramFunction::name);
    ASSERT_NE(function, module.functions.end());
    const auto entry = std::ranges::find(
        function->blocks,
        function->entry_block,
        &BasicBlock::id);
    ASSERT_NE(entry, function->blocks.end());
    const auto find_instruction = [&](std::string_view selector) {
        return std::ranges::find(
            entry->instructions,
            selector,
            &Instruction::selector);
    };
    const auto adopt = find_instruction("movie/adopt-restored-playback");
    const auto require_paused = find_instruction("entry/require");
    const auto start_recording = find_instruction("movie/branch-to-recording");
    const auto first_guest_advance = find_instruction("seed/stop");
    ASSERT_NE(adopt, entry->instructions.end());
    ASSERT_NE(require_paused, entry->instructions.end());
    ASSERT_NE(start_recording, entry->instructions.end());
    ASSERT_NE(first_guest_advance, entry->instructions.end());
    EXPECT_EQ(adopt->opcode, InstructionOpcode::AwaitAction);
    EXPECT_EQ(require_paused->opcode, InstructionOpcode::AwaitAction);
    EXPECT_EQ(start_recording->opcode, InstructionOpcode::AwaitAction);
    EXPECT_EQ(first_guest_advance->opcode, InstructionOpcode::AwaitAction);
    ASSERT_TRUE(adopt->target.dependency.has_value());
    ASSERT_TRUE(require_paused->target.dependency.has_value());
    ASSERT_TRUE(start_recording->target.dependency.has_value());
    ASSERT_TRUE(first_guest_advance->target.dependency.has_value());
    EXPECT_EQ(
        *adopt->target.dependency,
        CanonicalActionIdentity(
            CanonicalAction::MovieAdoptRestoredReadOnlyPlayback));
    EXPECT_EQ(
        *require_paused->target.dependency,
        CanonicalActionIdentity(CanonicalAction::ExecutionRequirePausedPc));
    EXPECT_EQ(
        *start_recording->target.dependency,
        CanonicalActionIdentity(CanonicalAction::MovieStartRecording));
    EXPECT_EQ(
        *first_guest_advance->target.dependency,
        CanonicalActionIdentity(CanonicalAction::ExecutionContinueUntil));
    EXPECT_LT(adopt, require_paused);
    EXPECT_LT(require_paused, start_recording);
    EXPECT_LT(start_recording, first_guest_advance);
}

TEST(BattleRecordModule,
     CapturesCheckpointThenPublishesNeutralTailAndExtendedDtm)
{
    std::string diagnostic;
    const auto definition = recording::PrepareBattleRecordFullPhaseV1(
        ReplayPlan(), &diagnostic);
    ASSERT_NE(definition, nullptr) << diagnostic;
    const auto decoded = DecodeProgramModuleV1(
        definition->module_envelope().payload);
    ASSERT_TRUE(decoded) << decoded.status.message;
    const auto& module = *decoded.value;
    const auto function = std::ranges::find(
        module.functions, recording::Entrypoint, &ProgramFunction::name);
    ASSERT_NE(function, module.functions.end());

    const auto find = [&](std::string_view selector) {
        for (const auto& block : function->blocks)
        {
            const auto instruction = std::ranges::find(
                block.instructions, selector, &Instruction::selector);
            if (instruction != block.instructions.end())
                return std::pair{&block, &*instruction};
        }
        return std::pair<const BasicBlock*, const Instruction*>{nullptr, nullptr};
    };
    const auto save = find("record/capture-checkpoint-sav");
    const auto begin = find("record/neutral-tail/binding");
    const auto observe = find("record/neutral-tail/observe-poll");
    const auto complete = find("record/neutral-tail/complete");
    const auto finalize = find("record/finalize-extended-dtm");
    const auto publish = find("record/publish-extended-dtm");
    for (const auto* instruction : {
             save.second, begin.second, observe.second,
             complete.second, finalize.second, publish.second})
        ASSERT_NE(instruction, nullptr);
    EXPECT_EQ(save.first, begin.first);
    EXPECT_EQ(save.first, observe.first);
    EXPECT_EQ(save.first, complete.first);
    EXPECT_EQ(save.first, finalize.first);
    EXPECT_EQ(save.first, publish.first);
    EXPECT_LT(save.second, begin.second);
    EXPECT_LT(begin.second, observe.second);
    EXPECT_LT(observe.second, complete.second);
    EXPECT_LT(complete.second, finalize.second);
    EXPECT_LT(finalize.second, publish.second);
    ASSERT_TRUE(observe.second->target.dependency.has_value());
    EXPECT_EQ(*observe.second->target.dependency,
        CanonicalActionIdentity(
            CanonicalAction::ExecutionContinueUntilInputObserved));
    EXPECT_EQ(publish.second->opcode, InstructionOpcode::PublishArtifact);
    ASSERT_EQ(publish.second->operands.size(), 1u);
    ASSERT_TRUE(finalize.second->result.has_value());
    EXPECT_EQ(publish.second->operands.front(), finalize.second->result->id);
    EXPECT_EQ(std::ranges::count_if(
        Instructions(module),
        [](const Instruction* instruction) {
            return instruction->opcode == InstructionOpcode::PublishArtifact;
        }), 1u);

    const recording::BattleRecordRequestV1 request{
        .output_dtm_path = "extended.dtm",
        .output_preseed_savestate_path = "checkpoint.sav",
    };
    const auto invocation = definition->BuildResolvedExecution(
        recording::EncodeBattleRecordExecutionInputV1(request),
        savor::runtime::InvocationId(1), savor::runtime::AttemptId(1),
        &diagnostic);
    ASSERT_TRUE(invocation.has_value()) << diagnostic;
    const auto root = std::ranges::find(
        invocation->input.values,
        invocation->input.root,
        &ProgramValue::id);
    ASSERT_NE(root, invocation->input.values.end());
    const auto* record = std::get_if<RecordValue>(&root->payload);
    ASSERT_NE(record, nullptr);
    ASSERT_GE(record->fields.size(), 2u);
    const auto save_value = std::ranges::find(
        invocation->input.values,
        record->fields[1],
        &ProgramValue::id);
    ASSERT_NE(save_value, invocation->input.values.end());
    ProgramValueGraph save_graph{
        save_value->id,
        std::vector<ProgramValue>{*save_value}};
    CanonicalActionPayload save_payload;
    ASSERT_TRUE(DecodeCanonicalActionPayload(
        save_graph,
        *CanonicalActionInputType(
            CanonicalAction::SavestateSaveImmutableArtifact).named,
        save_payload,
        &diagnostic)) << diagnostic;
    EXPECT_EQ(save_payload.Unsigned(
        CanonicalActionPayloadField::MovieArtifactMode),
        static_cast<std::uint64_t>(
            savor::runtime::SavestateMovieArtifactMode::
                DeferredFinalRecordingPair));
}

TEST(BattleResultsHandler, AdvancesFinalConfirmationWithoutManifest)
{
    composition::BattleResultsHandlerV1 handler;
    std::string diagnostic;
    const auto stop = [](std::uint32_t pc, std::uint64_t vi) {
        return composition::BattleResultsStopProvenanceV1{
            .pc=pc, .vi_count=vi, .workset_epoch=7};
    };
    ASSERT_TRUE(handler.Begin(
        stop(composition::kBattleResultsDescriptorReadyPc, 100),
        0x12345678u, &diagnostic)) << diagnostic;
    EXPECT_EQ(handler.NextStep().action,
        composition::BattleResultsHandlerAction::ContinueNeutral);
    ASSERT_TRUE(handler.Observe(
        stop(composition::kBattleResultsConfirmReadyPc, 101), &diagnostic));
    EXPECT_EQ(handler.NextStep().action,
        composition::BattleResultsHandlerAction::PressA);
    ASSERT_TRUE(handler.Observe(
        stop(composition::kBattleResultsConfirmAcceptedPc, 102),
        &diagnostic));
    EXPECT_EQ(handler.NextStep().action,
        composition::BattleResultsHandlerAction::ReleaseA);
    ASSERT_TRUE(handler.Observe(
        stop(composition::kBattleResultsGuestPadReadReturnedPc, 103),
        &diagnostic));
    ASSERT_TRUE(handler.Observe(
        stop(composition::kBattleResultsLifecycleExitPc, 104), &diagnostic));
    ASSERT_TRUE(handler.Observe(
        stop(composition::kBattleResultsCleanupCompletePc, 105), &diagnostic));
    ASSERT_TRUE(handler.complete());

    composition::BattleResultsHandlerReceiptV1 receipt;
    ASSERT_TRUE(handler.Finalize(0x12345678u, 1u, 0u, 6u,
        receipt, &diagnostic)) << diagnostic;
    EXPECT_EQ(receipt.terminal,
        stop(composition::kBattleResultsCleanupCompletePc, 105));
    EXPECT_EQ(receipt.entry_rng, receipt.exit_rng);
}

TEST(BattleResultsHandler, RejectsUnexpectedStopsAndTerminalInvariantDrift)
{
    composition::BattleResultsHandlerV1 handler;
    std::string diagnostic;
    ASSERT_TRUE(handler.Begin({
        .pc=composition::kBattleResultsDescriptorReadyPc,
        .vi_count=1,
        .workset_epoch=7}, 55, &diagnostic));
    EXPECT_FALSE(handler.Observe({
        .pc=0xdeadbeefu,
        .vi_count=2,
        .workset_epoch=7}, &diagnostic));
    EXPECT_FALSE(diagnostic.empty());

    composition::BattleResultsHandlerV1 terminal_handler;
    ASSERT_TRUE(terminal_handler.Begin({
        .pc=composition::kBattleResultsDescriptorReadyPc,
        .vi_count=10,
        .workset_epoch=9}, 55, &diagnostic));
    ASSERT_TRUE(terminal_handler.Observe({
        .pc=composition::kBattleResultsConfirmReadyPc,
        .vi_count=11,
        .workset_epoch=9}, &diagnostic));
    ASSERT_TRUE(terminal_handler.Observe({
        .pc=composition::kBattleResultsConfirmAcceptedPc,
        .vi_count=12,
        .workset_epoch=9}, &diagnostic));
    ASSERT_TRUE(terminal_handler.Observe({
        .pc=composition::kBattleResultsGuestPadReadReturnedPc,
        .vi_count=13,
        .workset_epoch=9}, &diagnostic));
    ASSERT_TRUE(terminal_handler.Observe({
        .pc=composition::kBattleResultsLifecycleExitPc,
        .vi_count=14,
        .workset_epoch=9}, &diagnostic));
    ASSERT_TRUE(terminal_handler.Observe({
        .pc=composition::kBattleResultsCleanupCompletePc,
        .vi_count=15,
        .workset_epoch=9}, &diagnostic));
    composition::BattleResultsHandlerReceiptV1 receipt;
    EXPECT_FALSE(terminal_handler.Finalize(
        56, 1u, 0u, 6u, receipt, &diagnostic));
    EXPECT_FALSE(diagnostic.empty());

    const auto catalog = capabilities::BuildSourceCapabilityPackCatalog();
    EXPECT_EQ(std::ranges::count_if(catalog.schemas,
        [](const TypeSchemaDefinition& schema) {
            return schema.identity.canonical_id.starts_with(
                "soa.battle.results.");
        }), 0);
    const auto pack = std::ranges::find(
        catalog.manifests, capabilities::BattleResultsPackIdentity(),
        &CapabilityPackManifest::identity);
    ASSERT_NE(pack, catalog.manifests.end());
    EXPECT_TRUE(pack->schemas.empty());
    EXPECT_TRUE(pack->reducers.empty());
    EXPECT_NE(std::ranges::find(
        pack->semantic_points,
        composition::kBattleResultsDescriptorReadyPc,
        &SemanticPointDescriptor::pc),
        pack->semantic_points.end());
}

} // namespace
