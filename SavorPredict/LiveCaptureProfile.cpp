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
        "effect_source_key_0x28:r29:0x28:u16",
        "effect_source_subtype_0x2a:r29:0x2a:u16",
        "effect_source_secondary_0x2c:r29:0x2c:u16",
        "effect_source_resource_id_0x30:r29:0x30:u32",
        "effect_flags_0x38:r29:0x38:u32",
        "effect_source_timing_raw_0x3c:r29:0x3c:u32",
        "effect_source_pos_x_raw_0x50:r29:0x50:u32",
        "effect_source_pos_y_raw_0x54:r29:0x54:u32",
        "effect_source_pos_z_raw_0x58:r29:0x58:u32",
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

std::vector<std::string_view> effect_record_copy_samples_from_r6()
{
    return {
        "effect_parent_action_thread_0x04:r6:0x04:u32",
        "effect_source_key_0x28:r6:0x28:u16",
        "effect_source_subtype_0x2a:r6:0x2a:u16",
        "effect_source_secondary_0x2c:r6:0x2c:u16",
        "effect_source_resource_id_0x30:r6:0x30:u32",
        "effect_flags_0x38:r6:0x38:u32",
        "effect_source_timing_raw_0x3c:r6:0x3c:u32",
        "effect_loop_count_0x5c:r6:0x5c:u16",
        "effect_variant_count_0x5e:r6:0x5e:u16",
        "effect_axis_mode_0x60:r6:0x60:u16",
        "effect_scale_x_raw_0x70:r6:0x70:u32",
        "effect_scale_y_raw_0x74:r6:0x74:u32",
        "effect_scale_z_raw_0x78:r6:0x78:u32",
    };
}

