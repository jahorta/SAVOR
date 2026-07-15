#include <gtest/gtest.h>

#include "CheckpointTrace.h"
#include "Core/PowerPcMemoryAccessDecoder.h"
#include "Core/Memory/Soa/SoaAddrProgramBuilder.h"
#include "Field6WatchpointModel.h"
#include "LiveCaptureProfile.h"
#include "Phases/Programs/BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "Runner/Capture/CaptureJsonlWriter.h"
#include "Runner/Capture/LinkedListSnapshot.h"
#include "Runner/Capture/CaptureProfile.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContext.h"
#include "Runner/Script/PhaseScriptVM.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
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

LinkedListSnapshotSpec TestThreadListSpec(std::uint32_t max_nodes = 64)
{
    LinkedListSnapshotSpec spec{};
    spec.name = "thread_list";
    spec.head_ptr_address = 0x80311A84u;
    spec.next_offset = 0x04;
    spec.max_nodes = max_nodes;
    spec.fields = {
        LinkedListFieldSpec{ "callback", 0x00, SampleWidth::U32 },
        LinkedListFieldSpec{ "next", 0x04, SampleWidth::U32 },
        LinkedListFieldSpec{ "parent", 0x08, SampleWidth::U32 },
        LinkedListFieldSpec{ "flags", 0x18, SampleWidth::U8 },
    };
    return spec;
}

std::string capture_field_value(const std::vector<CaptureField>& fields, std::string_view name)
{
    for (const auto& field : fields) {
        if (field.name == name) {
            return field.value;
        }
    }
    return {};
}

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
        "linked_list=thread_list:head_ptr=0x80311A84,next=0x04,max=64,fields=callback@0x00:u32|next@0x04:u32|parent@0x08:u32|flags@0x18:u8|depth@0x1b:u8|order_bits@0x20:u32|payload_word@0x24:u32\n"
        "addrprog_trace=true\n"
        "\n"
        "[watchpoint.field6_writer]\n"
        "address=0x81234567\n"
        "size=u16\n"
        "access=write\n"
        "scope=input_macro\n"
        "owns_rng_draw=true\n"
        "\n"
        "[dynamic_watchpoint.actor_field6]\n"
        "pc=0x8001331C\n"
        "base_gpr=r28\n"
        "offset=0x6\n"
        "size=u16\n"
        "access=access\n"
        "scope=input_macro\n"
        "one_shot=true\n"
        "\n"
        "[dynamic_watchpoint.absolute_after_turn_order]\n"
        "pc=0x8007154C\n"
        "address=0x812F5086\n"
        "size=u16\n"
        "access=access\n"
        "\n"
        "[dynamic_watchpoint.pointer_chased_field6]\n"
        "pc=0x80086F48\n"
        "addrprog=r31:+0x24|load_ptr32|+0x4c|load_ptr32|+0x6\n"
        "size=u16\n"
        "access=access\n"
        "one_shot=true\n"
        "owns_rng_draw=true\n"
        "\n"
        "[checkpoint.first]\n"
        "pc=0x80001000\n"
        "name=first_draw\n"
        "function=RNG\n"
        "checkpoint=draw\n"
        "owns_rng_draw=true\n"
        "max_hits=2\n"
        "reg_memory=payload_flags:r3:0x10:u32\n"
        "addrprog=actor_field6:r29:+0x6:u16\n"
        "linked_list=local_thread_children:head_ptr=0x80311A90,next=0x04,max=4,fields=callback@0x00:u32|parent@0x08:u32\n"
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
    ASSERT_EQ(profile.dynamic_memory_watchpoints.size(), 3u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "field6_writer");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x81234567u);
    EXPECT_EQ(profile.memory_watchpoints[0].size, SampleWidth::U16);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_EQ(profile.memory_watchpoints[0].scope, WatchpointScope::InputMacro);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);
    ASSERT_EQ(profile.default_linked_list_samples.size(), 1u);
    EXPECT_EQ(profile.default_linked_list_samples[0].name, "thread_list");
    EXPECT_EQ(profile.default_linked_list_samples[0].head_ptr_address, 0x80311A84u);
    EXPECT_EQ(profile.default_linked_list_samples[0].next_offset, 0x04);
    EXPECT_EQ(profile.default_linked_list_samples[0].max_nodes, 64u);
    ASSERT_EQ(profile.default_linked_list_samples[0].fields.size(), 7u);
    EXPECT_EQ(profile.default_linked_list_samples[0].fields[0].name, "callback");
    EXPECT_EQ(profile.default_linked_list_samples[0].fields[0].width, SampleWidth::U32);
    EXPECT_EQ(profile.default_linked_list_samples[0].fields[4].name, "depth");
    EXPECT_EQ(profile.default_linked_list_samples[0].fields[4].width, SampleWidth::U8);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].id, "actor_field6");
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].pc, 0x8001331Cu);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].base_reg, 28u);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].offset, 0x6);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].size, SampleWidth::U16);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].access, WatchpointAccess::Access);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[0].scope, WatchpointScope::InputMacro);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints[0].one_shot);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].id, "absolute_after_turn_order");
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].pc, 0x8007154Cu);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints[1].use_absolute_address);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].address, 0x812F5086u);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].size, SampleWidth::U16);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].access, WatchpointAccess::Access);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[1].scope, WatchpointScope::Normal);
    EXPECT_FALSE(profile.dynamic_memory_watchpoints[1].owns_rng_draw);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[2].id, "pointer_chased_field6");
    EXPECT_EQ(profile.dynamic_memory_watchpoints[2].pc, 0x80086F48u);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints[2].use_address_program);
    EXPECT_FALSE(profile.dynamic_memory_watchpoints[2].address_program.empty());
    EXPECT_EQ(profile.dynamic_memory_watchpoints[2].size, SampleWidth::U16);
    EXPECT_EQ(profile.dynamic_memory_watchpoints[2].access, WatchpointAccess::Access);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints[2].one_shot);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints[2].owns_rng_draw);
    EXPECT_FALSE(profile.dynamic_memory_watchpoints[1].one_shot);
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
    ASSERT_EQ(profile.checkpoints[0].linked_list_samples.size(), 2u);
    EXPECT_EQ(profile.checkpoints[0].linked_list_samples[0].name, "thread_list");
    EXPECT_EQ(profile.checkpoints[0].linked_list_samples[1].name, "local_thread_children");
    EXPECT_EQ(profile.checkpoints[0].linked_list_samples[1].head_ptr_address, 0x80311A90u);
    EXPECT_EQ(profile.checkpoints[0].linked_list_samples[1].max_nodes, 4u);
    ASSERT_EQ(profile.checkpoints[1].linked_list_samples.size(), 1u);
    EXPECT_EQ(profile.checkpoints[1].linked_list_samples[0].name, "thread_list");
    EXPECT_FALSE(profile.checkpoints[0].address_program_trace);
    EXPECT_TRUE(profile.checkpoints[0].owns_rng_draw);
    ASSERT_TRUE(profile.checkpoints[0].max_hits.has_value());
    EXPECT_EQ(*profile.checkpoints[0].max_hits, 2u);
    EXPECT_FALSE(profile.checkpoints[1].owns_rng_draw);
    EXPECT_FALSE(profile.checkpoints[1].max_hits.has_value());

    const auto pcs = profile.pcs();
    ASSERT_EQ(pcs.size(), 1u);
    EXPECT_EQ(pcs[0], 0x80001000u);
}

TEST(LiveCheckpointCaptureProfile, RejectsInvalidLinkedListSamples)
{
    const auto parse_with_linked_list = [](std::string linked_list) {
        const std::string text =
            "[profile]\n"
            "name=bad_capture\n"
            "schema_version=1\n"
            "linked_list=" + linked_list + "\n\n"
            "[checkpoint.first]\n"
            "pc=0x80001000\n"
            "name=first\n"
            "function=test\n"
            "checkpoint=test\n"
            "owns_rng_draw=false\n";
        return ParseCaptureProfileText(text);
    };

    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:next=0x04,max=64,fields=callback@0x00:u32").profile.has_value());
    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:head_ptr=0x80311A84,max=64,fields=callback@0x00:u32").profile.has_value());
    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:head_ptr=0x80311A84,next=0x04,max=0,fields=callback@0x00:u32").profile.has_value());
    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:head_ptr=0x80311A84,next=0x04,max=257,fields=callback@0x00:u32").profile.has_value());
    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:head_ptr=0x80311A84,next=0x04,max=64,fields=callback@0x00:f32").profile.has_value());
    EXPECT_FALSE(parse_with_linked_list(
        "thread_list:head_ptr=0x80311A84,next=0x04,max=64,fields=callback@0x00:u32|callback@0x04:u32").profile.has_value());
}

TEST(LiveCheckpointCaptureProfile, RejectsInvalidCheckpointMaxHits)
{
    const auto parse_with_max_hits = [](std::string max_hits) {
        const std::string text =
            "[profile]\n"
            "name=bad_capture\n"
            "schema_version=1\n\n"
            "[checkpoint.first]\n"
            "pc=0x80001000\n"
            "name=first\n"
            "function=test\n"
            "checkpoint=test\n"
            "owns_rng_draw=false\n"
            "max_hits=" + max_hits + "\n";
        return ParseCaptureProfileText(text);
    };

    EXPECT_TRUE(parse_with_max_hits("1").profile.has_value());
    EXPECT_FALSE(parse_with_max_hits("0").profile.has_value());
    EXPECT_FALSE(parse_with_max_hits("-1").profile.has_value());
    EXPECT_FALSE(parse_with_max_hits("abc").profile.has_value());
}

TEST(LiveCheckpointCaptureProfile, ParsesDeferredCheckpointActivationPc)
{
    const std::string text =
        "[profile]\n"
        "name=deferred_capture\n"
        "schema_version=1\n\n"
        "[checkpoint.frame_after_setup]\n"
        "pc=0x8000A2FC\n"
        "activate_on_pc=0x80082134\n"
        "name=frame_after_setup\n"
        "function=Battle::_battleController_8000a118\n"
        "checkpoint=case5_after_threads\n"
        "owns_rng_draw=false\n";

    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);
    ASSERT_EQ(parsed.profile->checkpoints.size(), 1u);
    ASSERT_TRUE(parsed.profile->checkpoints[0].activate_on_pc.has_value());
    EXPECT_EQ(*parsed.profile->checkpoints[0].activate_on_pc, 0x80082134u);
}

TEST(LiveCheckpointCapture, ArmsDeferredCheckpointAfterActivationPc)
{
    const std::string text =
        "[profile]\n"
        "name=deferred_capture\n"
        "schema_version=1\n\n"
        "[checkpoint.frame_after_setup]\n"
        "pc=0x8000A2FC\n"
        "activate_on_pc=0x80082134\n"
        "name=frame_after_setup\n"
        "function=Battle::_battleController_8000a118\n"
        "checkpoint=case5_after_threads\n"
        "owns_rng_draw=false\n";

    const auto temp_root = std::filesystem::temp_directory_path() / "savor_deferred_capture_test";
    std::filesystem::create_directories(temp_root);
    const auto profile_path = temp_root / "profile.ini";
    const auto output_path = temp_root / "capture.jsonl";
    {
        std::ofstream out(profile_path, std::ios::binary | std::ios::trunc);
        out << text;
    }

    LiveCheckpointCapture capture;
    std::string error;
    ASSERT_TRUE(capture.start(profile_path, output_path, &error)) << error;

    EXPECT_TRUE(capture.contains_pc(0x80082134u));
    EXPECT_FALSE(capture.contains_pc(0x8000A2FCu));
    ASSERT_EQ(capture.pcs().size(), 1u);
    EXPECT_EQ(capture.pcs()[0], 0x80082134u);

    capture.activate_deferred_checkpoints_for_pc(0x80082134u);

    const auto activated = capture.take_newly_activated_pcs();
    ASSERT_EQ(activated.size(), 1u);
    EXPECT_EQ(activated[0], 0x8000A2FCu);
    EXPECT_FALSE(capture.contains_pc(0x80082134u));
    EXPECT_TRUE(capture.contains_pc(0x8000A2FCu));
}

TEST(LiveCheckpointLinkedListSnapshot, CapturesEmptyList)
{
    std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80311A84u, 0u },
    };
    const auto fields = BuildLinkedListSnapshotFields(
        TestThreadListSpec(),
        [&](std::uint32_t address, SampleWidth, std::uint64_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) return false;
            out = found->second;
            return true;
        });

    EXPECT_EQ(capture_field_value(fields, "thread_list_read_ok"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_first_node"), "\"0x00000000\"");
    EXPECT_EQ(capture_field_value(fields, "thread_list_node_count"), "0");
    EXPECT_EQ(capture_field_value(fields, "thread_list_truncated"), "false");
    EXPECT_EQ(capture_field_value(fields, "thread_list_cycle_detected"), "false");
    EXPECT_EQ(capture_field_value(fields, "thread_list_nodes"), "[]");
}

