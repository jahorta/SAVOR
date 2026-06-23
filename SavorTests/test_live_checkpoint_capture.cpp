#include <gtest/gtest.h>

#include "CheckpointTrace.h"
#include "Core/PowerPcMemoryAccessDecoder.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "LiveCaptureProfile.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/Capture/CaptureJsonlWriter.h"
#include "Runner/Capture/CaptureProfile.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContext.h"
#include "Runner/Script/PhaseScriptVM.h"

#include <array>
#include <cstdint>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

using namespace savor::capture;
using namespace savor::predict;

std::uint32_t EncodeDForm(std::uint32_t primary, std::uint32_t rt, std::uint32_t ra, std::uint16_t imm)
{
    return (primary << 26) | (rt << 21) | (ra << 16) | imm;
}

std::uint32_t EncodeXForm(std::uint32_t rt, std::uint32_t ra, std::uint32_t rb, std::uint32_t xo)
{
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1);
}

struct TestRegisterFile {
    std::array<std::uint32_t, 32> values{};
    std::vector<std::uint8_t> reads;

    bool read(std::uint8_t reg, std::uint32_t& out)
    {
        reads.push_back(reg);
        out = values[reg];
        return true;
    }
};

TEST(LiveCheckpointCaptureProfile, ParsesDefaultSamplesAndUniquePcs)
{
    const std::string text =
        "[profile]\n"
        "name=test_capture\n"
        "schema_version=1\n"
        "memory=rng_seed_before:0x803469A8:u32, wide_counter:0x80000000:u64\n"
        "gprs=return_value:3\n"
        "reg_memory=payload_mode:r3:0x22:u16, saved_mode:31:-0x10:u16\n"
        "addrprog=payload_mode_chain:r3:+0x24|load_ptr32|+0x22:u16\n"
        "addrprog_trace=true\n"
        "\n"
        "[watchpoint.field6_writer]\n"
        "address=0x81234567\n"
        "size=u16\n"
        "access=write\n"
        "\n"
        "[checkpoint.first]\n"
        "pc=0x80001000\n"
        "name=first_draw\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=true\n"
        "reg_memory=payload_flags:r3:0x10:u32\n"
        "addrprog=actor_field6:r29:+0x6:u16\n"
        "addrprog_trace=false\n"
        "\n"
        "[checkpoint.second]\n"
        "pc=0x80001000\n"
        "name=second_label_same_pc\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=false\n";

    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "test_capture");
    ASSERT_EQ(profile.checkpoints.size(), 2u);
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "field6_writer");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x81234567u);
    EXPECT_EQ(profile.memory_watchpoints[0].size, SampleWidth::U16);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_EQ(profile.checkpoints[0].memory_samples.size(), 2u);
    EXPECT_EQ(profile.checkpoints[0].memory_samples[0].name, "rng_seed_before");
    EXPECT_EQ(profile.checkpoints[0].memory_samples[0].width, SampleWidth::U32);
    EXPECT_EQ(profile.checkpoints[0].memory_samples[1].width, SampleWidth::U64);
    EXPECT_EQ(profile.checkpoints[0].gpr_samples.size(), 1u);
    ASSERT_EQ(profile.checkpoints[0].register_memory_samples.size(), 3u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].name, "payload_mode");
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].base_reg, 3u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[0].offset, 0x22);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[1].base_reg, 31u);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[1].offset, -0x10);
    EXPECT_EQ(profile.checkpoints[0].register_memory_samples[2].name, "payload_flags");
    ASSERT_EQ(profile.checkpoints[0].address_program_samples.size(), 2u);
    EXPECT_EQ(profile.checkpoints[0].address_program_samples[0].name, "payload_mode_chain");
    EXPECT_EQ(profile.checkpoints[0].address_program_samples[0].width, SampleWidth::U16);
    EXPECT_EQ(profile.checkpoints[0].address_program_samples[1].name, "actor_field6");
    EXPECT_EQ(profile.checkpoints[0].address_program_samples[1].width, SampleWidth::U16);
    EXPECT_FALSE(profile.checkpoints[0].address_program_trace);
    EXPECT_TRUE(profile.checkpoints[0].owns_rng_draw);
    EXPECT_FALSE(profile.checkpoints[1].owns_rng_draw);

    const auto pcs = profile.pcs();
    ASSERT_EQ(pcs.size(), 1u);
    EXPECT_EQ(pcs[0], 0x80001000u);
}