std::vector<std::string_view> effect_source_record_samples_from_r31()
{
    return {
        "source_record_key_0x00:r31:0x00:u16",
        "source_record_subtype_0x02:r31:0x02:u16",
        "source_record_secondary_0x04:r31:0x04:u16",
        "source_record_resource_id_0x08:r31:0x08:u32",
        "source_record_flags_0x10:r31:0x10:u32",
        "source_record_timing_raw_0x14:r31:0x14:u32",
        "source_record_loop_count_0x34:r31:0x34:u16",
        "source_record_variant_count_0x36:r31:0x36:u16",
        "source_record_axis_mode_0x38:r31:0x38:u16",
        "source_record_scale_x_raw_0x48:r31:0x48:u32",
        "source_record_scale_y_raw_0x4c:r31:0x4c:u32",
        "source_record_scale_z_raw_0x50:r31:0x50:u32",
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

std::vector<std::string_view> attack_resolution_begin_gprs()
{
    return {
        "target_slot_arg:3",
        "actor_slot_arg:4",
    };
}

std::vector<std::string_view> action_source_selection_samples()
{
    return {
        "selected_source_slot:r3:0x90:u16",
        "controller_actor_slot_0x92:r3:0x92:u16",
    };
}

std::vector<std::string_view> action_source_bridge_samples()
{
    return {
        "actor_slot:r30:0x00:u8",
        "target_slot:r30:0x04:u8",
        "source_slot:r29:0x00:u8",
        "source_target_slot_0x4:r29:0x04:u8",
        "actor_field6_0x6:r30:0x06:u16",
        "actor_subtype_0x8:r30:0x08:u16",
        "actor_previous_field6_0x1c:r30:0x1c:u16",
        "source_field6_0x6:r29:0x06:u16",
        "source_subtype_0x8:r29:0x08:u16",
        "selected_field6_stack_0x0a:r1:0x0a:u16",
        "selected_subtype_stack_0x08:r1:0x08:u16",
        "selected_row_index_0xe4:r30:0xe4:u16",
        "handler_pc:r30:0xe0:u32",
        "callback_0xe0:r30:0xe0:u32",
    };
}

std::vector<std::string_view> sst_case2_action_field6_store_samples()
{
    return {
        "source_field4_0x04:r29:0x04:u16",
        "source_field6_0x06:r29:0x06:u16",
        "dest_field4_after_0x04:r3:0x04:u16",
        "dest_field6_after_0x06:r3:0x06:u16",
        "dest_subtype_after_0x08:r3:0x08:u16",
    };
}

std::vector<std::string_view> sst_case8_action_field6_store_samples()
{
    return {
        "source_field0_0x00:r30:0x00:u16",
        "source_field2_0x06:r30:0x06:u16",
        "source_field4_0x08:r30:0x08:u16",
        "source_field6_0x0a:r30:0x0a:u16",
        "source_subtype_0x0c:r30:0x0c:u16",
        "dest_field0_after_0x00:r3:0x00:u16",
        "dest_field2_after_0x02:r3:0x02:u16",
        "dest_field4_after_0x04:r3:0x04:u16",
        "dest_field6_after_0x06:r3:0x06:u16",
    };
}

std::vector<std::string_view> action_view_category2_gate_gprs()
{
    return {
        "aux_list_root:3",
        "query_arg0:4",
        "query_arg1:5",
        "query_arg2:6",
        "query_arg3:7",
        "r28_instruction_worksheet:28",
        "r29_action_view_state:29",
        "r30_action_view_payload:30",
        "r31_active_combatant:31",
    };
}

std::vector<std::string_view> action_view_category2_result_gprs()
{
    return {
        "query_result:3",
        "r28_instruction_worksheet:28",
        "r29_action_view_state:29",
        "r30_action_view_payload:30",
        "r31_active_combatant:31",
    };
}

std::vector<std::string_view> action_view_category2_spawn_gprs()
{
    return {
        "spawn_slot_arg:3",
        "spawn_mode_arg:4",
        "r28_instruction_worksheet:28",
        "r29_action_view_state:29",
        "r30_action_view_payload:30",
        "r31_active_combatant:31",
    };
}

std::vector<std::string_view> action_view_category2_gate_samples()
{
    return {
        "active_slot:r1:0x0a:u16",
        "target_slot:r1:0x08:u16",
        "actor_field6_0x6:r28:0x06:u16",
        "actor_subtype_0x8:r28:0x08:u16",
        "instruction_flags_0xf0:r28:0xf0:u32",
        "gate_category_0x2f:r29:0x2f:u8",
        "gate_state_0x30:r29:0x30:u16",
        "gate_active_slot_0x02:r29:0x02:u16",
        "gate_target_slot_0x04:r29:0x04:u16",
        "combatant_movement_worksheet_0x24:r31:0x24:u32",
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
        "attack_resolution_begin_80081B94",
        "80081B94",
        "attack_resolution_begin",
        "Battle::AtkMethods::performAttack_80081b94",
        "attack_begin",
        false,
        {},
        attack_resolution_begin_gprs());

    write_checkpoint(
        out,
        "action_source_selection_80067BD0",
        "80067BD0",
        "action_source_selection",
        "FUN_8006782c",
        "source_selection",
        false,
        action_view_globals(),
        {"r3_action_source_state:3"},
        action_source_selection_samples());

    write_checkpoint(
        out,
        "action_source_field6_bridge_8006778C",
        "8006778C",
        "action_source_field6_bridge",
        "FUN_8006721c",
        "action_source",
        false,
        {},
        {
            "selected_row_index_return:3",
            "r29_source_instruction:29",
            "r30_actor_instruction:30",
        },
        action_source_bridge_samples());

    write_checkpoint(
        out,
        "sst_action_field6_case2_store_complete_8000C4C8",
        "8000C4C8",
        "sst_action_field6_case2_store_complete",
        "SST::Command::Dispatch_8000c19c",
        "sst_action_field6_case2_store_complete",
        false,
        {},
        {
            "r0_written_field6:0",
            "r3_destination_worksheet:3",
            "r27_parent_thread:27",
            "r28_child_thread:28",
            "r29_serialized_command:29",
            "r31_dispatch_context:31",
        },
        sst_case2_action_field6_store_samples());

    write_checkpoint(
        out,
        "sst_action_field6_case8_store_complete_8000C6E8",
        "8000C6E8",
        "sst_action_field6_case8_store_complete",
        "SST::Command::Dispatch_8000c19c",
        "sst_action_field6_case8_store_complete",
        false,
        {},
        {
            "r0_written_field6:0",
            "r3_destination_worksheet:3",
            "r27_parent_thread:27",
            "r29_child_thread:29",
            "r30_serialized_command:30",
            "r31_dispatch_context:31",
        },
        sst_case8_action_field6_store_samples());

    write_checkpoint(
        out,
        "action_view_category2_query_call_8001331C",
        "8001331C",
        "action_view_category2_query_call",
        "FUN_80012f58",
        "action_view_query",
        false,
        action_view_globals(),
        action_view_category2_gate_gprs(),
        action_view_category2_gate_samples());

    write_checkpoint(
        out,
        "action_view_category2_query_result_80013320",
        "80013320",
        "action_view_category2_query_result",
        "FUN_80012f58",
        "action_view_query_result",
        false,
        action_view_globals(),
        action_view_category2_result_gprs(),
        action_view_category2_gate_samples());

    write_checkpoint(
        out,
        "action_view_category2_spawn_80013334",
        "80013334",
        "action_view_category2_spawn",
        "FUN_80053f38",
        "action_view_spawn",
        false,
        action_view_globals(),
        action_view_category2_spawn_gprs(),
        action_view_category2_gate_samples());

    write_checkpoint(
        out,
        "effect_record_copy_complete_8003BB24",
        "8003BB24",
        "effect_record_copy_complete",
        "FUN_8003ba08",
        "effect_record_copy_complete",
        false,
        {},
        {
            "r6_effect_buffer:6",
            "r30_parent_action_thread:30",
            "r31_source_record:31",
        },
        concat(effect_record_copy_samples_from_r6(), effect_source_record_samples_from_r31()));

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