TEST(LiveCheckpointLinkedListSnapshot, CapturesMultiNodeList)
{
    std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80311A84u, 0x81230000u },
        { 0x81230000u, 0x800136DCu },
        { 0x81230004u, 0x81230040u },
        { 0x81230008u, 0u },
        { 0x81230018u, 0x12u },
        { 0x81230040u, 0x80051264u },
        { 0x81230044u, 0u },
        { 0x81230048u, 0x81230000u },
        { 0x81230058u, 0x34u },
    };
    const auto fields = BuildLinkedListSnapshotFields(
        TestThreadListSpec(),
        [&](std::uint32_t address, SampleWidth, std::uint64_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) return false;
            out = found->second;
            return true;
        });

    EXPECT_EQ(capture_field_value(fields, "thread_list_read_ok"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_node_count"), "2");
    const auto nodes = capture_field_value(fields, "thread_list_nodes");
    EXPECT_NE(nodes.find("\"node\":\"0x81230000\""), std::string::npos);
    EXPECT_NE(nodes.find("\"callback\":\"0x800136DC\""), std::string::npos);
    EXPECT_NE(nodes.find("\"node\":\"0x81230040\""), std::string::npos);
    EXPECT_NE(nodes.find("\"parent\":\"0x81230000\""), std::string::npos);
}

TEST(LiveCheckpointLinkedListSnapshot, DetectsCycle)
{
    std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80311A84u, 0x81230000u },
        { 0x81230000u, 0x800136DCu },
        { 0x81230004u, 0x81230000u },
        { 0x81230008u, 0u },
        { 0x81230018u, 0u },
    };
    const auto fields = BuildLinkedListSnapshotFields(
        TestThreadListSpec(),
        [&](std::uint32_t address, SampleWidth, std::uint64_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) return false;
            out = found->second;
            return true;
        });

    EXPECT_EQ(capture_field_value(fields, "thread_list_read_ok"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_node_count"), "1");
    EXPECT_EQ(capture_field_value(fields, "thread_list_cycle_detected"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_error"), "cycle detected at 0x81230000");
}

TEST(LiveCheckpointLinkedListSnapshot, MarksMaxTruncation)
{
    std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80311A84u, 0x81230000u },
        { 0x81230000u, 0x800136DCu },
        { 0x81230004u, 0x81230040u },
        { 0x81230008u, 0u },
        { 0x81230018u, 0u },
    };
    const auto fields = BuildLinkedListSnapshotFields(
        TestThreadListSpec(1),
        [&](std::uint32_t address, SampleWidth, std::uint64_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) return false;
            out = found->second;
            return true;
        });

    EXPECT_EQ(capture_field_value(fields, "thread_list_read_ok"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_node_count"), "1");
    EXPECT_EQ(capture_field_value(fields, "thread_list_truncated"), "true");
    EXPECT_EQ(capture_field_value(fields, "thread_list_error"), "max nodes reached before null");
}

TEST(LiveCheckpointLinkedListSnapshot, RecordsPartialDataOnReadFailure)
{
    std::unordered_map<std::uint32_t, std::uint64_t> memory{
        { 0x80311A84u, 0x81230000u },
        { 0x81230004u, 0u },
        { 0x81230008u, 0u },
        { 0x81230018u, 0u },
    };
    const auto fields = BuildLinkedListSnapshotFields(
        TestThreadListSpec(),
        [&](std::uint32_t address, SampleWidth, std::uint64_t& out) {
            const auto found = memory.find(address);
            if (found == memory.end()) return false;
            out = found->second;
            return true;
        });

    EXPECT_EQ(capture_field_value(fields, "thread_list_read_ok"), "false");
    EXPECT_EQ(capture_field_value(fields, "thread_list_node_count"), "1");
    EXPECT_EQ(
        capture_field_value(fields, "thread_list_error"),
        "failed to read field 'callback' at 0x81230000");
    EXPECT_NE(
        capture_field_value(fields, "thread_list_nodes").find("\"callback_read_ok\":false"),
        std::string::npos);
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

TEST(LiveCheckpointCaptureJsonl, ParsesRawLinkedListNodeArrayField)
{
    CheckpointCaptureRecord record{};
    record.capture_sequence = 7;
    record.checkpoint_hit_count = 0;
    record.pc = 0x800136DC;
    record.checkpoint_id = "thread_runner_entry";
    record.checkpoint_name = "thread_runner_entry";
    record.function = "FUN_800136dc";
    record.checkpoint = "thread_runner_entry";
    record.movie_input_count = 1;
    record.vi_field_count = 2;
    record.frame_count = 3;
    record.tbr_u64 = 0x1111111122222222ull;
    record.tbr_high = 0x11111111u;
    record.tbr_low = 0x22222222u;
    record.fields.push_back(CaptureField{ "thread_list_read_ok", "true", false });
    record.fields.push_back(CaptureField{ "thread_list_node_count", "2", false });
    record.fields.push_back(CaptureField{
        "thread_list_nodes",
        "[{\"index\":0,\"node\":\"0x81230000\",\"callback\":\"0x800136DC\"},"
        "{\"index\":1,\"node\":\"0x81230040\",\"callback\":\"0x80051264\"}]",
        false });

    const auto line = SerializeJsonlRecord(record);
    EXPECT_NE(line.find("\"thread_list_nodes\":[{\"index\":0"), std::string::npos);

    std::istringstream input(line + "\n");
    const auto parsed = parse_checkpoint_stream(input);
    ASSERT_TRUE(parsed.errors.empty()) << (parsed.errors.empty() ? "" : parsed.errors.front());
    ASSERT_EQ(parsed.events.size(), 1u);
    EXPECT_EQ(parsed.events[0].fields.at("thread_list_read_ok"), "true");
    EXPECT_EQ(parsed.events[0].fields.at("thread_list_node_count"), "2");
    EXPECT_EQ(
        parsed.events[0].fields.at("thread_list_nodes"),
        "[{\"index\":0,\"node\":\"0x81230000\",\"callback\":\"0x800136DC\"},"
        "{\"index\":1,\"node\":\"0x81230040\",\"callback\":\"0x80051264\"}]");
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
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
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
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row00_location_code"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row00_payload_primary"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row05_location_code"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row05_payload_direct_secondary"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row12_location_code"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row17_payload_primary"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_query_call, "aux_row31_payload_direct_secondary"));

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

    for (const auto* helper_checkpoint_id : {
             "action_view_state0_actor_changed_spawn_8001318C",
             "action_view_mode0_state2_spawn_8001321C",
             "action_view_mode1_state2_call_8001329C",
             "action_view_mode2_tail_call_8001338C",
             "action_view_category3_spawn_800133E4",
             "action_view_mode5_call_800134A4",
         }) {
        const auto* helper_checkpoint = find_checkpoint(helper_checkpoint_id);
        ASSERT_NE(helper_checkpoint, nullptr) << helper_checkpoint_id;
        EXPECT_FALSE(helper_checkpoint->owns_rng_draw) << helper_checkpoint_id;
        EXPECT_TRUE(has_gpr_sample(*helper_checkpoint, "spawn_slot_arg")) << helper_checkpoint_id;
        EXPECT_TRUE(has_gpr_sample(*helper_checkpoint, "spawn_mode_arg")) << helper_checkpoint_id;
        EXPECT_TRUE(has_reg_sample(*helper_checkpoint, "actor_field6_0x6")) << helper_checkpoint_id;
        EXPECT_TRUE(has_reg_sample(*helper_checkpoint, "gate_state_0x30")) << helper_checkpoint_id;
    }

    const auto* action_view_category3_query =
        find_checkpoint("action_view_category3_query_call_800133CC");
    ASSERT_NE(action_view_category3_query, nullptr);
    EXPECT_FALSE(action_view_category3_query->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_category3_query, "query_arg0"));
    EXPECT_TRUE(has_reg_sample(*action_view_category3_query, "gate_category_0x2f"));
    EXPECT_TRUE(has_reg_sample(*action_view_category3_query, "gate_state_0x30"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category3_query, "aux_row00_location_code"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category3_query, "aux_row05_payload_primary"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category3_query, "aux_row12_payload_primary"));

    const auto* action_view_category3_result =
        find_checkpoint("action_view_category3_query_result_800133D0");
    ASSERT_NE(action_view_category3_result, nullptr);
    EXPECT_FALSE(action_view_category3_result->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_category3_result, "query_result"));

    const auto* action_view_category5_query =
        find_checkpoint("action_view_category5_query_call_80013478");
    ASSERT_NE(action_view_category5_query, nullptr);
    EXPECT_FALSE(action_view_category5_query->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_category5_query, "query_arg0"));
    EXPECT_TRUE(has_gpr_sample(*action_view_category5_query, "query_arg1"));
    EXPECT_TRUE(has_reg_sample(*action_view_category5_query, "actor_subtype_0x8"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category5_query, "aux_row00_location_code"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category5_query, "aux_row05_payload_primary"));
    EXPECT_TRUE(has_addrprog_sample(*action_view_category5_query, "aux_row17_payload_primary"));

    const auto* action_view_category5_result =
        find_checkpoint("action_view_category5_query_result_8001347C");
    ASSERT_NE(action_view_category5_result, nullptr);
    EXPECT_FALSE(action_view_category5_result->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*action_view_category5_result, "query_result"));

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

    const auto* source_selection_candidate =
        find_checkpoint("action_source_selection_candidate_80067A9C");
    ASSERT_NE(source_selection_candidate, nullptr);
    EXPECT_FALSE(source_selection_candidate->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*source_selection_candidate, "r5_selected_source_slot_candidate"));
    EXPECT_TRUE(has_reg_sample(*source_selection_candidate, "selected_source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_selection_candidate, "controller_actor_slot_0x92"));

    const auto* source_selection_fallback =
        find_checkpoint("action_source_selection_mode_gate_80067AD0");
    ASSERT_NE(source_selection_fallback, nullptr);
    EXPECT_FALSE(source_selection_fallback->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*source_selection_fallback, "r0_selected_source_slot_fallback"));
    EXPECT_TRUE(has_reg_sample(*source_selection_fallback, "selected_source_slot"));

    const auto* source_selection_global =
        find_checkpoint("action_source_selection_source_slot_80067B50");
    ASSERT_NE(source_selection_global, nullptr);
    EXPECT_FALSE(source_selection_global->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*source_selection_global, "r3_callback_thread"));
    EXPECT_TRUE(has_addrprog_sample(*source_selection_global, "selected_source_slot_global"));
    EXPECT_TRUE(has_addrprog_sample(*source_selection_global, "controller_actor_slot_0x92_global"));

    const auto* source_state_read =
        find_checkpoint("action_source_state_downstream_read_80067BD0");
    ASSERT_NE(source_state_read, nullptr);
    EXPECT_FALSE(source_state_read->owns_rng_draw);
    EXPECT_TRUE(has_reg_sample(*source_state_read, "selected_source_slot"));
    EXPECT_TRUE(has_reg_sample(*source_state_read, "controller_actor_slot_0x92"));

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
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
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
    EXPECT_NE(find_checkpoint("action_source_selection_mode_gate_80067AD0"), nullptr);
    const auto* source_slot_boundary = find_checkpoint("action_source_selection_source_slot_80067B50");
    ASSERT_NE(source_slot_boundary, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*source_slot_boundary, "selected_source_slot_global"));
    EXPECT_NE(find_checkpoint("action_source_state_downstream_read_80067BD0"), nullptr);
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

