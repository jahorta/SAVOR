#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_set>

#include <gtest/gtest.h>

#include "Runner/Script/PhaseScriptOpcodes.h"
#include "Runner/Script/PhaseScriptProgram.h"

TEST(PhaseScriptOpcodes, CatalogueEntriesAreSelfConsistentAndDiscoverable)
{
    const auto catalogue = savor::get_psop_catalogue();
    ASSERT_FALSE(catalogue.empty());

    std::unordered_set<std::uint8_t> codes;
    std::unordered_set<std::string_view> identifiers;

    for (const auto& metadata : catalogue) {
        const auto ordinal = static_cast<std::uint8_t>(metadata.code);
        EXPECT_EQ(metadata.ordinal, ordinal);
        EXPECT_TRUE(codes.insert(ordinal).second)
            << "duplicate opcode ordinal " << static_cast<unsigned>(ordinal);
        EXPECT_FALSE(metadata.identifier.empty());
        EXPECT_TRUE(identifiers.insert(metadata.identifier).second)
            << "duplicate opcode identifier " << metadata.identifier;
        EXPECT_FALSE(metadata.display_name.empty());
        EXPECT_EQ(savor::get_psop_metadata(metadata.code), &metadata);
        EXPECT_EQ(savor::get_psop_identifier(metadata.code), metadata.identifier);
        EXPECT_EQ(savor::get_psop_name(metadata.code), metadata.display_name);
    }
}

TEST(PhaseScriptOpcodes, CatalogueCoversEveryArgumentFormat)
{
    std::array<bool, static_cast<size_t>(savor::PSOpArgFormat::Count)> seen{};
    for (const auto& metadata : savor::get_psop_catalogue()) {
        const auto index = static_cast<size_t>(metadata.arg_format);
        ASSERT_LT(index, seen.size());
        seen[index] = true;
    }
    for (size_t index = 0; index < seen.size(); ++index) {
        EXPECT_TRUE(seen[index]) << "missing PSOpArgFormat ordinal " << index;
    }
}

TEST(PhaseScriptOpcodes, UnsupportedAndInvalidValuesFailClosed)
{
    const auto* gc_slot = savor::get_psop_metadata(savor::PSOpCode::GC_SLOT_A_SET_FROM);
    ASSERT_NE(gc_slot, nullptr);
    EXPECT_EQ(gc_slot->ordinal, 21);
    EXPECT_EQ(gc_slot->support, savor::PSOpSupport::Unsupported);

    for (const auto invalid : {
             savor::PSOpCode::Count,
             static_cast<savor::PSOpCode>(255),
         }) {
        EXPECT_EQ(savor::get_psop_metadata(invalid), nullptr);
        EXPECT_TRUE(savor::get_psop_identifier(invalid).empty());
        EXPECT_EQ(savor::get_psop_name(invalid), "Unknown Code");
        savor::PSOp op{};
        op.code = invalid;
        EXPECT_EQ(savor::get_psop_desc(op), "Unknown Code: []");
    }
}

TEST(PhaseScriptOpcodes, NavigationContextOpcodesAreSupportedAndStable)
{
    EXPECT_EQ(static_cast<std::uint8_t>(
        savor::PSOpCode::GET_NAVIGATION_CONTEXT), 50u);

    const auto* metadata = savor::get_psop_metadata(
        savor::PSOpCode::GET_NAVIGATION_CONTEXT);
    ASSERT_NE(metadata, nullptr);
    EXPECT_EQ(metadata->code, savor::PSOpCode::GET_NAVIGATION_CONTEXT);
    EXPECT_EQ(metadata->ordinal, 50u);
    EXPECT_EQ(metadata->identifier, "GET_NAVIGATION_CONTEXT");
    EXPECT_EQ(metadata->display_name, "Get Navigation Context");
    EXPECT_EQ(metadata->arg_format, savor::PSOpArgFormat::None);
    EXPECT_EQ(metadata->support, savor::PSOpSupport::Supported);

    const auto op = savor::OpGetNavigationContext();
    EXPECT_EQ(op.code, savor::PSOpCode::GET_NAVIGATION_CONTEXT);
    EXPECT_EQ(savor::get_psop_identifier(op.code), "GET_NAVIGATION_CONTEXT");
    EXPECT_EQ(savor::get_psop_name(op.code), "Get Navigation Context");
    EXPECT_EQ(savor::get_psop_desc(op), "Get Navigation Context: []");

    EXPECT_EQ(static_cast<std::uint8_t>(
        savor::PSOpCode::RECORD_CURRENT_PC_TO), 51u);
    const auto* record_pc = savor::get_psop_metadata(
        savor::PSOpCode::RECORD_CURRENT_PC_TO);
    ASSERT_NE(record_pc, nullptr);
    EXPECT_EQ(record_pc->ordinal, 51u);
    EXPECT_EQ(record_pc->identifier, "RECORD_CURRENT_PC_TO");
    EXPECT_EQ(record_pc->display_name, "Record Current PC to Context");
    EXPECT_EQ(record_pc->arg_format, savor::PSOpArgFormat::Key);
    EXPECT_EQ(record_pc->support, savor::PSOpSupport::Supported);

    constexpr auto entry_pc =
        savor::context::key::navigation::ENTRY_PC;
    const auto record_op = savor::OpRecordCurrentPcTo(entry_pc);
    EXPECT_EQ(record_op.code, savor::PSOpCode::RECORD_CURRENT_PC_TO);
    EXPECT_EQ(record_op.key.id, entry_pc);
    EXPECT_EQ(
        savor::get_psop_desc(record_op),
        "Record Current PC to Context: [key=navigation.entry_pc]");
}