TEST(AddrProgramEvaluator, EvaluatesRegisterRootedAddressWithTrace)
{
    addrprog::Builder builder;
    builder.op_base_gpr(3);
    builder.op_add_i32(0x24);
    builder.op_index(2, 4);
    builder.op_end();

    savor::DolphinWrapper host;
    const auto& blob = builder.blob();
    const auto result = addrprog::evaluate(
        blob.data(),
        blob.size(),
        0,
        host,
        nullptr,
        [](std::uint8_t reg, std::uint32_t& out) {
            if (reg != 3) return false;
            out = 0x812A3000u;
            return true;
        },
        true);

    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.va, 0x812A302Cu);
    ASSERT_EQ(result.trace.size(), 4u);
    EXPECT_EQ(result.trace[0].op_name, "BASE_GPR");
    EXPECT_EQ(result.trace[0].address_after, 0x812A3000u);
    EXPECT_EQ(result.trace[1].op_name, "ADD_I32");
    EXPECT_EQ(result.trace[1].address_after, 0x812A3024u);
    EXPECT_EQ(result.trace[2].op_name, "INDEX");
    EXPECT_EQ(result.trace[2].address_after, 0x812A302Cu);
    EXPECT_EQ(result.trace[3].op_name, "END");
}

TEST(AddrProgramEvaluator, FailsRegisterRootedAddressWithoutRegisterReader)
{
    addrprog::Builder builder;
    builder.op_base_gpr(3);
    builder.op_end();

    savor::DolphinWrapper host;
    const auto& blob = builder.blob();
    const auto result = addrprog::evaluate(blob.data(), blob.size(), 0, host, nullptr);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error, "BASE_GPR without register reader");
}

TEST(LiveCheckpointCaptureJsonl, SerializesRequiredFieldsAndStableRepeatedHitOrder)
{
    CheckpointCaptureRecord first{};
    first.capture_sequence = 0;
    first.checkpoint_hit_count = 0;
    first.pc = 0x8008B428;
    first.checkpoint_id = "soldier_ai_action";
    first.checkpoint_name = "soldier_ai_action";
    first.function = "RNG";
    first.checkpoint = "soldier_ai_action";
    first.movie_input_count = 11;
    first.vi_field_count = 22;
    first.frame_count = 33;
    first.tbr_u64 = 0x1111111122222222ull;
    first.tbr_high = 0x11111111u;
    first.tbr_low = 0x22222222u;
    first.rng_draw_index_before = 5;
    first.owns_rng_draw = true;
    first.fields.push_back(CaptureField{ "rng_seed_before", "\"0x12345678\"", false });

    CheckpointCaptureRecord second = first;
    second.capture_sequence = 1;
    second.checkpoint_hit_count = 1;
    second.rng_draw_index_before = 6;
    second.tbr_u64 = 0x3333333344444444ull;
    second.tbr_high = 0x33333333u;
    second.tbr_low = 0x44444444u;
    second.fields.clear();
    second.fields.push_back(CaptureField{ "rng_seed_before", "\"0x23456789\"", false });

    const auto first_line = SerializeJsonlRecord(first);
    const auto second_line = SerializeJsonlRecord(second);
    EXPECT_EQ(first_line.find(",}"), std::string::npos);
    EXPECT_NE(first_line.find("\"capture_sequence\":0"), std::string::npos);
    EXPECT_NE(first_line.find("\"stop_kind\":\"pc_breakpoint\""), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_high\":286331153"), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_low\":572662306"), std::string::npos);
    EXPECT_NE(first_line.find("\"tbr_u64_hex\":\"0x1111111122222222\""), std::string::npos);

    std::istringstream input(first_line + "\n" + second_line + "\n");
    const auto parsed = parse_checkpoint_stream(input);
    EXPECT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.events.size(), 2u);
    EXPECT_EQ(parsed.events[0].fields.at("capture_sequence"), "0");
    EXPECT_EQ(parsed.events[0].fields.at("stop_kind"), "pc_breakpoint");
    EXPECT_EQ(parsed.events[0].fields.at("checkpoint_hit_count"), "0");
    EXPECT_EQ(parsed.events[0].fields.at("tbr_high"), "286331153");
    EXPECT_EQ(parsed.events[0].fields.at("tbr_low"), "572662306");
    ASSERT_TRUE(parsed.events[0].rng_draw_index_before.has_value());
    ASSERT_TRUE(parsed.events[0].rng_seed_before.has_value());
    EXPECT_EQ(*parsed.events[0].rng_draw_index_before, 5);
    EXPECT_EQ(*parsed.events[0].rng_seed_before, 0x12345678u);
    EXPECT_EQ(parsed.events[1].fields.at("capture_sequence"), "1");
    EXPECT_EQ(parsed.events[1].fields.at("checkpoint_hit_count"), "1");
    ASSERT_TRUE(parsed.events[1].rng_draw_index_before.has_value());
    ASSERT_TRUE(parsed.events[1].rng_seed_before.has_value());
    EXPECT_EQ(*parsed.events[1].rng_draw_index_before, 6);
    EXPECT_EQ(*parsed.events[1].rng_seed_before, 0x23456789u);
}