TEST(SavorPredictLiveCaptureProfile, BuildsField6WatchProfile)
{
    const auto text = build_first_battle_field6_watch_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_field6_watchpoints");
    EXPECT_EQ(profile.memory_watchpoints.size(), 4u);
    EXPECT_GE(profile.dynamic_memory_watchpoints.size(), 8u);
    for (const auto& watchpoint : profile.memory_watchpoints) {
        EXPECT_EQ(watchpoint.scope, WatchpointScope::InputMacro) << watchpoint.id;
    }
    for (const auto& watchpoint : profile.dynamic_memory_watchpoints) {
        EXPECT_EQ(watchpoint.scope, WatchpointScope::InputMacro) << watchpoint.id;
    }

    bool found_progress_pc = false;
    bool found_targeting_camera = false;
    for (const auto& checkpoint : profile.checkpoints) {
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

    const auto find_dynamic = [&](std::string_view id)
        -> const DynamicMemoryWatchpointSpec* {
        for (const auto& watchpoint : profile.dynamic_memory_watchpoints) {
            if (watchpoint.id == id) {
                return &watchpoint;
            }
        }
        return nullptr;
    };

    const auto* action_view =
        find_dynamic("actor_field6_action_view_query_call_8001331C");
    ASSERT_NE(action_view, nullptr);
    EXPECT_EQ(action_view->pc, 0x8001331Cu);
    EXPECT_EQ(action_view->base_reg, 28u);
    EXPECT_EQ(action_view->offset, 0x6);
    EXPECT_EQ(action_view->size, SampleWidth::U16);
    EXPECT_EQ(action_view->access, WatchpointAccess::Access);
    EXPECT_EQ(action_view->scope, WatchpointScope::InputMacro);

    const auto* sst_case8_source =
        find_dynamic("sst_case8_source_field6_8000C6E8");
    ASSERT_NE(sst_case8_source, nullptr);
    EXPECT_EQ(sst_case8_source->base_reg, 30u);
    EXPECT_EQ(sst_case8_source->offset, 0x0a);
}

TEST(SavorPredictLiveCaptureProfile, BuildsViewEligibilityProfile)
{
    const auto text = build_first_battle_view_eligibility_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_view_eligibility");
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_EQ(profile.memory_watchpoints[0].scope, WatchpointScope::Normal);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints.empty());

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };

    const auto* pre_call = find_checkpoint("view_eligibility_pre_call_800144E8");
    ASSERT_NE(pre_call, nullptr);
    EXPECT_TRUE(has_memory_sample(*pre_call, "candidate_resource_list_root_80347398"));
    EXPECT_TRUE(has_memory_sample(*pre_call, "slot11_combatant_root_80302ADC"));
    EXPECT_EQ(pre_call->address_program_samples.size(), 260u);
    EXPECT_TRUE(has_addrprog_sample(*pre_call, "candidate_row00_id"));
    EXPECT_TRUE(has_addrprog_sample(*pre_call, "candidate_row31_word18"));
    EXPECT_TRUE(has_addrprog_sample(*pre_call, "slot0_eligibility_flags_0x0"));
    EXPECT_TRUE(has_addrprog_sample(*pre_call, "slot11_eligibility_status_0x44"));
    ASSERT_EQ(pre_call->linked_list_samples.size(), 1u);
    const auto& list = pre_call->linked_list_samples[0];
    EXPECT_EQ(list.name, "resource_reference_list");
    EXPECT_EQ(list.head_ptr_address, 0x8034739Cu);
    EXPECT_EQ(list.next_offset, 0x10);
    EXPECT_EQ(list.max_nodes, 64u);
    ASSERT_EQ(list.fields.size(), 3u);
    EXPECT_EQ(list.fields[0].name, "id");
    EXPECT_EQ(list.fields[1].name, "active_count");
    EXPECT_EQ(list.fields[2].name, "next");

    const auto* candidate_unloaded =
        find_checkpoint("view_eligibility_candidate_unloaded_8006D250");
    ASSERT_NE(candidate_unloaded, nullptr);
    ASSERT_TRUE(candidate_unloaded->activate_on_pc.has_value());
    EXPECT_EQ(*candidate_unloaded->activate_on_pc, 0x800144E8u);
    EXPECT_TRUE(has_addrprog_sample(*candidate_unloaded, "candidate_row_id"));
    EXPECT_TRUE(has_addrprog_sample(*candidate_unloaded, "candidate_row_loaded_ptr"));

    const auto* reference_active =
        find_checkpoint("view_eligibility_reference_active_8006D248");
    ASSERT_NE(reference_active, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*reference_active, "reference_node_active_count"));

    const auto* gate_return = find_checkpoint("view_eligibility_gate_return_8006D2B0");
    ASSERT_NE(gate_return, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*gate_return, "candidate_row31_word18"));
    ASSERT_EQ(gate_return->linked_list_samples.size(), 1u);

    EXPECT_NE(find_checkpoint("view_eligibility_aggregate_nonzero_8006D270"), nullptr);
    EXPECT_NE(find_checkpoint("view_eligibility_slot_status_three_8006D294"), nullptr);
    EXPECT_NE(find_checkpoint("view_placement_rng_call_800145C8"), nullptr);

    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_NE(checkpoint.pc, 0x8000A2FCu) << checkpoint.id;
        ASSERT_TRUE(checkpoint.max_hits.has_value()) << checkpoint.id;
        EXPECT_LE(*checkpoint.max_hits, 96u) << checkpoint.id;
    }
}

TEST(SavorPredictLiveCaptureProfile, BuildsViewPlacementCacheProfile)
{
    const auto text = build_first_battle_view_placement_cache_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_view_placement_cache");
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);
    EXPECT_TRUE(profile.dynamic_memory_watchpoints.empty());

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) return &checkpoint;
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };

    const auto* key = find_checkpoint("view_placement_geometry_result_80014548");
    ASSERT_NE(key, nullptr);
    EXPECT_TRUE(has_memory_sample(*key, "view_cache_control_8030A062"));
    EXPECT_TRUE(has_memory_sample(*key, "view_cache_center_x_bits_80309F88"));
    EXPECT_TRUE(has_addrprog_sample(*key, "slot0_cw_current_x_0x1c"));
    EXPECT_TRUE(has_addrprog_sample(*key, "slot11_iw_geometry_extent_0x15c"));
    EXPECT_TRUE(has_reg_sample(*key, "view_candidate_center_x_0x38"));
    EXPECT_TRUE(has_reg_sample(*key, "view_placement_distance_0x0c"));

    const auto* decision = find_checkpoint("view_placement_cache_decision_800145BC");
    ASSERT_NE(decision, nullptr);
    EXPECT_TRUE(has_reg_sample(*decision, "view_placement_angle_0x08"));

    const auto* rng_result = find_checkpoint("view_placement_rng_angle_selected_8006093C");
    ASSERT_NE(rng_result, nullptr);
    EXPECT_TRUE(has_reg_sample(*rng_result, "selected_angle_bits"));

    const auto* rng_value = find_checkpoint("view_placement_rng_value_800608E0");
    ASSERT_NE(rng_value, nullptr);
    EXPECT_TRUE(has_reg_sample(*rng_value, "angle_before_mapping_bits"));

    EXPECT_NE(find_checkpoint("view_cache_publish_from_14474_80014704"), nullptr);
    EXPECT_NE(find_checkpoint("view_cache_publish_from_136DC_800139D8"), nullptr);
    EXPECT_NE(find_checkpoint("view_cache_publish_from_121D8_80012530"), nullptr);

    const auto* candidate_gate = find_checkpoint("view_geometry_mld_slot_return_80011544");
    ASSERT_NE(candidate_gate, nullptr);
    EXPECT_TRUE(has_reg_sample(*candidate_gate, "candidate_geometry_flags_0xec"));

    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_NE(checkpoint.pc, 0x8000A2FCu) << checkpoint.id;
        ASSERT_TRUE(checkpoint.max_hits.has_value()) << checkpoint.id;
        EXPECT_LE(*checkpoint.max_hits, 96u) << checkpoint.id;
    }
}

TEST(SavorPredictLiveCaptureProfile, BuildsViewPlacementFrameThreadProfile)
{
    const auto text = build_first_battle_view_placement_frame_thread_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_view_placement_frame_thread");
    EXPECT_TRUE(profile.default_memory_samples.empty());
    EXPECT_TRUE(profile.default_address_program_samples.empty());
    EXPECT_TRUE(profile.default_linked_list_samples.empty());
    EXPECT_TRUE(profile.dynamic_memory_watchpoints.empty());
    ASSERT_EQ(profile.memory_watchpoints.size(), 2u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);
    EXPECT_EQ(profile.memory_watchpoints[1].id, "view_cache_control_write_8030A062");
    EXPECT_EQ(profile.memory_watchpoints[1].address, 0x8030A062u);
    EXPECT_EQ(profile.memory_watchpoints[1].access, WatchpointAccess::Write);
    EXPECT_FALSE(profile.memory_watchpoints[1].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) return &checkpoint;
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_thread_list = [](const CheckpointSpec& checkpoint, std::uint32_t max_nodes) {
        if (checkpoint.linked_list_samples.size() != 1u) return false;
        const auto& list = checkpoint.linked_list_samples.front();
        return list.name == "thread_list"
            && list.head_ptr_address == 0x80311A84u
            && list.next_offset == 0x04u
            && list.max_nodes == max_nodes
            && list.fields.size() == 7u
            && list.fields[0].name == "callback"
            && list.fields[1].name == "next"
            && list.fields[2].name == "parent"
            && list.fields[3].name == "flags"
            && list.fields[4].name == "depth"
            && list.fields[5].name == "order_bits"
            && list.fields[6].name == "payload_word";
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->pc, 0x8000A2FCu);
    EXPECT_FALSE(frame->activate_on_pc.has_value());
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    EXPECT_TRUE(has_thread_list(*frame, 128));
    EXPECT_TRUE(has_memory_sample(*frame, "turn_phase_8034733c"));
    EXPECT_TRUE(has_memory_sample(*frame, "battle_input_state_80347338"));
    EXPECT_TRUE(has_memory_sample(*frame, "active_actor_slot_80347334"));
    EXPECT_TRUE(has_memory_sample(*frame, "action_sequence_80347335"));
    EXPECT_TRUE(has_memory_sample(*frame, "view_interrupt_flags_80309F10"));
    EXPECT_TRUE(has_memory_sample(*frame, "view_cache_control_8030A062"));
    EXPECT_TRUE(has_memory_sample(*frame, "view_cache_angle_bits_8030A028"));

    const std::array<std::string_view, 6> list_snapshot_ids = {
        "view_placement_cache_decision_800145BC",
        "view_placement_rng_call_800145C8",
        "view_placement_post_selection_800145CC",
        "view_cache_publish_from_14474_80014704",
        "view_cache_publish_from_136DC_800139D8",
        "view_cache_publish_from_121D8_80012530",
    };
    for (const auto id : list_snapshot_ids) {
        const auto* checkpoint = find_checkpoint(id);
        ASSERT_NE(checkpoint, nullptr) << id;
        EXPECT_TRUE(has_thread_list(*checkpoint, 128)) << id;
        EXPECT_TRUE(has_memory_sample(*checkpoint, "view_cache_distance_bits_80309FD8")) << id;
        EXPECT_TRUE(has_memory_sample(*checkpoint, "view_cache_center_z_bits_80309F90")) << id;
    }

    const auto* decision = find_checkpoint("view_placement_cache_decision_800145BC");
    ASSERT_NE(decision, nullptr);
    EXPECT_TRUE(has_gpr_sample(*decision, "cache_hit_flag_r0"));
    EXPECT_TRUE(has_reg_sample(*decision, "view_placement_angle_0x08"));
    const auto* rng_value = find_checkpoint("view_placement_rng_value_800608E0");
    ASSERT_NE(rng_value, nullptr);
    EXPECT_TRUE(has_gpr_sample(*rng_value, "rng_rand15_value"));
    const auto* selected_angle = find_checkpoint("view_placement_rng_angle_selected_8006093C");
    ASSERT_NE(selected_angle, nullptr);
    EXPECT_TRUE(has_reg_sample(*selected_angle, "selected_angle_bits"));
    const auto* runner_publish = find_checkpoint("view_cache_publish_from_136DC_800139D8");
    ASSERT_NE(runner_publish, nullptr);
    EXPECT_TRUE(has_gpr_sample(*runner_publish, "runner_view_thread_context"));

    const std::array<std::string_view, 12> required_ids = {
        "battle_case5_after_threads_8000A2FC",
        "view_placement_entry_80014498",
        "view_placement_readiness_return_800144EC",
        "view_placement_geometry_result_80014548",
        "view_placement_cache_decision_800145BC",
        "view_placement_rng_call_800145C8",
        "view_placement_rng_value_800608E0",
        "view_placement_rng_angle_selected_8006093C",
        "view_placement_post_selection_800145CC",
        "view_cache_publish_from_14474_80014704",
        "view_cache_publish_from_136DC_800139D8",
        "view_cache_publish_from_121D8_80012530",
    };
    EXPECT_EQ(profile.checkpoints.size(), required_ids.size());
    std::unordered_set<std::string> section_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(section_ids.insert(checkpoint.id).second) << checkpoint.id;
        ASSERT_TRUE(checkpoint.max_hits.has_value()) << checkpoint.id;
        if (checkpoint.id == "battle_case5_after_threads_8000A2FC") {
            EXPECT_EQ(checkpoint.pc, 0x8000A2FCu);
            EXPECT_EQ(*checkpoint.max_hits, 2400u);
        } else {
            EXPECT_NE(checkpoint.pc, 0x8000A2FCu) << checkpoint.id;
            EXPECT_LE(*checkpoint.max_hits, 512u) << checkpoint.id;
            EXPECT_TRUE(checkpoint.linked_list_samples.empty() || has_thread_list(checkpoint, 128))
                << checkpoint.id;
        }
    }

    const auto rerun_text = build_first_battle_view_placement_frame_thread_profile_ini(256);
    const auto rerun_parsed = ParseCaptureProfileText(rerun_text);
    ASSERT_TRUE(rerun_parsed.profile.has_value()) << FormatCaptureProfileError(rerun_parsed);
    const auto& rerun_profile = *rerun_parsed.profile;
    const auto* rerun_frame = [&]() -> const CheckpointSpec* {
        for (const auto& checkpoint : rerun_profile.checkpoints) {
            if (checkpoint.id == "battle_case5_after_threads_8000A2FC") return &checkpoint;
        }
        return nullptr;
    }();
    ASSERT_NE(rerun_frame, nullptr);
    EXPECT_TRUE(has_thread_list(*rerun_frame, 256));
}

