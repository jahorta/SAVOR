#include "LiveCaptureProfile.h"

#include "CheckpointTrace.h"

#include "Core/Memory/Soa/SoaAddrRegistry.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace savor::predict {
namespace {

std::string hex_u32(std::uint32_t value)
{
    std::ostringstream out;
    out << "0x" << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}

std::string section_id(std::string owner, const std::string& pc)
{
    for (auto& ch : owner) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
            ch = '_';
        }
    }
    return owner + "_" + pc;
}

void write_checkpoint(
    std::ostringstream& out,
    std::string_view id,
    std::string_view pc,
    std::string_view name,
    std::string_view function,
    std::string_view checkpoint,
    bool owns_rng_draw,
    const std::vector<std::string_view>& memory = {},
    const std::vector<std::string_view>& gprs = {},
    const std::vector<std::string_view>& reg_memory = {})
{
    out << "[checkpoint." << id << "]\n";
    out << "pc=0x" << pc << "\n";
    out << "name=" << name << "\n";
    out << "function=" << function << "\n";
    out << "checkpoint=" << checkpoint << "\n";
    out << "owns_rng_draw=" << (owns_rng_draw ? "true" : "false") << "\n";
    if (!memory.empty()) {
        out << "memory=";
        for (std::size_t i = 0; i < memory.size(); ++i) {
            if (i != 0) out << ",";
            out << memory[i];
        }
        out << "\n";
    }
    if (!gprs.empty()) {
        out << "gprs=";
        for (std::size_t i = 0; i < gprs.size(); ++i) {
            if (i != 0) out << ",";
            out << gprs[i];
        }
        out << "\n";
    }
    if (!reg_memory.empty()) {
        out << "reg_memory=";
        for (std::size_t i = 0; i < reg_memory.size(); ++i) {
            if (i != 0) out << ",";
            out << reg_memory[i];
        }
        out << "\n";
    }
    out << "\n";
}

std::vector<std::string_view> action_view_globals()
{
    return {
        "global_camera_override_80347394:0x80347394:u32",
        "global_camera_flags_803472F4:0x803472F4:u32",
    };
}

std::vector<std::string_view> action_view_update_gprs()
{
    return {
        "r3_payload_or_return:3",
        "r29_thread:29",
        "r30_instruction:30",
        "r31_worksheet:31",
    };
}

std::vector<std::string_view> action_view_payload_from_r3_samples()
{
    return {
        "payload_primary_0x00:r3:0x00:u16",
        "payload_secondary_0x02:r3:0x02:u16",
        "payload_variant_0x04:r3:0x04:u16",
        "payload_low_flags_0x06:r3:0x06:u16",
        "payload_flags_0x10:r3:0x10:u32",
        "payload_scalar_0x14:r3:0x14:u32",
        "payload_start_frame_0x18:r3:0x18:u16",
        "payload_end_frame_0x1c:r3:0x1c:u16",
        "payload_hold_0x1e:r3:0x1e:u16",
        "payload_step_0x20:r3:0x20:u16",
        "payload_mode_0x22:r3:0x22:u16",
    };
}

std::vector<std::string_view> action_view_worksheet_from_r31_samples()
{
    return {
        "thread_state_0x19:r29:0x19:u8",
        "instruction_flags_0xf0:r30:0xf0:u32",
        "worksheet_flags_0x68:r31:0x68:u32",
        "worksheet_turn_timer_0x70:r31:0x70:u16",
        "worksheet_state_0x10e:r31:0x10e:u16",
        "worksheet_saved_mode_0x110:r31:0x110:u16",
        "worksheet_effective_mode_0x112:r31:0x112:u16",
        "worksheet_payload_ptr_0x178:r31:0x178:u32",
    };
}

std::vector<std::string_view> action_view_mode0e_rng_samples()
{
    return {
        "worksheet_payload_ptr_0x178:r30:0x178:u32",
        "worksheet_turn_timer_0x70:r30:0x70:u16",
        "worksheet_state_0x10e:r30:0x10e:u16",
        "worksheet_saved_mode_0x110:r30:0x110:u16",
        "worksheet_effective_mode_0x112:r30:0x112:u16",
        "mode0e_current_slot_0x174:r30:0x174:u8",
        "mode0e_target_slot_0x175:r30:0x175:u8",
    };
}

std::vector<std::string_view> concat(
    std::vector<std::string_view> lhs,
    const std::vector<std::string_view>& rhs)
{
    lhs.insert(lhs.end(), rhs.begin(), rhs.end());
    return lhs;
}

} // namespace

std::string build_first_battle_capture_profile_ini()
{
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_live_capture\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    for (const auto& [pc, owner] : known_rng_callsite_owners()) {
        if (pc == "800513D4") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "UpdateActionViewRecord",
                owner,
                true,
                action_view_globals(),
                action_view_update_gprs(),
                concat(action_view_payload_from_r3_samples(), action_view_worksheet_from_r31_samples()));
        } else if (pc == "80052BF0") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "FUN_80052b24",
                owner,
                true,
                {},
                {"r30_worksheet:30", "r31_target_buffer:31"},
                action_view_mode0e_rng_samples());
        } else {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "RNG",
                owner,
                true);
        }
    }

    write_checkpoint(
        out,
        "action_view_dispatch_state_80051424",
        "80051424",
        "action_view_dispatch_state",
        "UpdateActionViewRecord",
        "action_view_dispatch_state",
        false,
        action_view_globals(),
        action_view_update_gprs(),
        concat(action_view_payload_from_r3_samples(), action_view_worksheet_from_r31_samples()));

    write_checkpoint(
        out,
        "mode0_action_view_handler_entry_80052ECC",
        "80052ECC",
        "mode0_action_view_handler_entry",
        "FUN_80052ecc",
        "mode0_action_view_handler_entry",
        false,
        {},
        {"r3_worksheet:3"});

    write_checkpoint(
        out,
        "mode0e_action_view_handler_start_gate_80052BC4",
        "80052BC4",
        "mode0e_action_view_handler_start_gate",
        "FUN_80052b24",
        "mode0e_action_view_handler_start_gate",
        false,
        {},
        {"r3_payload:3", "r30_worksheet:30", "r31_target_buffer:31"},
        concat(
            action_view_payload_from_r3_samples(),
            {
                "worksheet_turn_timer_0x70:r30:0x70:u16",
                "worksheet_state_0x10e:r30:0x10e:u16",
                "worksheet_payload_ptr_0x178:r30:0x178:u32",
            }));

    return out.str();
}

int write_first_battle_capture_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-capture-profile requires --output PATH.\n";
        return 2;
    }
    if (const auto parent = output_path.parent_path(); !parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            err << "Failed to create output directory: " << ec.message() << "\n";
            return 1;
        }
    }

    std::ofstream file(output_path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        err << "Failed to open output profile: " << output_path.string() << "\n";
        return 1;
    }
    const auto text = build_first_battle_capture_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle capture profile: " << output_path.string() << "\n";
    return 0;
}

} // namespace savor::predict