TEST(LiveCheckpointCaptureJsonl, SerializesMemoryWatchpointProofFields)
{
    CheckpointCaptureRecord record{};
    record.capture_sequence = 9;
    record.checkpoint_hit_count = 0;
    record.pc = 0x8003C730;
    record.stop_kind = "memcheck";
    record.checkpoint_id = "memwatch.field6_writer";
    record.checkpoint_name = "field6_writer";
    record.function = "memory_watchpoint";
    record.checkpoint = "write";
    record.movie_input_count = 1;
    record.vi_field_count = 2;
    record.frame_count = 3;
    record.tbr_u64 = 0x1111111122222222ull;
    record.tbr_high = 0x11111111u;
    record.tbr_low = 0x22222222u;
    record.fields.push_back(CaptureField{ "memwatch_confirmed_current_instruction", "true", false });
    record.fields.push_back(CaptureField{ "memwatch_unattributed_extra_hits", "2", false });
    record.fields.push_back(CaptureField{ "decoded_opcode", "\"0x93C30178\"", false });
    record.fields.push_back(CaptureField{ "decoded_mnemonic", "stw", true });
    record.fields.push_back(CaptureField{ "decoded_effective_addr", "\"0x812A3F78\"", false });
    record.fields.push_back(CaptureField{ "decoded_value", "\"0x0000000080F46320\"", false });
    record.fields.push_back(CaptureField{ "decoded_value_source", "source_gpr", true });
    record.fields.push_back(CaptureField{ "decoded_memory_value", "\"0x0000000080F46320\"", false });
    record.fields.push_back(CaptureField{ "decoded_gpr_r3_value", "\"0x812A3E00\"", false });
    record.fields.push_back(CaptureField{ "decoded_gpr_r30_value", "\"0x80F46320\"", false });

    std::istringstream input(SerializeJsonlRecord(record) + "\n");
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.events.size(), 1u);
    EXPECT_EQ(parsed.events[0].fields.at("memwatch_confirmed_current_instruction"), "true");
    EXPECT_EQ(parsed.events[0].fields.at("memwatch_unattributed_extra_hits"), "2");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_mnemonic"), "stw");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_effective_addr"), "0x812A3F78");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_value"), "0x0000000080F46320");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_value_source"), "source_gpr");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_memory_value"), "0x0000000080F46320");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_gpr_r3_value"), "0x812A3E00");
    EXPECT_EQ(parsed.events[0].fields.at("decoded_gpr_r30_value"), "0x80F46320");
}

TEST(VMMemoryWatchpoints, ConstructsLiteralAndContextAddressOps)
{
    const auto literal = savor::OpArmMemoryWatchpoint(
        7,
        0x81234567u,
        2,
        savor::DolphinWrapper::MemoryWatchpointAccess::Write);
    EXPECT_EQ(literal.code, savor::PSOpCode::ARM_MEMORY_WATCHPOINT);
    EXPECT_EQ(literal.memwatch.id, 7u);
    EXPECT_EQ(literal.memwatch.address, 0x81234567u);
    EXPECT_EQ(literal.memwatch.use_address_key, 0u);
    EXPECT_EQ(literal.memwatch.size, 2u);
    EXPECT_EQ(literal.memwatch.access, static_cast<std::uint32_t>(savor::DolphinWrapper::MemoryWatchpointAccess::Write));

    const auto from_key = savor::OpArmMemoryWatchpointFromKey(
        8,
        savor::context::key::core::RUN_HIT_PC,
        4,
        savor::DolphinWrapper::MemoryWatchpointAccess::Access);
    EXPECT_EQ(from_key.code, savor::PSOpCode::ARM_MEMORY_WATCHPOINT);
    EXPECT_EQ(from_key.memwatch.id, 8u);
    EXPECT_EQ(from_key.memwatch.address_key, savor::context::key::core::RUN_HIT_PC);
    EXPECT_EQ(from_key.memwatch.use_address_key, 1u);
    EXPECT_EQ(from_key.memwatch.size, 4u);
    EXPECT_EQ(from_key.memwatch.access, static_cast<std::uint32_t>(savor::DolphinWrapper::MemoryWatchpointAccess::Access));

    EXPECT_EQ(savor::OpRunUntilDebugStop().code, savor::PSOpCode::RUN_UNTIL_DEBUG_STOP);
    EXPECT_EQ(savor::OpClearMemoryWatchpoints().code, savor::PSOpCode::CLEAR_MEMORY_WATCHPOINTS);
    EXPECT_EQ(savor::OpArmCaptureMemoryWatchpoints().code, savor::PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS);
}