TEST(SavorPredictLiveCaptureProfile, BuildsViewPlacementSemanticHooksProfile)
{
    const auto text = build_first_battle_view_placement_semantic_hooks_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_view_placement_semantic_hooks");
    EXPECT_TRUE(profile.default_memory_samples.empty());
    EXPECT_TRUE(profile.default_address_program_samples.empty());
    EXPECT_TRUE(profile.default_linked_list_samples.empty());
    EXPECT_TRUE(profile.dynamic_memory_watchpoints.empty());
    ASSERT_EQ(profile.memory_watchpoints.size(), 2u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);
    EXPECT_EQ(profile.memory_watchpoints[1].id, "view_cache_control_write_8030A062");
    EXPECT_EQ(profile.memory_watchpoints[1].address, 0x8030A062u);
    EXPECT_EQ(profile.memory_watchpoints[1].access, WatchpointAccess::Write);
    EXPECT_FALSE(profile.memory_watchpoints[1].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) return &checkpoint;
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_thread_list = [](const CheckpointSpec& checkpoint, std::uint32_t max_nodes) {
        if (checkpoint.linked_list_samples.size() != 1u) return false;
        const auto& list = checkpoint.linked_list_samples.front();
        return list.name == "thread_list"
            && list.head_ptr_address == 0x80311A84u
            && list.next_offset == 0x04u
            && list.max_nodes == max_nodes
            && list.fields.size() == 7u
            && list.fields[0].name == "callback"
            && list.fields[1].name == "next"
            && list.fields[2].name == "parent"
            && list.fields[3].name == "flags"
            && list.fields[4].name == "depth"
            && list.fields[5].name == "order_bits"
            && list.fields[6].name == "payload_word";
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    EXPECT_TRUE(has_thread_list(*frame, 128));
    EXPECT_TRUE(has_memory_sample(*frame, "turn_phase_8034733c"));
    EXPECT_TRUE(has_memory_sample(*frame, "view_cache_control_8030A062"));

    const auto* geometry_entry = find_checkpoint("view_geometry_entry_800114AC");
    ASSERT_NE(geometry_entry, nullptr);
    EXPECT_TRUE(has_gpr_sample(*geometry_entry, "geometry_use_saved_position"));
    EXPECT_TRUE(has_addrprog_sample(*geometry_entry, "slot0_cw_current_x_0x1c"));
    EXPECT_TRUE(has_addrprog_sample(*geometry_entry, "slot0_iw_saved_x_0x13c"));
    EXPECT_TRUE(has_addrprog_sample(*geometry_entry, "slot11_iw_geometry_flags_0xec"));
    EXPECT_TRUE(has_addrprog_sample(*geometry_entry, "slot11_iw_geometry_extent_0x15c"));
    EXPECT_TRUE(has_memory_sample(
        *geometry_entry,
        "view_geometry_distance_scale_bits_80348210"));
    EXPECT_TRUE(has_memory_sample(*geometry_entry, "slot0_combatant_thread_ptr"));
    EXPECT_TRUE(has_memory_sample(*geometry_entry, "slot11_combatant_thread_ptr"));

    const auto* mld_return = find_checkpoint("view_geometry_mld_slot_return_80011544");
    ASSERT_NE(mld_return, nullptr);
    EXPECT_TRUE(has_gpr_sample(*mld_return, "geometry_slot_index"));
    EXPECT_TRUE(has_gpr_sample(*mld_return, "mld_slot_return"));
    EXPECT_TRUE(has_reg_sample(*mld_return, "candidate_geometry_flags_0xec"));

    for (const auto id : {
            "placement_function_geometry_return_80012254",
            "runner_geometry_return_800138DC",
            "direct_view_geometry_return_80014508"}) {
        const auto* checkpoint = find_checkpoint(id);
        ASSERT_NE(checkpoint, nullptr) << id;
        EXPECT_TRUE(has_reg_sample(*checkpoint, "geometry_half_x_bits")) << id;
        EXPECT_TRUE(has_reg_sample(*checkpoint, "geometry_center_z_bits")) << id;
        EXPECT_TRUE(has_addrprog_sample(*checkpoint, "slot0_iw_geometry_flags_0xec")) << id;
    }

    const auto* placement_decision =
        find_checkpoint("placement_function_cache_decision_80012308");
    ASSERT_NE(placement_decision, nullptr);
    EXPECT_TRUE(has_gpr_sample(*placement_decision, "cache_hit_flag_r0"));
    EXPECT_TRUE(has_reg_sample(
        *placement_decision,
        "placement_function_distance_bits_0x0c"));
    EXPECT_TRUE(has_thread_list(*placement_decision, 128));
    EXPECT_NE(find_checkpoint("placement_function_cache_miss_call_80012314"), nullptr);
    EXPECT_NE(find_checkpoint("view_cache_publish_from_121D8_80012530"), nullptr);

    const auto* runner_draw = find_checkpoint("runner_cache_draw_80013920");
    ASSERT_NE(runner_draw, nullptr);
    EXPECT_TRUE(has_gpr_sample(*runner_draw, "runner_thread_context"));
    EXPECT_TRUE(has_reg_sample(*runner_draw, "runner_state_0x30"));
    EXPECT_TRUE(has_thread_list(*runner_draw, 128));
    const auto* runner_publish = find_checkpoint("view_cache_publish_from_136DC_800139D8");
    ASSERT_NE(runner_publish, nullptr);
    EXPECT_TRUE(has_gpr_sample(*runner_publish, "runner_thread_context"));

    EXPECT_NE(find_checkpoint("view_placement_cache_decision_800145BC"), nullptr);
    EXPECT_NE(find_checkpoint("view_placement_rng_call_800145C8"), nullptr);
    EXPECT_NE(find_checkpoint("view_placement_rng_value_800608E0"), nullptr);
    EXPECT_NE(find_checkpoint("view_placement_rng_angle_selected_8006093C"), nullptr);
    EXPECT_NE(find_checkpoint("view_placement_post_selection_800145CC"), nullptr);
    EXPECT_NE(find_checkpoint("view_cache_publish_from_14474_80014704"), nullptr);

    const auto* reset_before =
        find_checkpoint("active_record_reset_before_memset_80014B68");
    ASSERT_NE(reset_before, nullptr);
    EXPECT_TRUE(has_gpr_sample(*reset_before, "workspace_reset_destination"));
    EXPECT_TRUE(has_gpr_sample(*reset_before, "workspace_reset_size"));
    EXPECT_TRUE(has_thread_list(*reset_before, 128));
    EXPECT_NE(find_checkpoint("active_record_reset_after_memset_80014B6C"), nullptr);

    const auto* copy_before = find_checkpoint("workspace_copy_before_loop_80052AE8");
    ASSERT_NE(copy_before, nullptr);
    EXPECT_TRUE(has_gpr_sample(*copy_before, "workspace_copy_source"));
    EXPECT_TRUE(has_reg_sample(*copy_before, "workspace_copy_source_center_x_0x38"));
    EXPECT_TRUE(has_reg_sample(*copy_before, "workspace_copy_source_distance_0x88"));
    EXPECT_TRUE(has_reg_sample(*copy_before, "workspace_copy_source_angle_0xd8"));
    EXPECT_TRUE(has_reg_sample(*copy_before, "workspace_copy_source_control_0x112"));
    EXPECT_TRUE(has_thread_list(*copy_before, 128));
    EXPECT_NE(find_checkpoint("workspace_copy_after_loop_80052B04"), nullptr);

    const std::array<std::string_view, 24> required_ids = {
        "battle_case5_after_threads_8000A2FC",
        "view_geometry_entry_800114AC",
        "view_geometry_mld_slot_return_80011544",
        "placement_function_geometry_return_80012254",
        "placement_function_cache_decision_80012308",
        "placement_function_cache_miss_call_80012314",
        "view_cache_publish_from_121D8_80012530",
        "runner_geometry_return_800138DC",
        "runner_cache_draw_80013920",
        "view_cache_publish_from_136DC_800139D8",
        "view_placement_entry_80014498",
        "view_placement_readiness_return_800144EC",
        "direct_view_geometry_return_80014508",
        "view_placement_geometry_result_80014548",
        "view_placement_cache_decision_800145BC",
        "view_placement_rng_call_800145C8",
        "view_placement_rng_value_800608E0",
        "view_placement_rng_angle_selected_8006093C",
        "view_placement_post_selection_800145CC",
        "view_cache_publish_from_14474_80014704",
        "active_record_reset_before_memset_80014B68",
        "active_record_reset_after_memset_80014B6C",
        "workspace_copy_before_loop_80052AE8",
        "workspace_copy_after_loop_80052B04",
    };
    EXPECT_EQ(profile.checkpoints.size(), required_ids.size());
    std::unordered_set<std::string> section_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(section_ids.insert(checkpoint.id).second) << checkpoint.id;
        ASSERT_TRUE(checkpoint.max_hits.has_value()) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80030A4Cu) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80052238u) << checkpoint.id;
    }
    for (const auto id : required_ids) {
        EXPECT_NE(find_checkpoint(id), nullptr) << id;
    }

    const auto rerun_text =
        build_first_battle_view_placement_semantic_hooks_profile_ini(256);
    const auto rerun_parsed = ParseCaptureProfileText(rerun_text);
    ASSERT_TRUE(rerun_parsed.profile.has_value()) << FormatCaptureProfileError(rerun_parsed);
    const auto& rerun_profile = *rerun_parsed.profile;
    for (const auto& checkpoint : rerun_profile.checkpoints) {
        if (!checkpoint.linked_list_samples.empty()) {
            EXPECT_TRUE(has_thread_list(checkpoint, 256)) << checkpoint.id;
        }
    }
}

