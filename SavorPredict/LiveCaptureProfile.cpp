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

bool is_combat_effect_burst_pc(std::string_view pc)
{
    return pc == "80042F3C"
        || pc == "80042FBC"
        || pc == "80043020"
        || pc == "80043048"
        || pc == "80043070"
        || pc == "800430FC"
        || pc == "80043200";
}

bool is_effect_emitter_spawn_pc(std::string_view pc)
{
    return pc == "80041F3C"
        || pc == "80041F60"
        || pc == "80041F88"
        || pc == "80041FB0"
        || pc == "80041FCC"
        || pc == "80041FE8"
        || pc == "80042020";
}

bool is_effect_particle_tick_pc(std::string_view pc)
{
    return pc == "800425A0"
        || pc == "800425E0"
        || pc == "80042630"
        || pc == "80042670";
}

bool is_excluded_from_default_live_profile(std::string_view pc)
{
    // This targeting-camera rand fires while the battle input macro is still
    // selecting targets. Capturing it by default currently perturbs the macro
    // path enough to miss the finalized-target breakpoint, so keep it for
    // explicit profiles only until that stepping interaction is fixed.
    return pc == "800608DC";
}

std::vector<std::string_view> combat_effect_burst_samples()
{
    return {
        "effect_flags_0x38:r29:0x38:u32",
        "effect_loop_count_0x5c:r29:0x5c:u16",
        "effect_variant_count_0x5e:r29:0x5e:u16",
        "effect_axis_mode_0x60:r29:0x60:u16",
        "effect_axis_value_raw_0x68:r29:0x68:u32",
        "effect_scale_x_raw_0x70:r29:0x70:u32",
        "effect_scale_y_raw_0x74:r29:0x74:u32",
        "effect_scale_z_raw_0x78:r29:0x78:u32",
        "child_effect_flags_0x44:r27:0x44:u32",
        "child_effect_variant_0x02:r27:0x02:u16",
    };
}

std::vector<std::string_view> effect_emitter_spawn_samples()
{
    return {
        "emitter_source_ptr_0x8c:r26:0x8c:u32",
        "emitter_buffer_frame_0x00:r26:0x00:u16",
        "emitter_buffer_action_ptr_0x04:r26:0x04:u32",
    };
}

std::vector<std::string_view> effect_emitter_source_record_samples()
{
    return {
        "emitter_source_start_0x14:r4:0x14:u16",
        "emitter_variant_count_0x16:r4:0x16:u16",
        "emitter_child_count_0x1c:r4:0x1c:u16",
        "emitter_child_stride_0x1e:r4:0x1e:u16",
        "emitter_axis_mode_0x3c:r4:0x3c:u16",
        "emitter_outer_count_0x142:r4:0x142:u16",
    };
}

std::vector<std::string_view> effect_particle_tick_samples()
{
    return {
        "particle_payload_frame_0x00:r30:0x00:u16",
        "particle_payload_lifetime_0x28:r30:0x28:u16",
        "particle_payload_source_ptr_0x20:r30:0x20:u32",
        "particle_state_x_raw_0x1c:r31:0x1c:u32",
        "particle_state_z_raw_0x24:r31:0x24:u32",
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
        if (is_excluded_from_default_live_profile(pc)) {
            continue;
        }
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
        } else if (is_combat_effect_burst_pc(pc)) {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "FUN_80042b10",
                owner,
                true,
                {},
                {
                    "r27_child_effect_buffer:27",
                    "r28_loop_index:28",
                    "r29_effect_buffer:29",
                    "r30_combatant_worksheet:30",
                },
                combat_effect_burst_samples());
        } else if (is_effect_emitter_spawn_pc(pc)) {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "FUN_80041e64",
                owner,
                true,
                {},
                {
                    "r25_outer_index:25",
                    "r26_emitter_buffer:26",
                    "r4_source_record_candidate:4",
                },
                effect_emitter_spawn_samples());
        } else if (is_effect_particle_tick_pc(pc)) {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "FUN_800422d0",
                owner,
                true,
                {},
                {
                    "r30_particle_payload:30",
                    "r31_particle_state:31",
                },
                effect_particle_tick_samples());
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

    // action_view_dispatch_state_80051424 is intentionally omitted from the
    // default profile. It is useful in narrow action-view research profiles,
    // but it fires once per update tick and can exhaust the VM run timeout.

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

    write_checkpoint(
        out,
        "effect_emitter_source_gate_80041F30",
        "80041F30",
        "effect_emitter_source_gate",
        "FUN_80041e64",
        "effect_emitter_source_gate",
        false,
        {},
        {"r25_outer_index:25", "r26_emitter_buffer:26", "r4_source_record:4"},
        effect_emitter_source_record_samples());

    write_checkpoint(
        out,
        "combat_effect_loop_gate_80043278",
        "80043278",
        "combat_effect_loop_gate",
        "FUN_80042b10",
        "combat_effect_loop_gate",
        false,
        {},
        {
            "r28_loop_index:28",
            "r29_effect_buffer:29",
        },
        combat_effect_burst_samples());

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