TEST(VMMemoryWatchpoints, DecodesDFormStoreWithOnlyRelevantRegisters)
{
    TestRegisterFile rf;
    rf.values[3] = 0x812A3E00u;
    rf.values[30] = 0x80F46320u;

    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x8003C730u,
        EncodeDForm(36, 30, 3, 0x0178u),
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); });

    EXPECT_TRUE(decoded.decoded);
    EXPECT_TRUE(decoded.supported);
    EXPECT_TRUE(decoded.is_memory_access);
    EXPECT_EQ(decoded.mnemonic, "stw");
    EXPECT_EQ(decoded.access, savor::ppc::MemoryAccessKind::Write);
    EXPECT_EQ(decoded.effective_address, 0x812A3F78u);
    EXPECT_EQ(decoded.access_size, 4u);
    ASSERT_TRUE(decoded.value_available);
    EXPECT_EQ(decoded.value, 0x80F46320ull);
    EXPECT_EQ(decoded.value_source, "source_gpr");
    EXPECT_EQ(rf.reads, (std::vector<std::uint8_t>{ 3, 30 }));
}

TEST(VMMemoryWatchpoints, DecodesHalfwordStoreValueMask)
{
    TestRegisterFile rf;
    rf.values[3] = 0x812A3F20u;
    rf.values[0] = 0xAABBCCDDu;

    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x80051400u,
        EncodeDForm(44, 0, 3, 0x0022u),
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); });

    EXPECT_TRUE(decoded.supported);
    EXPECT_EQ(decoded.mnemonic, "sth");
    EXPECT_EQ(decoded.effective_address, 0x812A3F42u);
    EXPECT_EQ(decoded.access_size, 2u);
    ASSERT_TRUE(decoded.value_available);
    EXPECT_EQ(decoded.value, 0xCCDDull);
    EXPECT_EQ(decoded.value_source, "source_gpr");
    EXPECT_EQ(rf.reads, (std::vector<std::uint8_t>{ 3, 0 }));
}

TEST(VMMemoryWatchpoints, DecodesLoadValueFromMemoryCallback)
{
    TestRegisterFile rf;
    rf.values[3] = 0x812A3F60u;
    std::vector<std::tuple<std::uint32_t, std::uint32_t>> memory_reads;

    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x80001000u,
        EncodeDForm(32, 5, 3, 0x0018u),
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); },
        [&](std::uint32_t address, std::uint32_t size, std::uint64_t& out) {
            memory_reads.emplace_back(address, size);
            out = 0x80F46320u;
            return true;
        });

    EXPECT_TRUE(decoded.supported);
    EXPECT_EQ(decoded.mnemonic, "lwz");
    EXPECT_EQ(decoded.access, savor::ppc::MemoryAccessKind::Read);
    EXPECT_EQ(decoded.effective_address, 0x812A3F78u);
    EXPECT_EQ(decoded.access_size, 4u);
    ASSERT_TRUE(decoded.value_available);
    EXPECT_EQ(decoded.value, 0x80F46320ull);
    EXPECT_EQ(decoded.value_source, "memory");
    ASSERT_TRUE(decoded.memory_value_available);
    EXPECT_EQ(decoded.memory_value, 0x80F46320ull);
    EXPECT_EQ(rf.reads, (std::vector<std::uint8_t>{ 3 }));
    ASSERT_EQ(memory_reads.size(), 1u);
    EXPECT_EQ(std::get<0>(memory_reads[0]), 0x812A3F78u);
    EXPECT_EQ(std::get<1>(memory_reads[0]), 4u);
}

TEST(VMMemoryWatchpoints, DecodesIndexedStoreWithOnlyReferencedRegisters)
{
    TestRegisterFile rf;
    rf.values[3] = 0x812A3000u;
    rf.values[4] = 0x00000F78u;
    rf.values[5] = 0x12345678u;

    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x80001000u,
        EncodeXForm(5, 3, 4, 151),
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); });

    EXPECT_TRUE(decoded.supported);
    EXPECT_EQ(decoded.mnemonic, "stwx");
    EXPECT_EQ(decoded.effective_address, 0x812A3F78u);
    EXPECT_EQ(decoded.access_size, 4u);
    ASSERT_TRUE(decoded.value_available);
    EXPECT_EQ(decoded.value, 0x12345678ull);
    EXPECT_EQ(decoded.value_source, "source_gpr");
    EXPECT_EQ(rf.reads, (std::vector<std::uint8_t>{ 3, 4, 5 }));
}

TEST(VMMemoryWatchpoints, BranchDoesNotConfirmMemcheck)
{
    TestRegisterFile rf;
    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x800513D4u,
        0x48000001u,
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); });

    EXPECT_TRUE(decoded.decoded);
    EXPECT_FALSE(decoded.is_memory_access);
    EXPECT_FALSE(decoded.supported);
    EXPECT_EQ(decoded.mnemonic, "bl");
    EXPECT_TRUE(rf.reads.empty());

    const std::vector<savor::DolphinWrapper::MemoryWatchpointDelta> deltas{
        savor::DolphinWrapper::MemoryWatchpointDelta{
            .id = 1,
            .address = 0x80F46342u,
            .size = 2,
            .access = savor::DolphinWrapper::MemoryWatchpointAccess::Write,
            .hit_pc = 0x800513D4u,
            .num_hits_before = 0,
            .num_hits_after = 8,
        },
    };
    EXPECT_FALSE(savor::DolphinWrapper::ProveMemoryWatchpointHitAtCurrentInstruction(decoded, deltas).has_value());
}