TEST(SavorPredictLiveCaptureProfile, BuildsPredictorLiveComparisonProfile)
{
    const auto text = build_first_battle_predictor_live_comparison_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_predictor_live_comparison");
    ASSERT_EQ(profile.memory_watchpoints.size(), 2u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) return &checkpoint;
        }
        return nullptr;
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    ASSERT_EQ(frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(frame->linked_list_samples[0].max_nodes, 128u);
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot0_cw_current_x_0x1c"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot11_cw_current_z_0x24"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot4_iw_move_inc_x_0x104"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot5_iw_target_z_0x118"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot0_iw_speed_0x12c"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot1_iw_action_mode_0x06"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot4_movement_worksheet_path_index_0x15"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root0_cw_owner_0x00"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root2_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root2_cw_facing_angle_0x2c"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root3_iw_action_mode_0x06"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root11_iw_move_inc_z_0x10c"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root11_iw_target_z_0x118"));

    for (const auto id : {
            "attack_hit_dodge_80010BDC",
            "attack_critical_80010C44",
            "combat_effect_spawn_position_binary_80042FBC"}) {
        const auto* checkpoint = find_checkpoint(id);
        ASSERT_NE(checkpoint, nullptr) << id;
        EXPECT_FALSE(checkpoint->owns_rng_draw) << id;
    }
    EXPECT_NE(find_checkpoint("view_placement_rng_call_800145C8"), nullptr);
    EXPECT_NE(find_checkpoint("movement_handler_promote_80080244"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_setup_after_increment_8001FC04"), nullptr);
    EXPECT_NE(find_checkpoint("movement_commit_entry_8008178C"), nullptr);

    std::unordered_set<std::string> section_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(section_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_FALSE(checkpoint.owns_rng_draw) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80030A4Cu) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80052238u) << checkpoint.id;
    }

    const auto expanded = ParseCaptureProfileText(
        build_first_battle_predictor_live_comparison_profile_ini(256));
    ASSERT_TRUE(expanded.profile.has_value()) << FormatCaptureProfileError(expanded);
    const auto* expanded_frame = [&]() -> const CheckpointSpec* {
        for (const auto& checkpoint : expanded.profile->checkpoints) {
            if (checkpoint.id == "battle_case5_after_threads_8000A2FC") return &checkpoint;
        }
        return nullptr;
    }();
    ASSERT_NE(expanded_frame, nullptr);
    ASSERT_EQ(expanded_frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(expanded_frame->linked_list_samples[0].max_nodes, 256u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsActionViewResourceProfile)
{
    const auto text = build_first_battle_action_view_resource_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_action_view_resource_materialization");

    bool found_progress_pc = false;
    bool found_targeting_camera = false;
    for (const auto& checkpoint : profile.checkpoints) {
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
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* resource_entry =
        find_checkpoint("battle_std_resource_pair_entry_80067E8C");
    ASSERT_NE(resource_entry, nullptr);
    EXPECT_TRUE(has_gpr_sample(*resource_entry, "filename_arg"));
    EXPECT_TRUE(has_reg_sample(*resource_entry, "filename_word0"));

    const auto* materializer_call =
        find_checkpoint("battle_std0_materializer_call_80067FD8");
    ASSERT_NE(materializer_call, nullptr);
    EXPECT_TRUE(has_gpr_sample(*materializer_call, "std0_filename_arg"));
    EXPECT_TRUE(has_gpr_sample(*materializer_call, "loaded_resource_root_field_ptr"));
    EXPECT_TRUE(has_reg_sample(*materializer_call, "filename_word0"));
    EXPECT_TRUE(has_addrprog_sample(*materializer_call, "root_ptr_value_before"));
    EXPECT_TRUE(has_addrprog_sample(*materializer_call, "root_rows_ptr_0x0c"));

    const auto* cache_lookup =
        find_checkpoint("std0_cache_lookup_80035D80");
    ASSERT_NE(cache_lookup, nullptr);
    EXPECT_TRUE(has_memory_sample(*cache_lookup, "std0_cache_table_ptr_slot00"));
    EXPECT_TRUE(has_memory_sample(*cache_lookup, "std0_cache_filename_key_slot11"));
    EXPECT_TRUE(has_memory_sample(*cache_lookup, "std0_transient_handoff_8030a20c"));
    EXPECT_TRUE(has_gpr_sample(*cache_lookup, "cache_key_expected"));
    EXPECT_TRUE(has_gpr_sample(*cache_lookup, "cache_slot_index"));

    const auto* cache_result =
        find_checkpoint("std0_cache_result_store_80035E0C");
    ASSERT_NE(cache_result, nullptr);
    EXPECT_TRUE(has_gpr_sample(*cache_result, "cached_table_ptr"));
    EXPECT_TRUE(has_reg_sample(*cache_result, "root_field_before_store"));

    const auto* transient =
        find_checkpoint("std0_transient_handoff_clear_80035E48");
    ASSERT_NE(transient, nullptr);
    EXPECT_TRUE(has_memory_sample(*transient, "std0_transient_handoff_8030a20c"));
    EXPECT_TRUE(has_gpr_sample(*transient, "transient_handoff_value"));

    const auto* materialized_store =
        find_checkpoint("std0_materialized_result_store_80035FA0");
    ASSERT_NE(materialized_store, nullptr);
    EXPECT_TRUE(has_gpr_sample(*materialized_store, "materialized_table_ptr"));
    EXPECT_TRUE(has_reg_sample(*materialized_store, "materialized_rows_ptr_0x0c"));

    const auto* query_call =
        find_checkpoint("action_view_category2_query_call_8001331C");
    ASSERT_NE(query_call, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*query_call, "aux_row17_payload_primary"));
}

TEST(SavorPredictLiveCaptureProfile, BuildsActionViewSelectorCoverageProfile)
{
    const auto text = build_first_battle_action_view_selector_coverage_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_action_view_selector_coverage");

    bool found_progress_pc = false;
    bool found_targeting_camera = false;
    for (const auto& checkpoint : profile.checkpoints) {
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

    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
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
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) {
                return true;
            }
        }
        return false;
    };

    const auto* classify =
        find_checkpoint("action_view_selector_field6_classify_80012FCC");
    ASSERT_NE(classify, nullptr);
    EXPECT_EQ(classify->pc, 0x80012FCCu);
    EXPECT_TRUE(has_gpr_sample(*classify, "combatant_thread"));
    EXPECT_TRUE(has_reg_sample(*classify, "actor_field6_0x6"));
    EXPECT_TRUE(has_reg_sample(*classify, "gate_category_0x2f"));

    const auto* requested =
        find_checkpoint("action_view_selector_requested_mode_ready_80013038");
    ASSERT_NE(requested, nullptr);
    EXPECT_TRUE(has_gpr_sample(*requested, "requested_mode_r4"));

    const auto* dispatch =
        find_checkpoint("action_view_selector_dispatch_800131AC");
    ASSERT_NE(dispatch, nullptr);
    EXPECT_TRUE(has_reg_sample(*dispatch, "gate_category_0x2f"));
    EXPECT_TRUE(has_reg_sample(*dispatch, "gate_state_0x30"));
    EXPECT_TRUE(has_addrprog_sample(*dispatch, "action_view_chain_loaded_resource_0x10"));

    const auto* mode1 =
        find_checkpoint("action_view_mode1_state2_call_8001329C");
    ASSERT_NE(mode1, nullptr);
    EXPECT_EQ(mode1->pc, 0x8001329Cu);
    EXPECT_FALSE(mode1->owns_rng_draw);
    EXPECT_TRUE(has_gpr_sample(*mode1, "spawn_slot_arg"));
    EXPECT_TRUE(has_gpr_sample(*mode1, "spawn_mode_arg"));
}