TEST(PhaseScriptOpcodes, BuildersUseCorrectedAndDecoupledContracts)
{
    EXPECT_EQ(static_cast<uint8_t>(savor::PSOpCode::START_DETERMINISTIC_RUN), 11);
    EXPECT_EQ(savor::OpStartDeterministicRun().code, savor::PSOpCode::START_DETERMINISTIC_RUN);
    EXPECT_EQ(static_cast<uint8_t>(savor::PSOpCode::MATERIALIZE_BATTLE_END_RESULTS_MACRO_STEPS), 48);
    EXPECT_EQ(static_cast<uint8_t>(savor::PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS), 48);
    EXPECT_EQ(static_cast<uint8_t>(savor::PSOpCode::MATERIALIZE_BATTLE_COMPLETION_MACRO_STEPS), 49);
    EXPECT_EQ(savor::OpMaterializeBattleEndResultsMacroSteps().code,
        savor::PSOpCode::MATERIALIZE_BATTLE_END_RESULTS_MACRO_STEPS);
    EXPECT_EQ(savor::OpMaterializeBattleResultsScreenMacroSteps().code,
        savor::PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS);
    EXPECT_EQ(savor::OpMaterializeBattleCompletionMacroSteps().code,
        savor::PSOpCode::MATERIALIZE_BATTLE_COMPLETION_MACRO_STEPS);
    EXPECT_EQ(savor::OpExecuteInputMacroStep().code, savor::PSOpCode::EXECUTE_BATTLE_MACRO_STEP);

    EXPECT_EQ(static_cast<uint32_t>(savor::PSMemoryWatchpointAccess::Read), 1u);
    EXPECT_EQ(static_cast<uint32_t>(savor::PSMemoryWatchpointAccess::Write), 2u);
    EXPECT_EQ(static_cast<uint32_t>(savor::PSMemoryWatchpointAccess::Access), 3u);

    const auto watch = savor::OpArmMemoryWatchpointFromKey(
        7,
        static_cast<savor::context::key::KeyId>(0xfffe),
        4,
        savor::PSMemoryWatchpointAccess::Write);
    EXPECT_EQ(watch.code, savor::PSOpCode::ARM_MEMORY_WATCHPOINT);
    EXPECT_EQ(watch.memwatch.id, 7u);
    EXPECT_EQ(watch.memwatch.address, 0u);
    EXPECT_EQ(watch.memwatch.address_key, 0xfffeu);
    EXPECT_EQ(watch.memwatch.use_address_key, 1u);
    EXPECT_EQ(watch.memwatch.size, 4u);
    EXPECT_EQ(watch.memwatch.access, savor::PSMemoryWatchpointAccess::Write);
}

TEST(PhaseScriptOpcodes, DescriptionsFormatEveryArgumentCategory)
{
    constexpr auto key_a = static_cast<savor::context::key::KeyId>(0xfffe);
    constexpr auto key_b = static_cast<savor::context::key::KeyId>(0xfffd);

    EXPECT_EQ(savor::get_psop_desc(savor::OpMovieStop()), "Stop TAS Movie: []");
    EXPECT_EQ(savor::get_psop_desc(savor::OpApplyInputFrom(key_a)), "Apply Input: [key=65534]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpStepFrames(3, true)), "Step Frames: [n=3, disable_breakpoints=1]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpRunUntilBpKey(204)), "Run Until BP Key: [bp_key=204]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpReadU32(4096, key_a)), "Read u32: [addr=4096, dst=65534]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpWriteU32(8192, key_a)), "Write u32: [addr=8192, value_key=65534]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpLabel("loop")), "Set Label: [name=loop]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpGoto("loop")), "Goto Label: [name=loop]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpGotoIf(key_a, savor::PSCmp::GE, 9, "done")),
        "Constant Goto Label If: [key=65534, cmp=GE, imm=9, name=done]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpGotoIfKeys(key_a, savor::PSCmp::NE, key_b, "retry")),
        "Context Goto Label If: [left=65534, cmp=NE, right=65533, name=retry]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpReturnResult(key_a, 17)),
        "Return Result: [key=65534, imm=17]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpStepOpcode(true)),
        "Step Opcode: [disable_breakpoints=1]");
    EXPECT_EQ(savor::get_psop_desc(savor::OpArmMemoryWatchpointFromKey(
        7, key_a, 4, savor::PSMemoryWatchpointAccess::Write)),
        "Arm Memory Watchpoint: [id=7, addr=0, addr_key=65534, use_addr_key=1, size=4, access=2]");
}

TEST(PhaseScriptProgram, DefaultContractsRemainStable)
{
    savor::PhaseScript program{};
    EXPECT_TRUE(program.canonical_bp_keys.empty());
    EXPECT_TRUE(program.gated_bp_keys.empty());
    EXPECT_TRUE(program.ops.empty());

    savor::PSInit init{};
    EXPECT_EQ(init.derived_buffer_type, savor::DBuf::DK_None);

    savor::PSResult result{};
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.w_err, 0u);
    EXPECT_STREQ(savor::RunToBpOutcomeToString(
        static_cast<uint32_t>(savor::RunToBpOutcome::InputPlaybackFailed)), "InputPlaybackFailed");
    EXPECT_STREQ(savor::RunToBpOutcomeToString(6), "UnrecognizedRunOutcome");
}