TEST(VMMemoryWatchpoints, ProvesOnlyIntersectingCurrentInstructionAccess)
{
    TestRegisterFile rf;
    rf.values[3] = 0x812A3E00u;
    rf.values[30] = 0x80F46320u;

    const auto decoded = savor::ppc::DecodeCurrentMemoryAccess(
        0x8003C730u,
        EncodeDForm(36, 30, 3, 0x0178u),
        [&](std::uint8_t reg, std::uint32_t& out) { return rf.read(reg, out); });
    const std::vector<savor::DolphinWrapper::MemoryWatchpointDelta> deltas{
        savor::DolphinWrapper::MemoryWatchpointDelta{
            .id = 2,
            .address = 0x812A3F78u,
            .size = 4,
            .access = savor::DolphinWrapper::MemoryWatchpointAccess::Write,
            .hit_pc = 0x8003C730u,
            .num_hits_before = 4,
            .num_hits_after = 7,
        },
    };

    const auto hit = savor::DolphinWrapper::ProveMemoryWatchpointHitAtCurrentInstruction(decoded, deltas);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(hit->confirmed_current_instruction);
    EXPECT_EQ(hit->id, 2u);
    EXPECT_EQ(hit->hit_pc, 0x8003C730u);
    EXPECT_EQ(hit->unattributed_extra_hits, 2u);
    EXPECT_EQ(hit->decoded_access.effective_address, 0x812A3F78u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsParseableFirstBattleRngProfile)
{
    const auto text = build_first_battle_capture_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_live_capture");
    ASSERT_GT(profile.checkpoints.size(), known_rng_callsite_owners().size());
    ASSERT_FALSE(profile.checkpoints.empty());
    int rng_checkpoints = 0;
    int action_view_state_checkpoints = 0;
    bool found_default_targeting_camera = false;
    bool found_default_action_view_dispatch_state = false;
    for (const auto& checkpoint : profile.checkpoints) {
        ASSERT_FALSE(checkpoint.memory_samples.empty()) << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].name, "rng_seed_before") << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].width, SampleWidth::U32) << checkpoint.id;
        if (checkpoint.owns_rng_draw) {
            ++rng_checkpoints;
        }
        if (checkpoint.id == "pre_ai_attack_targeting_camera_800608DC") {
            found_default_targeting_camera = true;
        }
        if (checkpoint.id == "action_view_dispatch_state_80051424") {
            found_default_action_view_dispatch_state = true;
        }
        if (checkpoint.id.find("action_view") != std::string::npos
            || checkpoint.id.find("mode0") != std::string::npos) {
            ++action_view_state_checkpoints;
        }
    }
    EXPECT_EQ(rng_checkpoints, static_cast<int>(known_rng_callsite_owners().size()) - 1);
    EXPECT_FALSE(found_default_targeting_camera);
    EXPECT_FALSE(found_default_action_view_dispatch_state);
    EXPECT_GE(action_view_state_checkpoints, 7);

    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* combat_effect =
        find_checkpoint("combat_effect_spawn_scale_x_80043020");
    ASSERT_NE(combat_effect, nullptr);
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_key_0x28"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_subtype_0x2a"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_secondary_0x2c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_resource_id_0x30"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_timing_raw_0x3c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_x_raw_0x50"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_y_raw_0x54"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_source_pos_z_raw_0x58"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_loop_count_0x5c"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_flags_0x38"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_variant_count_0x5e"));
    EXPECT_TRUE(has_reg_sample(*combat_effect, "effect_axis_mode_0x60"));

    const auto* emitter_source =
        find_checkpoint("effect_emitter_source_gate_80041F30");
    ASSERT_NE(emitter_source, nullptr);
    EXPECT_FALSE(emitter_source->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_outer_count_0x142"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_child_count_0x1c"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_variant_count_0x16"));
    EXPECT_TRUE(has_reg_sample(*emitter_source, "emitter_axis_mode_0x3c"));

    const auto* particle_tick =
        find_checkpoint("effect_particle_motion_gate_x_800425A0");
    ASSERT_NE(particle_tick, nullptr);

    const auto* action_view_query_call =
        find_checkpoint("action_view_category2_query_call_8001331C");
    ASSERT_NE(action_view_query_call, nullptr);
    EXPECT_FALSE(action_view_query_call->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_query_call, "aux_list_root"));
    EXPECT_TRUE(has_gpr_sample(*action_view_query_call, "query_arg1"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "active_slot"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "target_slot"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "actor_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_call, "gate_state_0x30"));

    const auto* action_view_query_result =
        find_checkpoint("action_view_category2_query_result_80013320");
    ASSERT_NE(action_view_query_result, nullptr);
    EXPECT_FALSE(action_view_query_result->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_query_result, "query_result"));
    EXPECT_TRUE(has_reg_sample(*action_view_query_result, "actor_field6_0x6"));

    const auto* action_view_spawn =
        find_checkpoint("action_view_category2_spawn_80013334");
    ASSERT_NE(action_view_spawn, nullptr);
    EXPECT_FALSE(action_view_spawn->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_spawn, "spawn_slot_arg"));
    EXPECT_TRUE(has_gpr_sample(*action_view_spawn, "spawn_mode_arg"));

    const auto* effect_record_copy =
        find_checkpoint("effect_record_copy_complete_8003BB24");
    ASSERT_NE(effect_record_copy, nullptr);
    EXPECT_FALSE(effect_record_copy->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r6_effect_buffer"));
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r30_parent_action_thread"));
    EXPECT_TRUE(has_gpr_sample(*effect_record_copy, "r31_source_record"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "effect_parent_action_thread_0x04"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "effect_source_key_0x28"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "source_record_key_0x00"));
    EXPECT_TRUE(has_reg_sample(*effect_record_copy, "source_record_loop_count_0x34"));
    EXPECT_TRUE(has_reg_sample(*particle_tick, "particle_payload_source_ptr_0x20"));
    EXPECT_TRUE(has_reg_sample(*particle_tick, "particle_payload_lifetime_0x28"));

    const auto* attack_begin =
        find_checkpoint("attack_resolution_begin_80081B94");
    ASSERT_NE(attack_begin, nullptr);
    EXPECT_FALSE(attack_begin->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*attack_begin, "target_slot_arg"));
    EXPECT_TRUE(has_gpr_sample(*attack_begin, "actor_slot_arg"));

    const auto* damage_apply =
        find_checkpoint("damage_apply_death_call_8002DD14");
    ASSERT_NE(damage_apply, nullptr);
    EXPECT_FALSE(damage_apply->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*damage_apply, "enemy_id_slot4"));
    EXPECT_TRUE(has_memory_sample(*damage_apply, "enemy_id_slot5"));
    EXPECT_TRUE(has_gpr_sample(*damage_apply, "target_slot"));
    EXPECT_TRUE(has_gpr_sample(*damage_apply, "damage"));
    EXPECT_TRUE(has_gpr_sample(*damage_apply, "hp_after"));

    const auto* death_handler =
        find_checkpoint("death_handler_hp_gate_8002BC80");
    ASSERT_NE(death_handler, nullptr);
    EXPECT_FALSE(death_handler->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*death_handler, "cur_hp"));
    EXPECT_TRUE(has_gpr_sample(*death_handler, "combatant_instance"));
    EXPECT_TRUE(has_gpr_sample(*death_handler, "target_slot"));

    const auto* enemy_drop_call =
        find_checkpoint("enemy_drop_call_8002BD20");
    ASSERT_NE(enemy_drop_call, nullptr);
    EXPECT_FALSE(enemy_drop_call->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*enemy_drop_call, "target_slot"));
    EXPECT_TRUE(has_gpr_sample(*enemy_drop_call, "enemy_def_ptr"));

    const auto* drop_entry =
        find_checkpoint("enemy_drop_entry_8002BA8C");
    ASSERT_NE(drop_entry, nullptr);
    EXPECT_FALSE(drop_entry->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*drop_entry, "enemy_id_slot4"));
    EXPECT_TRUE(has_gpr_sample(*drop_entry, "target_slot"));

    const auto* drop_roll =
        find_checkpoint("enemy_drop_roll_8002BAD8");
    ASSERT_NE(drop_roll, nullptr);
    EXPECT_TRUE(drop_roll->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*drop_roll, "enemy_id_slot4"));
    EXPECT_TRUE(has_gpr_sample(*drop_roll, "target_slot"));
    EXPECT_TRUE(has_gpr_sample(*drop_roll, "drop_row_index_zero_based"));
    EXPECT_TRUE(has_gpr_sample(*drop_roll, "drop_threshold"));
    EXPECT_TRUE(has_reg_sample(*drop_roll, "drop_item_id"));
    EXPECT_TRUE(has_reg_sample(*drop_roll, "drop_amount"));
    EXPECT_TRUE(has_reg_sample(*drop_roll, "drop_row_chance_raw"));

    const auto* source_selection =
        find_checkpoint("action_source_selection_80067BD0");
    ASSERT_NE(source_selection, nullptr);
    EXPECT_FALSE(source_selection->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*source_selection, "selected_source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_selection, "controller_actor_slot_0x92"));

    const auto* source_bridge =
        find_checkpoint("action_source_field6_bridge_8006778C");
    ASSERT_NE(source_bridge, nullptr);
    EXPECT_FALSE(source_bridge->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*source_bridge, "actor_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "target_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "actor_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "source_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "handler_pc"));
    EXPECT_TRUE(has_reg_sample(*source_bridge, "callback_0xe0"));

    const auto* sst_case2 =
        find_checkpoint("sst_action_field6_case2_store_complete_8000C4C8");
    ASSERT_NE(sst_case2, nullptr);
    EXPECT_FALSE(sst_case2->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r0_written_field6"));
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r3_destination_worksheet"));
    EXPECT_TRUE(has_gpr_sample(*sst_case2, "r29_serialized_command"));
    EXPECT_TRUE(has_reg_sample(*sst_case2, "source_field6_0x06"));
    EXPECT_TRUE(has_reg_sample(*sst_case2, "dest_field6_after_0x06"));

    const auto* sst_case8 =
        find_checkpoint("sst_action_field6_case8_store_complete_8000C6E8");
    ASSERT_NE(sst_case8, nullptr);
    EXPECT_FALSE(sst_case8->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r0_written_field6"));
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r3_destination_worksheet"));
    EXPECT_TRUE(has_gpr_sample(*sst_case8, "r30_serialized_command"));
    EXPECT_TRUE(has_reg_sample(*sst_case8, "source_field6_0x0a"));
    EXPECT_TRUE(has_reg_sample(*sst_case8, "dest_field6_after_0x06"));
}

TEST(SavorPredictLiveCaptureProfile, BuildsPredictorValidationProfile)
{
    const auto text = build_first_battle_predictor_validation_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_predictor_validation");
    bool found_targeting_camera = false;
    bool found_progress_pc = false;
    for (const auto& checkpoint : profile.checkpoints) {
        ASSERT_FALSE(checkpoint.memory_samples.empty()) << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].name, "rng_seed_before") << checkpoint.id;
        if (checkpoint.pc == 0x800608DCu) {
            found_targeting_camera = true;
        }
        const auto lowered_id = checkpoint.id;
        if (lowered_id.find("progress") != std::string::npos) {
            found_progress_pc = true;
        }
    }
    EXPECT_FALSE(found_targeting_camera);
    EXPECT_FALSE(found_progress_pc);

    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* hit = find_checkpoint("attack_hit_dodge_80010BDC");
    ASSERT_NE(hit, nullptr);
    EXPECT_TRUE(hit->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*hit, "slot0_instr_param_0x6"));
    EXPECT_TRUE(has_memory_sample(*hit, "slot4_attack_result_0xc"));
    EXPECT_TRUE(has_gpr_sample(*hit, "r0_instr_param_candidate"));
    EXPECT_TRUE(has_gpr_sample(*hit, "attack_result"));

    const auto* crit_gate = find_checkpoint("crit_gate_branch_80010C40");
    ASSERT_NE(crit_gate, nullptr);
    EXPECT_FALSE(crit_gate->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*crit_gate, "slot1_instr_param_0x6"));
    EXPECT_TRUE(has_gpr_sample(*crit_gate, "instr_param_0x6"));

    EXPECT_NE(find_checkpoint("pc_attack_fallback_param_set_b_800856C4"), nullptr);
    EXPECT_NE(find_checkpoint("pc_fallback_attack_worker_entry_80085CE0"), nullptr);
    EXPECT_NE(find_checkpoint("enemy_direct_worker_select_8008BDAC"), nullptr);

    const auto* enemy_helper = find_checkpoint("enemy_helper_8008a174_return_8008BCB0");
    ASSERT_NE(enemy_helper, nullptr);
    EXPECT_TRUE(has_gpr_sample(*enemy_helper, "helper_result"));

    const auto* counter_inputs = find_checkpoint("counter_gate_inputs_800819FC");
    ASSERT_NE(counter_inputs, nullptr);
    EXPECT_TRUE(has_memory_sample(*counter_inputs, "slot4_critical_marker_0x8"));
    EXPECT_TRUE(has_gpr_sample(*counter_inputs, "target_instance"));
    EXPECT_TRUE(has_reg_sample(*counter_inputs, "target_status_flags"));
    EXPECT_TRUE(has_reg_sample(*counter_inputs, "target_current_counter_chance"));

    const auto* counter_roll = find_checkpoint("counter_roll_80081A88");
    ASSERT_NE(counter_roll, nullptr);
    EXPECT_TRUE(counter_roll->owns_rng_draw);
    EXPECT_TRUE(has_memory_sample(*counter_roll, "slot1_action_marker_0x0"));
    EXPECT_TRUE(has_reg_sample(*counter_roll, "target_base_counter_chance"));

    EXPECT_NE(find_checkpoint("counter_followup_call_prepare_80081D80"), nullptr);
    EXPECT_NE(find_checkpoint("action_source_selection_entry_8006782C"), nullptr);
    EXPECT_NE(find_checkpoint("action_source_selection_candidate_80067A9C"), nullptr);
    EXPECT_NE(find_checkpoint("action_source_selection_source_slot_80067B50"), nullptr);
}