TEST(SavorPredictLiveCaptureProfile, BuildsThreadListProfile)
{
    const auto text = build_first_battle_thread_list_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_thread_list_ordering");
    ASSERT_EQ(profile.default_linked_list_samples.size(), 1u);
    const auto& list = profile.default_linked_list_samples[0];
    EXPECT_EQ(list.name, "thread_list");
    EXPECT_EQ(list.head_ptr_address, 0x80311A84u);
    EXPECT_EQ(list.next_offset, 0x04);
    EXPECT_EQ(list.max_nodes, 64u);
    ASSERT_EQ(list.fields.size(), 7u);
    EXPECT_EQ(list.fields[0].name, "callback");
    EXPECT_EQ(list.fields[1].name, "next");
    EXPECT_EQ(list.fields[2].name, "parent");
    EXPECT_EQ(list.fields[3].name, "flags");
    EXPECT_EQ(list.fields[4].name, "depth");
    EXPECT_EQ(list.fields[5].name, "order_bits");
    EXPECT_EQ(list.fields[6].name, "payload_word");

    bool found_progress_pc = false;
    bool found_targeting_camera = false;
    for (const auto& checkpoint : profile.checkpoints) {
        if (checkpoint.pc == 0x800608DCu) {
            found_targeting_camera = true;
        }
        if (checkpoint.id.find("progress") != std::string::npos
            || checkpoint.name.find("progress") != std::string::npos) {
            found_progress_pc = true;
        }
        ASSERT_EQ(checkpoint.linked_list_samples.size(), 1u) << checkpoint.id;
        EXPECT_EQ(checkpoint.linked_list_samples[0].name, "thread_list") << checkpoint.id;
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

    EXPECT_NE(find_checkpoint("thread_runner_entry_800136DC"), nullptr);
    ASSERT_NE(find_checkpoint("thread_runner_entry_800136DC"), nullptr);
    ASSERT_TRUE(find_checkpoint("thread_runner_entry_800136DC")->max_hits.has_value());
    EXPECT_EQ(*find_checkpoint("thread_runner_entry_800136DC")->max_hits, 1u);
    ASSERT_NE(find_checkpoint("effect_record_spawn_copy_entry_8003BA08"), nullptr);
    ASSERT_TRUE(find_checkpoint("effect_record_spawn_copy_entry_8003BA08")->max_hits.has_value());
    EXPECT_EQ(*find_checkpoint("effect_record_spawn_copy_entry_8003BA08")->max_hits, 8u);
    EXPECT_NE(find_checkpoint("setup_turn_action_entry_80082134"), nullptr);
    EXPECT_NE(find_checkpoint("action_view_update_entry_80051264"), nullptr);
    EXPECT_NE(find_checkpoint("action_view_tail_draw_gate_80051320"), nullptr);
    EXPECT_NE(find_checkpoint("action_view_pathing_tail_gate_800514B0"), nullptr);
    EXPECT_NE(find_checkpoint("combat_effect_worker_entry_80042B10"), nullptr);
    EXPECT_NE(find_checkpoint("combat_effect_rng_key_gate_80042EB8"), nullptr);
    EXPECT_NE(find_checkpoint("position_line_score_entry_8001AB60"), nullptr);
}

TEST(SavorPredictLiveCaptureProfile, BuildsPreHandlerFramePathingProfile)
{
    const auto text = build_first_battle_pre_handler_frame_pathing_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_pre_handler_frame_pathing");
    ASSERT_EQ(profile.default_linked_list_samples.size(), 1u);
    EXPECT_EQ(profile.default_linked_list_samples[0].name, "thread_list");

    const auto has_default_memory_sample = [&](std::string_view name) {
        for (const auto& sample : profile.default_memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_default_addrprog_sample = [&](std::string_view name) {
        for (const auto& sample : profile.default_address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_gpr_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.gpr_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.register_memory_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };

    EXPECT_TRUE(has_default_memory_sample("turn_phase_8034733c"));
    EXPECT_TRUE(has_default_memory_sample("pos4_x_bits"));
    EXPECT_TRUE(has_default_memory_sample("slot5_instr_param_0x6"));
    EXPECT_TRUE(has_default_addrprog_sample("slot4_movement_worksheet_pending_handler_0x10"));
    EXPECT_TRUE(has_default_addrprog_sample("slot5_movement_worksheet_cur_grid_x_0x0c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot0_movement_worksheet_status_0x16"));

    const auto* pc_store = find_checkpoint("setup_action_pc_handler_store_80070A54");
    ASSERT_NE(pc_store, nullptr);
    EXPECT_EQ(pc_store->pc, 0x80070A54u);
    EXPECT_TRUE(has_gpr_sample(*pc_store, "handler_pc"));
    EXPECT_TRUE(has_reg_sample(*pc_store, "selected_movement_worksheet_pending_handler_before_0x10"));

    const auto* enemy_store = find_checkpoint("setup_action_enemy_handler_store_80070A74");
    ASSERT_NE(enemy_store, nullptr);
    EXPECT_EQ(enemy_store->pc, 0x80070A74u);
    EXPECT_TRUE(has_gpr_sample(*enemy_store, "actor_slot"));

    const auto* promote = find_checkpoint("movement_handler_promote_80080244");
    ASSERT_NE(promote, nullptr);
    EXPECT_EQ(promote->pc, 0x80080244u);
    EXPECT_TRUE(has_gpr_sample(*promote, "pending_handler_pc"));
    EXPECT_TRUE(has_reg_sample(*promote, "movement_buffer_callback_before_0x00"));
    EXPECT_TRUE(has_reg_sample(*promote, "selected_movement_worksheet_pending_handler_0x10"));

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->pc, 0x8000A2FCu);
    ASSERT_TRUE(frame->activate_on_pc.has_value());
    EXPECT_EQ(*frame->activate_on_pc, 0x80080244u);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 1200u);

    EXPECT_NE(find_checkpoint("movement_commit_entry_8008178C"), nullptr);
    EXPECT_NE(find_checkpoint("movement_posholder_x_store_800819A0"), nullptr);
    EXPECT_NE(find_checkpoint("movement_posholder_z_store_800819B8"), nullptr);
    const auto* bridge_link = find_checkpoint("bridge_link_read_8001AD2C");
    ASSERT_NE(bridge_link, nullptr);
    EXPECT_TRUE(has_reg_sample(*bridge_link, "iw_action_mode_0x06"));
    ASSERT_TRUE(bridge_link->max_hits.has_value());
    EXPECT_EQ(*bridge_link->max_hits, 2400u);
    const auto* bridge_cw_x = find_checkpoint("bridge_cw_x_store_8001AD54");
    ASSERT_NE(bridge_cw_x, nullptr);
    ASSERT_TRUE(bridge_cw_x->max_hits.has_value());
    EXPECT_EQ(*bridge_cw_x->max_hits, 2400u);
    const auto* bridge_cw_z = find_checkpoint("bridge_cw_z_store_8001AD5C");
    ASSERT_NE(bridge_cw_z, nullptr);
    ASSERT_TRUE(bridge_cw_z->max_hits.has_value());
    EXPECT_EQ(*bridge_cw_z->max_hits, 2400u);
    const auto* bridge_snapshot = find_checkpoint("bridge_snapshot_call_8001AD68");
    ASSERT_NE(bridge_snapshot, nullptr);
    ASSERT_TRUE(bridge_snapshot->max_hits.has_value());
    EXPECT_EQ(*bridge_snapshot->max_hits, 2400u);
    EXPECT_NE(find_checkpoint("pc_handler_entry_80086C68"), nullptr);
    EXPECT_NE(find_checkpoint("enemy_handler_entry_8008B9E0"), nullptr);
}

TEST(SavorPredictLiveCaptureProfile, BuildsMovementDestinationStopProfile)
{
    const auto text = build_first_battle_movement_destination_stop_profile_ini(128);
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_movement_destination_stop");
    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos) << checkpoint.id;
    }

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_reg_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.register_memory_samples.begin(),
            checkpoint.register_memory_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.memory_samples.begin(),
            checkpoint.memory_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    ASSERT_EQ(frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(frame->linked_list_samples[0].head_ptr_address, 0x80311A84u);
    EXPECT_EQ(frame->linked_list_samples[0].max_nodes, 128u);
    EXPECT_TRUE(has_addrprog_sample(*frame, "base_grid_120"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "active_grid_120"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot0_movement_path_node10_x"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot11_movement_reachability_status_0x16"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "root0_iw_target_x_0x110"));
    EXPECT_TRUE(has_memory_sample(*frame, "movement_completion_override_80347348"));
    EXPECT_TRUE(has_memory_sample(*frame, "movement_completion_mask_80347374"));

    const auto* path_entry = find_checkpoint("movement_path_extract_entry_800823E4");
    ASSERT_NE(path_entry, nullptr);
    EXPECT_TRUE(has_reg_sample(*path_entry, "movement_path_node10_x_0x2b"));
    EXPECT_TRUE(has_reg_sample(*path_entry, "movement_path_terminator_candidate_0x2d"));
    EXPECT_TRUE(has_reg_sample(*path_entry, "movement_scratch_grid_120"));
    const auto* reachability_return =
        find_checkpoint("movement_reachability_return_80083858");
    ASSERT_NE(reachability_return, nullptr);
    EXPECT_TRUE(has_reg_sample(*reachability_return, "movement_reachability_grid_120"));
    EXPECT_NE(find_checkpoint("movement_path_extract_return_800824DC"), nullptr);
    EXPECT_NE(find_checkpoint("movement_path_index_adjust_entry_8007FE0C"), nullptr);
    EXPECT_NE(find_checkpoint("movement_path_index_adjust_return_8007FFD4"), nullptr);

    for (const auto pc : {
             "80086480", "80086698", "800871C4", "800879A8", "8008816C",
             "800883CC", "80088434", "8008C67C", "8008C844", "8008C920",
             "8008D56C"}) {
        const auto* callsite = find_checkpoint(std::string("movement_commit_callsite_") + pc);
        ASSERT_NE(callsite, nullptr) << pc;
        EXPECT_TRUE(has_reg_sample(*callsite, "movement_path_node10_x_0x2b")) << pc;
    }

    EXPECT_NE(find_checkpoint("movement_posholder_x_store_800819A0"), nullptr);
    EXPECT_NE(find_checkpoint("movement_posholder_z_store_800819B8"), nullptr);
    const auto* target = find_checkpoint("action_motion_target_return_8001FADC");
    ASSERT_NE(target, nullptr);
    EXPECT_TRUE(has_reg_sample(*target, "target_vector_x_bits"));
    const auto* setup = find_checkpoint("action_motion_setup_complete_8001FC04");
    ASSERT_NE(setup, nullptr);
    EXPECT_TRUE(has_reg_sample(*setup, "inst_move_inc_x_0x104"));
    EXPECT_NE(find_checkpoint("action_motion_clamp_x_return_8001EAB8"), nullptr);
    EXPECT_NE(find_checkpoint("action_motion_clamp_z_return_8001EB20"), nullptr);
    EXPECT_NE(find_checkpoint("action_motion_final_result_8001EB54"), nullptr);
    EXPECT_NE(find_checkpoint("action_motion_caller_consumption_8001B778"), nullptr);
    const auto* completion_entry =
        find_checkpoint("movement_completion_gate_entry_80080438");
    ASSERT_NE(completion_entry, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*completion_entry, "movement_slot_0x00"));
    EXPECT_TRUE(has_addrprog_sample(*completion_entry, "movement_coord_state_0x50"));
    EXPECT_NE(find_checkpoint("movement_completion_gate_true_return_800804AC"), nullptr);
    EXPECT_NE(find_checkpoint("movement_completion_gate_false_return_800804B4"), nullptr);
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id,
              "movement_completion_override_write_80347348");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x80347348u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsActionViewServiceLifecycleProfile)
{
    const auto text = build_first_battle_action_view_service_lifecycle_profile_ini(128);
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_action_view_service_lifecycle");
    EXPECT_TRUE(profile.default_memory_samples.empty());
    EXPECT_TRUE(profile.default_address_program_samples.empty());
    EXPECT_TRUE(profile.default_linked_list_samples.empty());
    EXPECT_TRUE(profile.dynamic_memory_watchpoints.empty());
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };
    const auto has_memory_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.memory_samples.begin(),
            checkpoint.memory_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_thread_list = [](const CheckpointSpec& checkpoint, std::uint32_t max_nodes) {
        if (checkpoint.linked_list_samples.size() != 1u) {
            return false;
        }
        const auto& list = checkpoint.linked_list_samples.front();
        return list.name == "thread_list"
            && list.head_ptr_address == 0x80311A84u
            && list.next_offset == 0x04u
            && list.max_nodes == max_nodes
            && list.fields.size() == 7u;
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->activate_on_pc.has_value());
    EXPECT_EQ(*frame->activate_on_pc, 0x80082134u);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    EXPECT_TRUE(has_thread_list(*frame, 128));
    EXPECT_TRUE(has_memory_sample(*frame, "slot0_combatant_thread_ptr"));
    EXPECT_TRUE(has_memory_sample(*frame, "slot11_combatant_thread_ptr"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot0_thread_callback_0x00"));
    EXPECT_TRUE(has_addrprog_sample(*frame, "slot11_iw_visual_timer_0x164"));

    const std::vector<std::string_view> required_ids = {
        "setup_turn_action_entry_80082134",
        "battle_case5_after_threads_8000A2FC",
        "action_view_controller_creator_entry_80014784",
        "action_view_controller_child_return_800147B0",
        "action_view_controller_publication_800147E8",
        "action_view_controller_selector_call_80013B20",
        "action_view_controller_selector_return_80013B28",
        "action_view_controller_direct_view_call_800139F8",
        "direct_view_cache_draw_call_800145C8",
        "action_view_selector_field6_classify_80012FCC",
        "action_view_category2_query_call_8001331C",
        "action_view_category2_query_result_80013320",
        "serialized_action_view_creator_entry_8003C690",
        "serialized_action_view_gate_result_8003C6C0",
        "serialized_action_view_child_return_8003C6D8",
        "serialized_action_view_publication_8003C738",
        "synthetic_action_view_creator_entry_80053F38",
        "synthetic_action_view_mode_selected_80054038",
        "synthetic_action_view_payload_ready_80054058",
        "synthetic_action_view_child_return_8005408C",
        "synthetic_action_view_publication_800540BC",
        "action_view_record_state0_helper_80051320",
        "action_view_record_mode0_draw_800513D4",
        "action_view_record_effective_mode_write_8005141C",
        "action_view_record_mode1_call_800514B0",
        "action_view_record_mode0e_call_800514C8",
        "action_view_record_cleanup_begin_80051600",
        "action_view_record_normal_completion_80051698",
        "action_view_record_state3_completion_800516BC",
        "action_service_creator_entry_8003B1D8",
        "action_service_gate_result_8003B208",
        "action_service_child_return_8003B220",
        "action_service_publication_8003B2B4",
        "action_service_init_800428D4",
        "action_service_delay_80042938",
        "action_service_split_80042958",
        "action_service_nested_call_80042990",
        "action_service_forced_path_800429AC",
        "action_service_state3_cleanup_800429D8",
        "action_service_cleanup_commit_80042A14",
        "action_service_alt_cleanup_commit_80042A90",
        "action_service_nested_entry_80020B8C",
        "action_service_nested_dispatch_80020D28",
        "action_service_resolution_entry_8002E5D0",
        "action_service_mode_path_8002E8F8",
        "action_service_eb4c_gate_8002E9B0",
        "action_service_eb4c_call_8002E9C4",
        "eb4c_entry_8002EB4C",
        "eb4c_initial_candidate_lookup_8002EBA4",
        "eb4c_fallback_draw_8002EBDC",
        "eb4c_selected_candidate_lookup_8002EC08",
        "eb4c_selection_8002EC14",
        "eb4c_publication_call_8002EC2C",
        "eb4c_return_8002EC90",
    };
    for (const auto id : required_ids) {
        EXPECT_NE(find_checkpoint(id), nullptr) << id;
    }
    for (const auto id : {
             "action_view_controller_entry_800136DC",
             "action_view_selector_entry_80012F58",
             "action_view_selector_requested_mode_ready_80013038",
             "action_view_selector_state_after_request_800130D8",
             "action_view_selector_dispatch_800131AC",
             "action_view_selector_return_800134E4",
             "action_view_record_entry_80051264",
             "action_view_record_dispatch_800512AC",
             "action_view_record_payload_dispatch_80051484",
             "action_view_record_return_80051730",
             "action_service_entry_8004281C",
             "action_service_dispatch_80042888",
             "action_service_return_80042AF4"}) {
        EXPECT_EQ(find_checkpoint(id), nullptr) << id;
    }

    const auto* controller_publication =
        find_checkpoint("action_view_controller_publication_800147E8");
    ASSERT_NE(controller_publication, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*controller_publication, "selector_state_0x30"));
    EXPECT_TRUE(has_thread_list(*controller_publication, 128));

    const auto* record_publication =
        find_checkpoint("serialized_action_view_publication_8003C738");
    ASSERT_NE(record_publication, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*record_publication, "record_origin_thread_ptr_0x74"));
    EXPECT_TRUE(has_addrprog_sample(*record_publication, "record_payload_mode_0x22"));
    EXPECT_TRUE(has_thread_list(*record_publication, 128));

    const auto* service_publication = find_checkpoint("action_service_publication_8003B2B4");
    ASSERT_NE(service_publication, nullptr);
    EXPECT_TRUE(has_addrprog_sample(*service_publication, "service_derived_mode_0x02"));
    EXPECT_TRUE(has_addrprog_sample(*service_publication, "service_command_mode_0x12"));
    EXPECT_TRUE(has_addrprog_sample(*service_publication, "service_command_subtype_0x14"));
    EXPECT_TRUE(has_addrprog_sample(*service_publication, "service_selected_slot_0x06"));
    EXPECT_TRUE(has_addrprog_sample(*service_publication, "service_origin_thread_ptr_0x08"));
    EXPECT_TRUE(
        has_addrprog_sample(*service_publication, "service_selected_target_thread_ptr_0x0c"));
    EXPECT_TRUE(has_thread_list(*service_publication, 128));

    const auto* eb4c_draw = find_checkpoint("eb4c_fallback_draw_8002EBDC");
    ASSERT_NE(eb4c_draw, nullptr);
    EXPECT_FALSE(eb4c_draw->owns_rng_draw);
    EXPECT_TRUE(has_addrprog_sample(*eb4c_draw, "eb4c_target_iw_slot_0x00"));
    EXPECT_TRUE(has_thread_list(*eb4c_draw, 128));
    const auto* mode0_draw = find_checkpoint("action_view_record_mode0_draw_800513D4");
    ASSERT_NE(mode0_draw, nullptr);
    EXPECT_FALSE(mode0_draw->owns_rng_draw);
    const auto* direct_draw = find_checkpoint("direct_view_cache_draw_call_800145C8");
    ASSERT_NE(direct_draw, nullptr);
    EXPECT_FALSE(direct_draw->owns_rng_draw);

    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80030A4Cu) << checkpoint.id;
        EXPECT_NE(checkpoint.pc, 0x80052238u) << checkpoint.id;
        ASSERT_TRUE(checkpoint.max_hits.has_value()) << checkpoint.id;
        EXPECT_LE(*checkpoint.max_hits, 2400u) << checkpoint.id;
    }

    const auto rerun_text =
        build_first_battle_action_view_service_lifecycle_profile_ini(256);
    const auto rerun_parsed = ParseCaptureProfileText(rerun_text);
    ASSERT_TRUE(rerun_parsed.profile.has_value()) << FormatCaptureProfileError(rerun_parsed);
    for (const auto& checkpoint : rerun_parsed.profile->checkpoints) {
        if (!checkpoint.linked_list_samples.empty()) {
            EXPECT_TRUE(has_thread_list(checkpoint, 256)) << checkpoint.id;
        }
    }
}

TEST(SavorPredictLiveCaptureProfile, BuildsPcWorkerSelectorLifetimeProfile)
{
    const auto text = build_first_battle_pc_worker_selector_lifetime_profile_ini(128);
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_pc_worker_selector_lifetime");
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        const auto found = std::find_if(
            profile.checkpoints.begin(),
            profile.checkpoints.end(),
            [id](const auto& checkpoint) { return checkpoint.id == id; });
        return found == profile.checkpoints.end() ? nullptr : &*found;
    };
    const auto has_addrprog = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_memory = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.memory_samples.begin(),
            checkpoint.memory_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    ASSERT_TRUE(frame->activate_on_pc.has_value());
    EXPECT_EQ(*frame->activate_on_pc, 0x80070A54u);
    ASSERT_EQ(frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(frame->linked_list_samples[0].max_nodes, 128u);
    EXPECT_EQ(frame->linked_list_samples[0].fields.size(), 8u);
    EXPECT_TRUE(std::any_of(
        frame->linked_list_samples[0].fields.begin(),
        frame->linked_list_samples[0].fields.end(),
        [](const auto& field) { return field.name == "state" && field.offset == 0x19u; }));
    EXPECT_TRUE(has_addrprog(*frame, "slot0_movement_path_node10_x"));
    EXPECT_TRUE(has_addrprog(*frame, "slot11_movement_reachability_status_0x16"));
    EXPECT_TRUE(has_memory(*frame, "slot0_queued_instr_param_0x06"));
    EXPECT_TRUE(has_memory(*frame, "slot11_movement_thread_ptr"));

    const std::vector<std::string_view> required_ids = {
        "setup_action_before_initial_relay_80070B54",
        "setup_action_after_initial_relay_80070B58",
        "setup_action_before_dispatch_relay_80070B98",
        "setup_action_after_dispatch_relay_80070B9C",
        "pc_selector_entry_800855AC",
        "pc_selector_reachability_return_80085670",
        "pc_selector_path_shape_return_80085680",
        "pc_selector_distance_loaded_8008569C",
        "pc_selector_direct_branch_800856A8",
        "pc_selector_fallback_branch_800856B0",
        "handle_pc_direct_publish_complete_80086F40",
        "handle_pc_direct_immediate_call_80086F48",
        "handle_pc_fallback_publish_complete_80086F70",
        "pc_fallback_worker_entry_80085CE0",
        "pc_direct_worker_entry_80086308",
        "pc_fallback_poll_mode7_call_80085F14",
        "pc_fallback_poll_actor_mode16_return_800860A8",
        "pc_fallback_poll_target_mode16_return_800860BC",
        "pc_fallback_poll_mode0_return_80086130",
        "pc_worker_terminal_entry_80086C48",
        "passive_initial_relay_entry_800804B8",
        "passive_relay_entry_800801A8",
        "passive_relay_promote_80080244",
        "passive_dispatch_entry_8008DEEC",
        "passive_ambient_pursuit_entry_8008C21C",
        "passive_ambient_formation_entry_8008C7B0",
        "passive_affected_target_entry_8008D3B0",
        "passive_affected_target_worker_entry_8008CDA8",
        "attack_result_return_80081BE8",
        "action_view_record_mode1_call_800514B0",
    };
    for (const auto id : required_ids) {
        const auto* checkpoint = find_checkpoint(id);
        ASSERT_NE(checkpoint, nullptr) << id;
        ASSERT_TRUE(checkpoint->max_hits.has_value()) << id;
        EXPECT_LE(*checkpoint->max_hits, 2400u) << id;
    }

    const auto* selector = find_checkpoint("pc_selector_distance_loaded_8008569C");
    ASSERT_NE(selector, nullptr);
    EXPECT_TRUE(has_addrprog(*selector, "slot0_movement_distance_0x14"));
    EXPECT_TRUE(has_addrprog(*selector, "slot0_movement_path_node0_x"));
    EXPECT_TRUE(has_addrprog(*selector, "slot11_movement_path_node10_z"));
    EXPECT_TRUE(has_addrprog(*selector, "slot11_movement_thread_state_0x19"));
    const auto* fallback = find_checkpoint("pc_fallback_worker_entry_80085CE0");
    ASSERT_NE(fallback, nullptr);
    EXPECT_TRUE(has_addrprog(*fallback, "worker_thread_state_0x19"));
    EXPECT_TRUE(has_addrprog(*fallback, "worker_iw_distance_0x14"));

    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos)
            << checkpoint.id;
    }

    const auto rerun_text =
        build_first_battle_pc_worker_selector_lifetime_profile_ini(256);
    const auto rerun_parsed = ParseCaptureProfileText(rerun_text);
    ASSERT_TRUE(rerun_parsed.profile.has_value())
        << FormatCaptureProfileError(rerun_parsed);
    const auto rerun_frame = std::find_if(
        rerun_parsed.profile->checkpoints.begin(),
        rerun_parsed.profile->checkpoints.end(),
        [](const auto& checkpoint) {
            return checkpoint.id == "battle_case5_after_threads_8000A2FC";
        });
    ASSERT_NE(rerun_frame, rerun_parsed.profile->checkpoints.end());
    ASSERT_EQ(rerun_frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(rerun_frame->linked_list_samples[0].max_nodes, 256u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsActionViewPathingLoopProfile)
{
    const auto text = build_first_battle_action_view_pathing_loop_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_action_view_pathing_loop");
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        const auto found = std::find_if(
            profile.checkpoints.begin(),
            profile.checkpoints.end(),
            [id](const auto& checkpoint) { return checkpoint.id == id; });
        return found == profile.checkpoints.end() ? nullptr : &*found;
    };
    const auto has_addrprog = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };
    const auto has_reg_memory = [](const CheckpointSpec& checkpoint, std::string_view name) {
        return std::any_of(
            checkpoint.register_memory_samples.begin(),
            checkpoint.register_memory_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };

    const auto* outer = find_checkpoint("pathing_outer_loop_entry_800526EC");
    ASSERT_NE(outer, nullptr);
    EXPECT_EQ(outer->pc, 0x800526ECu);
    EXPECT_TRUE(has_addrprog(*outer, "turn_yaw_0xd8"));
    EXPECT_TRUE(has_addrprog(*outer, "turn_path_x_0xf4"));
    EXPECT_TRUE(has_addrprog(*outer, "slot0_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_addrprog(*outer, "slot11_iw_extent_0x15c"));

    for (const auto id : {
             "pathing_actor_scan_call_800518A8",
             "pathing_target_scan_call_800518C4"}) {
        const auto* call = find_checkpoint(id);
        ASSERT_NE(call, nullptr) << id;
        ASSERT_TRUE(call->activate_on_pc.has_value()) << id;
        EXPECT_EQ(*call->activate_on_pc, 0x80082134u) << id;
        EXPECT_TRUE(has_addrprog(*call, "turn_yaw_0xd8")) << id;
        EXPECT_TRUE(has_reg_memory(*call, "outer_input_x")) << id;
        EXPECT_TRUE(has_reg_memory(*call, "outer_path_z")) << id;
    }

    const auto* candidate =
        find_checkpoint("pathing_candidate_geometry_call_80011724");
    ASSERT_NE(candidate, nullptr);
    EXPECT_TRUE(has_reg_memory(*candidate, "candidate_position_x"));
    EXPECT_TRUE(has_reg_memory(*candidate, "candidate_extent_0x15c"));
    ASSERT_TRUE(candidate->max_hits.has_value());
    EXPECT_EQ(*candidate->max_hits, 4096u);

    const auto* candidate_return =
        find_checkpoint("pathing_candidate_geometry_return_80011728");
    ASSERT_NE(candidate_return, nullptr);
    EXPECT_TRUE(has_reg_memory(*candidate_return, "scan_score_after"));
    EXPECT_NE(find_checkpoint("pathing_scan_zero_score_fallback_80011794"), nullptr);
    EXPECT_NE(find_checkpoint("pathing_scan_nonzero_return_800117C4"), nullptr);
    EXPECT_NE(find_checkpoint("pathing_outer_loop_return_800519F0"), nullptr);

    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos) << checkpoint.id;
    }
}

TEST(SavorPredictLiveCaptureProfile, BuildsThreadPathingTimingProfile)
{
    const auto text = build_first_battle_thread_pathing_timing_profile_ini(128);
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_thread_pathing_timing");
    EXPECT_EQ(profile.capture_only_hit_limit, 131072u);
    ASSERT_EQ(profile.memory_watchpoints.size(), 1u);
    EXPECT_EQ(profile.memory_watchpoints[0].id, "rng_seed_write_803469A8");
    EXPECT_EQ(profile.memory_watchpoints[0].address, 0x803469A8u);
    EXPECT_EQ(profile.memory_watchpoints[0].access, WatchpointAccess::Write);
    EXPECT_TRUE(profile.memory_watchpoints[0].owns_rng_draw);

    ASSERT_EQ(profile.dynamic_memory_watchpoints.size(), 24u);
    for (const auto& watchpoint : profile.dynamic_memory_watchpoints) {
        EXPECT_EQ(watchpoint.pc, 0x80082134u) << watchpoint.id;
        EXPECT_TRUE(watchpoint.use_address_program) << watchpoint.id;
        EXPECT_FALSE(watchpoint.address_program.empty()) << watchpoint.id;
        EXPECT_EQ(watchpoint.size, SampleWidth::U32) << watchpoint.id;
        EXPECT_EQ(watchpoint.access, WatchpointAccess::Write) << watchpoint.id;
        EXPECT_EQ(watchpoint.scope, WatchpointScope::Normal) << watchpoint.id;
        EXPECT_FALSE(watchpoint.one_shot) << watchpoint.id;
        EXPECT_FALSE(watchpoint.owns_rng_draw) << watchpoint.id;
    }

    const auto find_watchpoint = [&](std::string_view id)
        -> const DynamicMemoryWatchpointSpec* {
        for (const auto& watchpoint : profile.dynamic_memory_watchpoints) {
            if (watchpoint.id == id) return &watchpoint;
        }
        return nullptr;
    };
    EXPECT_NE(find_watchpoint("packed0_cw_cur_x_write"), nullptr);
    EXPECT_NE(find_watchpoint("packed0_cw_cur_z_write"), nullptr);
    EXPECT_NE(find_watchpoint("packed11_cw_cur_x_write"), nullptr);
    EXPECT_NE(find_watchpoint("packed11_cw_cur_z_write"), nullptr);

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        const auto found = std::find_if(
            profile.checkpoints.begin(),
            profile.checkpoints.end(),
            [id](const auto& checkpoint) { return checkpoint.id == id; });
        return found == profile.checkpoints.end() ? nullptr : &*found;
    };
    const auto has_addrprog = [](const CheckpointSpec& checkpoint,
                                 std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };

    // Write-watch rows carry the exact source PC and eight stack frames. The
    // case-5 snapshot carries full thread order, so generic per-callback
    // breakpoints would add cost without adding ownership evidence.
    EXPECT_EQ(find_checkpoint("thread_callback_before_802265BC"), nullptr);
    EXPECT_EQ(find_checkpoint("thread_callback_after_802265C0"), nullptr);

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_EQ(frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(frame->linked_list_samples[0].max_nodes, 128u);
    EXPECT_TRUE(has_addrprog(*frame, "slot0_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_addrprog(*frame, "slot0_movement_path_index_0x15"));
    EXPECT_TRUE(has_addrprog(*frame, "slot11_movement_path_node10_z"));

    for (const auto id : {
             "action_view_record_mode1_call_800514B0",
             "pathing_outer_loop_entry_800526EC",
             "pathing_actor_scan_call_800518A8",
             "pathing_target_scan_call_800518C4",
             "pathing_scan_zero_score_fallback_80011794",
             "movement_commit_entry_8008178C",
             "action_motion_target_return_8001FADC",
             "action_motion_setup_complete_8001FC04",
             "action_motion_final_result_8001EB54",
             "action_motion_caller_consumption_8001B778"}) {
        EXPECT_NE(find_checkpoint(id), nullptr) << id;
    }

    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos)
            << checkpoint.id;
    }

    const auto rerun_text =
        build_first_battle_thread_pathing_timing_profile_ini(256);
    const auto rerun_parsed = ParseCaptureProfileText(rerun_text);
    ASSERT_TRUE(rerun_parsed.profile.has_value())
        << FormatCaptureProfileError(rerun_parsed);
    const auto rerun_frame = std::find_if(
        rerun_parsed.profile->checkpoints.begin(),
        rerun_parsed.profile->checkpoints.end(),
        [](const auto& checkpoint) {
            return checkpoint.id == "battle_case5_after_threads_8000A2FC";
        });
    ASSERT_NE(rerun_frame, rerun_parsed.profile->checkpoints.end());
    ASSERT_EQ(rerun_frame->linked_list_samples.size(), 1u);
    EXPECT_EQ(rerun_frame->linked_list_samples[0].max_nodes, 256u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsBattleThreadProducerProfile)
{
    const auto text = build_battle_thread_producer_profile_ini(128);
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "battle_thread_producer");
    ASSERT_EQ(profile.memory_watchpoints.size(), 25u);
    EXPECT_EQ(profile.memory_watchpoints.front().id,
              "thread_list_head_write_80311A84");
    EXPECT_EQ(profile.memory_watchpoints.front().address, 0x80311A84u);
    for (const auto& watchpoint : profile.memory_watchpoints) {
        EXPECT_EQ(watchpoint.access, WatchpointAccess::Write) << watchpoint.id;
        EXPECT_EQ(watchpoint.scope, WatchpointScope::Normal) << watchpoint.id;
        EXPECT_FALSE(watchpoint.owns_rng_draw) << watchpoint.id;
    }

    const auto find_checkpoint = [&](std::string_view id) -> const CheckpointSpec* {
        const auto found = std::find_if(
            profile.checkpoints.begin(),
            profile.checkpoints.end(),
            [id](const auto& checkpoint) { return checkpoint.id == id; });
        return found == profile.checkpoints.end() ? nullptr : &*found;
    };
    const auto has_addrprog = [](const CheckpointSpec& checkpoint,
                                 std::string_view name) {
        return std::any_of(
            checkpoint.address_program_samples.begin(),
            checkpoint.address_program_samples.end(),
            [name](const auto& sample) { return sample.name == name; });
    };

    for (const auto id : {
             "battle_case5_after_threads_8000A2FC",
             "setup_grid_combatant_call_800849D8",
             "setup_combatant_mkchild_return_80084304",
             "load_combatant_std_pair_call_80030384",
             "load_combatant_std_pair_return_80030388",
             "create_std_mkchild_return_8001FEB8",
             "create_std_root_publication_8001FFB8",
             "mkchild_entry_802268E8",
             "thread_remove_before_unlink_80226610",
             "thread_remove_after_unlink_80226628",
             "thread_remove_after_free_8022662C"}) {
        const auto* checkpoint = find_checkpoint(id);
        ASSERT_NE(checkpoint, nullptr) << id;
        ASSERT_EQ(checkpoint->linked_list_samples.size(), 1u) << id;
        EXPECT_EQ(checkpoint->linked_list_samples.front().head_ptr_address,
                  0x80311A84u) << id;
        EXPECT_EQ(checkpoint->linked_list_samples.front().max_nodes, 128u) << id;
    }

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);
    EXPECT_TRUE(has_addrprog(*frame, "movement_root0_owner_slot"));
    EXPECT_TRUE(has_addrprog(*frame, "movement_root11_owner_slot"));
    EXPECT_TRUE(has_addrprog(*frame, "instruction_root0_owner_slot"));
    EXPECT_TRUE(has_addrprog(*frame, "instruction_root11_owner_slot"));
    const auto runner_cursor = std::find_if(
        frame->memory_samples.begin(),
        frame->memory_samples.end(),
        [](const auto& sample) {
            return sample.name == "thread_runner_current";
        });
    ASSERT_NE(runner_cursor, frame->memory_samples.end());
    EXPECT_EQ(runner_cursor->address, 0x80311A7Cu);
    EXPECT_EQ(runner_cursor->width, SampleWidth::U32);

    std::unordered_set<std::string> checkpoint_ids;
    for (const auto& checkpoint : profile.checkpoints) {
        EXPECT_TRUE(checkpoint_ids.insert(checkpoint.id).second) << checkpoint.id;
        EXPECT_EQ(checkpoint.id.find("handler_tick"), std::string::npos)
            << checkpoint.id;
    }

    const auto rerun = ParseCaptureProfileText(
        build_battle_thread_producer_profile_ini(256));
    ASSERT_TRUE(rerun.profile.has_value()) << FormatCaptureProfileError(rerun);
    const auto rerun_frame = std::find_if(
        rerun.profile->checkpoints.begin(),
        rerun.profile->checkpoints.end(),
        [](const auto& checkpoint) {
            return checkpoint.id == "battle_case5_after_threads_8000A2FC";
        });
    ASSERT_NE(rerun_frame, rerun.profile->checkpoints.end());
    EXPECT_EQ(rerun_frame->linked_list_samples.front().max_nodes, 256u);
}