TEST(SavorPredictLiveCaptureProfile, BuildsTurnOrderValidationProfile)
{
    const auto text = build_first_battle_turn_order_validation_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_turn_order_validation");

    bool found_progress_pc = false;
    bool found_targeting_camera = false;
    for (const auto& checkpoint : profile.checkpoints) {
        ASSERT_FALSE(checkpoint.memory_samples.empty()) << checkpoint.id;
        EXPECT_EQ(checkpoint.memory_samples[0].name, "rng_seed_before") << checkpoint.id;
        if (checkpoint.pc == 0x800608DCu) {
            found_targeting_camera = true;
        }
        if (checkpoint.id.find("progress") != std::string::npos
            || checkpoint.name.find("progress") != std::string::npos) {
            found_progress_pc = true;
        }
    }
    EXPECT_FALSE(found_targeting_camera);
    EXPECT_FALSE(found_progress_pc);
    EXPECT_GT(profile.find_checkpoints(0x80071408u).size(), 1u);
    EXPECT_GT(profile.find_checkpoints(0x8007140Cu).size(), 1u);

    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* priority = find_checkpoint("turn_order_priority_jitter_800711F8");
    ASSERT_NE(priority, nullptr);
    EXPECT_TRUE(priority->owns_rng_draw);

    const auto* qsort_call = find_checkpoint("turn_order_qsort_call_80071408");
    ASSERT_NE(qsort_call, nullptr);
    EXPECT_FALSE(qsort_call->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*qsort_call, "queued_count"));
    EXPECT_TRUE(has_gpr_sample(*qsort_call, "qsort_elem_size_arg"));

    const auto* input0 = find_checkpoint("turn_order_qsort_input_entry_0_80071408");
    ASSERT_NE(input0, nullptr);
    EXPECT_EQ(input0->checkpoint, "qsort_input");
    EXPECT_TRUE(has_memory_sample(*input0, "record_word0"));
    EXPECT_TRUE(has_memory_sample(*input0, "slot"));
    EXPECT_TRUE(has_memory_sample(*input0, "assigned_priority"));
    EXPECT_TRUE(has_memory_sample(*input0, "record_word8"));

    const auto* output7 = find_checkpoint("turn_order_qsort_output_entry_7_8007140C");
    ASSERT_NE(output7, nullptr);
    EXPECT_EQ(output7->checkpoint, "qsort_output");
    EXPECT_TRUE(has_memory_sample(*output7, "record_word0"));
    EXPECT_TRUE(has_memory_sample(*output7, "slot"));
    EXPECT_TRUE(has_memory_sample(*output7, "assigned_priority"));
    EXPECT_TRUE(has_memory_sample(*output7, "record_word8"));

    const auto* execution3 = find_checkpoint("turn_order_execution_order_entry_3_8007154C");
    ASSERT_NE(execution3, nullptr);
    EXPECT_EQ(execution3->checkpoint, "execution_order");
    EXPECT_TRUE(has_memory_sample(*execution3, "slot"));
}