TEST(SavorPredictLiveCaptureProfile, BuildsFloatMotionProfile)
{
    const auto text = build_first_battle_float_motion_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_float_motion");
    ASSERT_EQ(profile.default_linked_list_samples.size(), 1u);
    EXPECT_EQ(profile.default_linked_list_samples[0].name, "thread_list");

    const auto has_default_addrprog_sample = [&](std::string_view name) {
        for (const auto& sample : profile.default_address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
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
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto has_addrprog_sample = [](const CheckpointSpec& checkpoint, std::string_view name) {
        for (const auto& sample : checkpoint.address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };

    EXPECT_TRUE(has_default_addrprog_sample("slot4_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot0_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot0_cw_cur_z_0x24"));
    EXPECT_TRUE(has_default_addrprog_sample("slot1_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot1_cw_cur_z_0x24"));
    EXPECT_TRUE(has_default_addrprog_sample("slot5_iw_move_inc_z_0x10c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot0_iw_speed_0x12c"));
    EXPECT_TRUE(has_default_addrprog_sample("slot1_iw_alt_speed_0x130"));

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->activate_on_pc.has_value());
    EXPECT_EQ(*frame->activate_on_pc, 0x80080244u);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);

    const auto* setup_alt = find_checkpoint("float_motion_setup_alt_speed_call_8001FBEC");
    ASSERT_NE(setup_alt, nullptr);
    EXPECT_EQ(setup_alt->pc, 0x8001FBECu);
    EXPECT_TRUE(has_reg_sample(*setup_alt, "inst_alt_speed_0x130"));

    const auto* setup_base = find_checkpoint("float_motion_setup_base_speed_call_8001FC00");
    ASSERT_NE(setup_base, nullptr);
    EXPECT_EQ(setup_base->pc, 0x8001FC00u);
    EXPECT_TRUE(has_reg_sample(*setup_base, "inst_speed_0x12c"));

    const auto* worker = find_checkpoint("float_motion_worker_loaded_800506D8");
    ASSERT_NE(worker, nullptr);
    EXPECT_TRUE(has_reg_sample(*worker, "payload_primary_thread_0x04"));
    EXPECT_TRUE(has_addrprog_sample(*worker, "primary_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_addrprog_sample(*worker, "secondary_iw_flags_0xf0"));

    const auto* primary_x_before = find_checkpoint("float_motion_primary_x_store_before_80050984");
    ASSERT_NE(primary_x_before, nullptr);
    EXPECT_TRUE(has_reg_sample(*primary_x_before, "dest_cw_cur_x_0x1c"));
    EXPECT_TRUE(has_reg_sample(*primary_x_before, "delta_x_bits_stack_0x08"));

    EXPECT_NE(find_checkpoint("float_motion_primary_x_store_after_80050988"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_primary_z_store_before_8005099C"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_primary_z_store_after_800509A0"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_secondary_step_call_800509FC"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_secondary_x_store_before_80050A14"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_secondary_x_store_after_80050A18"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_secondary_z_store_before_80050A2C"), nullptr);
    EXPECT_NE(find_checkpoint("float_motion_secondary_z_store_after_80050A30"), nullptr);
    const auto* movement_commit = find_checkpoint("movement_commit_entry_8008178C");
    ASSERT_NE(movement_commit, nullptr);
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_dist_to_target_0x14"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node0_x_0x17"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node0_z_0x18"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node3_x_0x1d"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node3_z_0x1e"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node7_x_0x25"));
    EXPECT_TRUE(has_reg_sample(*movement_commit, "movement_path_node7_z_0x26"));
    EXPECT_NE(find_checkpoint("position_sync_entry_8001AB60"), nullptr);
}

TEST(SavorPredictLiveCaptureProfile, BuildsMoveIncrementReadWatchProfile)
{
    const auto text = build_first_battle_move_increment_read_watch_profile_ini();
    const auto parsed = ParseCaptureProfileText(text);
    ASSERT_TRUE(parsed.profile.has_value()) << FormatCaptureProfileError(parsed);

    const auto& profile = *parsed.profile;
    EXPECT_EQ(profile.name, "first_battle_move_increment_read_watch");
    ASSERT_EQ(profile.default_linked_list_samples.size(), 1u);
    EXPECT_EQ(profile.default_linked_list_samples[0].name, "thread_list");
    ASSERT_EQ(profile.dynamic_memory_watchpoints.size(), 3u);

    const auto find_watchpoint = [&](std::string_view id)
        -> const DynamicMemoryWatchpointSpec* {
        for (const auto& watchpoint : profile.dynamic_memory_watchpoints) {
            if (watchpoint.id == id) {
                return &watchpoint;
            }
        }
        return nullptr;
    };
    const auto expect_component = [&](std::string_view id, std::int32_t offset) {
        const auto* watchpoint = find_watchpoint(id);
        ASSERT_NE(watchpoint, nullptr) << id;
        EXPECT_EQ(watchpoint->pc, 0x8001FC04u);
        EXPECT_EQ(watchpoint->base_reg, 31u);
        EXPECT_EQ(watchpoint->offset, offset);
        EXPECT_EQ(watchpoint->size, SampleWidth::U32);
        EXPECT_EQ(watchpoint->access, WatchpointAccess::Read);
        EXPECT_EQ(watchpoint->scope, WatchpointScope::Normal);
        EXPECT_FALSE(watchpoint->one_shot);
        EXPECT_FALSE(watchpoint->owns_rng_draw);
    };
    expect_component("move_increment_x_read_after_8001FC04", 0x104);
    expect_component("move_increment_y_read_after_8001FC04", 0x108);
    expect_component("move_increment_z_read_after_8001FC04", 0x10c);

    const auto has_default_addrprog_sample = [&](std::string_view name) {
        for (const auto& sample : profile.default_address_program_samples) {
            if (sample.name == name) return true;
        }
        return false;
    };
    const auto find_checkpoint = [&](std::string_view id)
        -> const CheckpointSpec* {
        for (const auto& checkpoint : profile.checkpoints) {
            if (checkpoint.id == id) {
                return &checkpoint;
            }
        }
        return nullptr;
    };

    EXPECT_TRUE(has_default_addrprog_sample("slot4_iw_move_inc_x_0x104"));
    EXPECT_TRUE(has_default_addrprog_sample("slot5_iw_move_inc_z_0x10c"));

    const auto* frame = find_checkpoint("battle_case5_after_threads_8000A2FC");
    ASSERT_NE(frame, nullptr);
    ASSERT_TRUE(frame->activate_on_pc.has_value());
    EXPECT_EQ(*frame->activate_on_pc, 0x8001FC04u);
    ASSERT_TRUE(frame->max_hits.has_value());
    EXPECT_EQ(*frame->max_hits, 2400u);

    EXPECT_NE(find_checkpoint("float_motion_setup_after_increment_8001FC04"), nullptr);
    EXPECT_NE(find_checkpoint("movement_commit_entry_8008178C"), nullptr);
    EXPECT_NE(find_checkpoint("position_sync_entry_8001AB60"), nullptr);
}

TEST(Field6WatchpointModel, ClassifiesAccessWatchpointsByDecodedInstruction)
{
    CheckpointEvent read{};
    read.function = "memory_watchpoint";
    read.checkpoint = "access";
    read.pc = "80013320";
    read.rng_draw_index_before = 12;
    read.fields["capture_sequence"] = "3";
    read.fields["checkpoint_id"] = "memwatch.actor_field6";
    read.fields["memwatch_label"] = "actor_field6";
    read.fields["memwatch_addr"] = "0x81234506";
    read.fields["memwatch_access"] = "access";
    read.fields["memwatch_confirmed_current_instruction"] = "true";
    read.fields["decoded_access"] = "read";
    read.fields["decoded_mnemonic"] = "lhz";
    read.fields["decoded_memory_value"] = "0x0000000000000001";

    CheckpointEvent write{};
    write.function = "memory_watchpoint";
    write.checkpoint = "access";
    write.pc = "800856C4";
    write.rng_draw_index_before = 13;
    write.fields["capture_sequence"] = "4";
    write.fields["checkpoint_id"] = "memwatch.actor_field6";
    write.fields["memwatch_label"] = "actor_field6";
    write.fields["memwatch_addr"] = "0x81234506";
    write.fields["memwatch_access"] = "access";
    write.fields["memwatch_confirmed_current_instruction"] = "true";
    write.fields["decoded_access"] = "write";
    write.fields["decoded_mnemonic"] = "sth";
    write.fields["decoded_value"] = "0x0000000000000000";

    const auto summary = summarize_field6_watchpoints({ read, write });
    EXPECT_EQ(summary.observed_events, 2);
    EXPECT_EQ(summary.confirmed_events, 2);
    EXPECT_EQ(summary.confirmed_reads, 1);
    EXPECT_EQ(summary.confirmed_writes, 1);
    EXPECT_EQ(summary.producer_not_seen_reads, 1);
    ASSERT_EQ(summary.events.size(), 2u);
    EXPECT_TRUE(summary.events[0].producer_not_seen);
    EXPECT_FALSE(summary.events[1].producer_not_seen);
    EXPECT_STREQ(field6_watchpoint_status(summary), "validated");
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
    uint32_t battle_run_ms = 0;
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_ENABLED, override_enabled));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RNG_OVERRIDE_SEED, override_seed));
    ASSERT_TRUE(ctx.get(savor::context::key::battle::RUN_LONG_TIMEOUT_MS, battle_run_ms));
    EXPECT_EQ(override_enabled, 1u);
    EXPECT_EQ(override_seed, 0x12345678u);
    EXPECT_EQ(battle_run_ms, spec.run_ms);
}

} // namespace