TEST(BattleTurnRunnerPayload, RoundTripsLiveCaptureContextPaths)
{
    phase::battle::turnrunner::EncodeSpec spec{};
    spec.run_ms = 120000;
    spec.vi_stall_ms = 5000;
    spec.capture_profile_path = "D:/SavorPredictDB/capture/first_battle.ini";
    spec.capture_output_path = "D:/SavorPredictDB/capture/job_1.jsonl";
    spec.override_start_rng_seed = 0x12345678u;

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(phase::battle::turnrunner::encode_payload(spec, payload));

    savor::PSContext ctx;
    ASSERT_TRUE(phase::battle::turnrunner::decode_payload(payload, ctx));

    std::string profile_path;
    std::string output_path;
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_PROFILE_PATH, profile_path));
    ASSERT_TRUE(ctx.get(savor::context::key::core::CAPTURE_OUTPUT_PATH, output_path));
    EXPECT_EQ(profile_path, spec.capture_profile_path);
    EXPECT_EQ(output_path, spec.capture_output_path);

    uint32_t override_enabled = 0;
    uint32_t override_seed = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_ENABLED, override_enabled));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_SEED, override_seed));
    EXPECT_EQ(override_enabled, 1u);
    EXPECT_EQ(override_seed, 0x12345678u);
}

} // namespace
