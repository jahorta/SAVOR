#include "LiveCaptureProfile.h"

#include "CheckpointTrace.h"

#include "Core/Memory/Soa/SoaAddrRegistry.h"

#include <filesystem>
#include <fstream>
#include <cctype>
#include <array>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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
    const std::vector<std::string_view>& reg_memory = {},
    const std::vector<std::string_view>& addrprog = {},
    const std::vector<std::string_view>& linked_list = {},
    std::uint32_t max_hits = 0,
    std::uint32_t activate_on_pc = 0)
{
    out << "[checkpoint." << id << "]\n";
    out << "pc=0x" << pc << "\n";
    if (activate_on_pc != 0) {
        out << "activate_on_pc=" << hex_u32(activate_on_pc) << "\n";
    }
    out << "name=" << name << "\n";
    out << "function=" << function << "\n";
    out << "checkpoint=" << checkpoint << "\n";
    out << "owns_rng_draw=" << (owns_rng_draw ? "true" : "false") << "\n";
    if (max_hits != 0) {
        out << "max_hits=" << max_hits << "\n";
    }
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
    if (!addrprog.empty()) {
        out << "addrprog=";
        for (std::size_t i = 0; i < addrprog.size(); ++i) {
            if (i != 0) out << ",";
            out << addrprog[i];
        }
        out << "\n";
    }
    if (!linked_list.empty()) {
        out << "linked_list=";
        for (std::size_t i = 0; i < linked_list.size(); ++i) {
            if (i != 0) out << ";";
            out << linked_list[i];
        }
        out << "\n";
    }
    out << "\n";
}

void write_static_watchpoint(
    std::ostringstream& out,
    std::string_view id,
    std::uint32_t address,
    std::string_view size,
    std::string_view access,
    std::string_view scope = {})
{
    out << "[watchpoint." << id << "]\n";
    out << "address=" << hex_u32(address) << "\n";
    out << "size=" << size << "\n";
    out << "access=" << access << "\n";
    if (!scope.empty()) {
        out << "scope=" << scope << "\n\n";
    } else {
        out << "\n";
    }
}

void write_dynamic_watchpoint(
    std::ostringstream& out,
    std::string_view id,
    std::string_view pc,
    std::string_view base_gpr,
    std::string_view offset,
    std::string_view size,
    std::string_view access,
    std::string_view scope = {})
{
    out << "[dynamic_watchpoint." << id << "]\n";
    out << "pc=0x" << pc << "\n";
    out << "base_gpr=" << base_gpr << "\n";
    out << "offset=" << offset << "\n";
    out << "size=" << size << "\n";
    out << "access=" << access << "\n";
    if (!scope.empty()) {
        out << "scope=" << scope << "\n\n";
    } else {
        out << "\n";
    }
}

void write_checkpoint_owned(
    std::ostringstream& out,
    std::string_view id,
    std::string_view pc,
    std::string_view name,
    std::string_view function,
    std::string_view checkpoint,
    bool owns_rng_draw,
    const std::vector<std::string>& memory,
    const std::vector<std::string_view>& gprs = {})
{
    std::vector<std::string_view> memory_views;
    memory_views.reserve(memory.size());
    for (const auto& sample : memory) {
        memory_views.push_back(sample);
    }
    write_checkpoint(out, id, pc, name, function, checkpoint, owns_rng_draw, memory_views, gprs);
}

void write_owned_csv(std::ostringstream& out, const std::vector<std::string>& values)
{
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ",";
        out << values[i];
    }
}

std::string memory_sample(std::string_view name, std::uint32_t address, std::string_view type)
{
    std::ostringstream out;
    out << name << ":" << hex_u32(address) << ":" << type;
    return out.str();
}

std::vector<std::string_view> action_view_globals()
{
    return {
        "global_camera_override_80347394:0x80347394:u32",
        "global_camera_flags_803472F4:0x803472F4:u32",
    };
}

std::string thread_runner_list_snapshot_sample()
{
    return "thread_list:head_ptr=0x80311A84,next=0x04,max=64,"
        "fields=callback@0x00:u32|next@0x04:u32|parent@0x08:u32|"
        "flags@0x18:u8|depth@0x1b:u8|order_bits@0x20:u32|payload_word@0x24:u32";
}

std::vector<std::string_view> action_source_globals()
{
    return {
        "global_action_source_state_80346bd8:0x80346BD8:u32",
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

std::vector<std::string_view> first_battle_enemy_id_samples()
{
    static const std::string slot4 =
        "enemy_id_slot4:" + hex_u32(addr::AddrRegistry::base(addr::battle::CombatantIdTable) + 4u * 2u) + ":u16";
    static const std::string slot5 =
        "enemy_id_slot5:" + hex_u32(addr::AddrRegistry::base(addr::battle::CombatantIdTable) + 5u * 2u) + ":u16";
    return {
        std::string_view(slot4.data(), slot4.size()),
        std::string_view(slot5.data(), slot5.size()),
    };
}

std::vector<std::string_view> damage_apply_gprs()
{
    return {
        "target_slot:3",
        "target_slot_saved:29",
        "hp_after:7",
        "damage:8",
    };
}

std::vector<std::string_view> death_handler_gate_gprs()
{
    return {
        "cur_hp:0",
        "combatant_instance:30",
        "target_slot:31",
    };
}

std::vector<std::string_view> enemy_drop_call_gprs()
{
    return {
        "target_slot:3",
        "target_slot_saved:31",
        "enemy_def_ptr:6",
    };
}

std::vector<std::string_view> drop_entry_gprs()
{
    return {
        "target_slot:3",
    };
}

std::vector<std::string_view> drop_rng_gprs()
{
    return {
        "target_slot:29",
        "drop_row_index_zero_based:30",
        "drop_threshold:27",
        "drop_row_ptr:28",
        "enemy_def_ptr:31",
    };
}

std::vector<std::string_view> drop_rng_samples()
{
    return {
        "drop_row_chance_raw:r28:0x72:u16",
        "drop_amount:r28:0x74:u16",
        "drop_item_id:r28:0x76:u16",
        "first_row_chance_raw:r31:0x72:u16",
        "first_row_amount:r31:0x74:u16",
        "first_row_item_id:r31:0x76:u16",
        "second_row_chance_raw:r31:0x78:u16",
        "second_row_amount:r31:0x7a:u16",
        "second_row_item_id:r31:0x7c:u16",
    };
}

std::vector<std::string_view> action_source_selection_samples()
{
    return {
        "selected_source_slot:r3:0x90:u16",
        "controller_actor_slot_0x92:r3:0x92:u16",
        "controller_target_slot_0x94:r3:0x94:u16",
    };
}

std::vector<std::string_view> action_source_state_r3_samples()
{
    return {
        "selected_source_slot:r3:0x90:u16",
        "controller_actor_slot_0x92:r3:0x92:u16",
        "controller_target_slot_0x94:r3:0x94:u16",
    };
}

std::vector<std::string_view> action_source_state_r4_samples()
{
    return {
        "selected_source_slot:r4:0x90:u16",
        "controller_actor_slot_0x92:r4:0x92:u16",
        "controller_target_slot_0x94:r4:0x94:u16",
    };
}

std::vector<std::string_view> action_source_state_global_addrprog_samples()
{
    return {
        "selected_source_slot_global:0x80346BD8:load_ptr32|+0x90:u16",
        "controller_actor_slot_0x92_global:0x80346BD8:load_ptr32|+0x92:u16",
        "controller_target_slot_0x94_global:0x80346BD8:load_ptr32|+0x94:u16",
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

std::vector<std::string> action_view_chain_addrprog_samples()
{
    return {
        "action_view_chain_loaded_resource_0x10:r31:+0x24|load_ptr32|+0x10:u32",
        "action_view_chain_aux_root_0x30:r31:+0x24|load_ptr32|+0x10|load_ptr32|+0x30:u32",
    };
}

std::vector<std::string> action_view_aux_table_fingerprint_addrprog_samples()
{
    std::vector<std::string> samples;
    // First-battle action-view queries need rows 5, 9, 12, 17, and 19
    // across the known PC/enemy _0_STD tables. Keep this light enough for
    // normal captures while covering every currently observed key 4/5/8 case.
    constexpr int kRowsToSample = 32;
    samples.reserve(kRowsToSample * 7);

    for (int row = 0; row < kRowsToSample; ++row) {
        const int base = row * 0x10;
        std::ostringstream prefix;
        prefix << "aux_row" << std::setw(2) << std::setfill('0') << row;
        const auto row_name = prefix.str();
        auto hex_offset = [](int offset) {
            std::ostringstream out;
            out << "+0x" << std::hex << std::uppercase << offset;
            return out.str();
        };

        samples.push_back(row_name + "_location_code:r3:" + hex_offset(base + 0x00) + ":u16");
        samples.push_back(row_name + "_opcode:r3:" + hex_offset(base + 0x02) + ":u16");
        samples.push_back(row_name + "_payload_size:r3:" + hex_offset(base + 0x08) + ":u32");
        samples.push_back(row_name + "_payload_ptr:r3:" + hex_offset(base + 0x0c) + ":u32");
        samples.push_back(row_name + "_payload_primary:r3:" + hex_offset(base + 0x0c)
            + "|load_ptr32|+0x00:u16");
        samples.push_back(row_name + "_payload_secondary:r3:" + hex_offset(base + 0x0c)
            + "|load_ptr32|+0x02:u16");
        samples.push_back(row_name + "_payload_direct_secondary:r3:" + hex_offset(base + 0x0c)
            + "|load_ptr32|+0x04:u16");
    }
    return samples;
}

std::vector<std::string> action_view_selector_query_addrprog_samples()
{
    auto samples = action_view_chain_addrprog_samples();
    auto aux_table_samples = action_view_aux_table_fingerprint_addrprog_samples();
    samples.insert(samples.end(), aux_table_samples.begin(), aux_table_samples.end());
    return samples;
}

std::vector<std::string_view> as_string_views(const std::vector<std::string>& values)
{
    std::vector<std::string_view> views;
    views.reserve(values.size());
    for (const auto& value : values) {
        views.push_back(value);
    }
    return views;
}

std::vector<std::string> std0_cache_global_samples()
{
    std::vector<std::string> samples;
    samples.reserve(26);
    samples.push_back(memory_sample("std0_transient_handoff_8030a20c", 0x8030A20Cu, "u32"));
    samples.push_back(memory_sample("std0_combatant_transient_handoff_8030a208", 0x8030A208u, "u32"));
    for (int i = 0; i < 12; ++i) {
        std::ostringstream suffix;
        suffix << std::setw(2) << std::setfill('0') << i;
        const auto index = suffix.str();
        const auto offset = static_cast<std::uint32_t>(i) * 4u;
        samples.push_back(memory_sample(
            "std0_cache_table_ptr_slot" + index,
            0x8030A214u + offset,
            "u32"));
        samples.push_back(memory_sample(
            "std0_cache_filename_key_slot" + index,
            0x8030A244u + offset,
            "u32"));
    }
    return samples;
}

std::vector<std::string_view> std_resource_string_word_samples_from_r4()
{
    return {
        "filename_word0:r4:0x00:u32",
        "filename_word4:r4:0x04:u32",
        "filename_word8:r4:0x08:u32",
        "filename_wordc:r4:0x0c:u32",
    };
}

std::vector<std::string_view> std_resource_string_word_samples_from_r3()
{
    return {
        "filename_word0:r3:0x00:u32",
        "filename_word4:r3:0x04:u32",
        "filename_word8:r3:0x08:u32",
        "filename_wordc:r3:0x0c:u32",
    };
}

std::vector<std::string_view> std_root_pointer_samples_from_r4()
{
    return {
        "root_ptr_value_before:r4:+0x00:u32",
        "root_prefix_location_0x00:r4:+0x00|load_ptr32|+0x00:u16",
        "root_prefix_opcode_0x02:r4:+0x00|load_ptr32|+0x02:u16",
        "root_rows_ptr_0x0c:r4:+0x00|load_ptr32|+0x0c:u32",
    };
}

void write_action_view_selector_helper_checkpoints(
    std::ostringstream& out,
    const std::vector<std::string_view>& action_view_chain_sample_views)
{
    auto write_helper_checkpoint = [&](std::string_view id,
                                       std::string_view pc,
                                       std::string_view name,
                                       std::string_view function,
                                       std::string_view checkpoint) {
        write_checkpoint(
            out,
            id,
            pc,
            name,
            function,
            checkpoint,
            false,
            action_view_globals(),
            action_view_category2_spawn_gprs(),
            action_view_category2_gate_samples(),
            action_view_chain_sample_views);
    };

    write_helper_checkpoint(
        "action_view_category2_spawn_80013334",
        "80013334",
        "action_view_category2_spawn",
        "FUN_80053f38",
        "action_view_spawn");

    write_helper_checkpoint(
        "action_view_category3_spawn_800133E4",
        "800133E4",
        "action_view_category3_spawn",
        "FUN_80053f38",
        "action_view_spawn");

    write_helper_checkpoint(
        "action_view_state0_actor_changed_spawn_8001318C",
        "8001318C",
        "action_view_state0_actor_changed_spawn",
        "FUN_80053f38",
        "action_view_spawn");

    write_helper_checkpoint(
        "action_view_mode0_state2_spawn_8001321C",
        "8001321C",
        "action_view_mode0_state2_spawn",
        "FUN_80053f38",
        "action_view_spawn");

    write_helper_checkpoint(
        "action_view_mode1_state2_call_8001329C",
        "8001329C",
        "action_view_mode1_state2_call",
        "FUN_80032bbc",
        "action_view_helper");

    write_helper_checkpoint(
        "action_view_mode2_tail_call_8001338C",
        "8001338C",
        "action_view_mode2_tail_call",
        "FUN_80032bbc",
        "action_view_helper");

    write_helper_checkpoint(
        "action_view_mode5_call_800134A4",
        "800134A4",
        "action_view_mode5_call",
        "FUN_80032bbc",
        "action_view_helper");
}

void write_action_view_selector_query_checkpoints(std::ostringstream& out)
{
    const auto action_view_chain_samples = action_view_chain_addrprog_samples();
    const auto action_view_chain_sample_views = as_string_views(action_view_chain_samples);
    const auto selector_query_samples = action_view_selector_query_addrprog_samples();
    const auto selector_query_sample_views = as_string_views(selector_query_samples);

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
        action_view_category2_gate_samples(),
        selector_query_sample_views);

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
        action_view_category2_gate_samples(),
        action_view_chain_sample_views);

    write_checkpoint(
        out,
        "action_view_category3_query_call_800133CC",
        "800133CC",
        "action_view_category3_query_call",
        "FUN_80012f58",
        "action_view_query",
        false,
        action_view_globals(),
        action_view_category2_gate_gprs(),
        action_view_category2_gate_samples(),
        selector_query_sample_views);

    write_checkpoint(
        out,
        "action_view_category3_query_result_800133D0",
        "800133D0",
        "action_view_category3_query_result",
        "FUN_80012f58",
        "action_view_query_result",
        false,
        action_view_globals(),
        action_view_category2_result_gprs(),
        action_view_category2_gate_samples(),
        action_view_chain_sample_views);

    write_checkpoint(
        out,
        "action_view_category5_query_call_80013478",
        "80013478",
        "action_view_category5_query_call",
        "FUN_80012f58",
        "action_view_query",
        false,
        action_view_globals(),
        action_view_category2_gate_gprs(),
        action_view_category2_gate_samples(),
        selector_query_sample_views);

    write_checkpoint(
        out,
        "action_view_category5_query_result_8001347C",
        "8001347C",
        "action_view_category5_query_result",
        "FUN_80012f58",
        "action_view_query_result",
        false,
        action_view_globals(),
        action_view_category2_result_gprs(),
        action_view_category2_gate_samples(),
        action_view_chain_sample_views);

    write_action_view_selector_helper_checkpoints(out, action_view_chain_sample_views);
}

std::vector<std::string_view> action_view_selector_core_gprs()
{
    return {
        "requested_mode_r4:4",
        "selector_worksheet:29",
        "action_payload:30",
        "combatant_thread:31",
    };
}

void write_action_view_selector_coverage_checkpoints(std::ostringstream& out)
{
    const auto chain_samples = action_view_chain_addrprog_samples();
    const auto chain_sample_views = as_string_views(chain_samples);

    const auto write_selector_checkpoint =
        [&](std::string_view id,
            std::string_view pc,
            std::string_view name,
            std::string_view checkpoint,
            const std::vector<std::string_view>& gprs = action_view_selector_core_gprs()) {
            write_checkpoint(
                out,
                id,
                pc,
                name,
                "FUN_80012f58",
                checkpoint,
                false,
                action_view_globals(),
                gprs,
                action_view_category2_gate_samples(),
                chain_sample_views);
        };

    write_selector_checkpoint(
        "action_view_selector_field6_classify_80012FCC",
        "80012FCC",
        "action_view_selector_field6_classify",
        "action_view_selector_entry");

    write_selector_checkpoint(
        "action_view_selector_requested_mode_ready_80013038",
        "80013038",
        "action_view_selector_requested_mode_ready",
        "action_view_selector_requested_mode");

    write_selector_checkpoint(
        "action_view_selector_state_after_request_800130D8",
        "800130D8",
        "action_view_selector_state_after_request",
        "action_view_selector_state_after_request");

    write_selector_checkpoint(
        "action_view_selector_state0_effective_mode_write_800130F4",
        "800130F4",
        "action_view_selector_state0_effective_mode_write",
        "action_view_selector_effective_mode_write");

    write_selector_checkpoint(
        "action_view_selector_dispatch_800131AC",
        "800131AC",
        "action_view_selector_dispatch",
        "action_view_selector_dispatch");

    write_action_view_selector_helper_checkpoints(out, chain_sample_views);
}

std::vector<std::string_view> first_battle_queued_instruction_samples()
{
    return {
        "slot0_instruction_0x0:0x80309174:u32",
        "slot0_target_0x4:0x80309178:u8",
        "slot0_instr_param_0x6:0x8030917A:u16",
        "slot0_attack_result_0xc:0x80309180:u8",
        "slot1_instruction_0x0:0x80309194:u32",
        "slot1_target_0x4:0x80309198:u8",
        "slot1_instr_param_0x6:0x8030919A:u16",
        "slot1_attack_result_0xc:0x803091A0:u8",
        "slot4_instruction_0x0:0x803091F4:u32",
        "slot4_target_0x4:0x803091F8:u8",
        "slot4_instr_param_0x6:0x803091FA:u16",
        "slot4_attack_result_0xc:0x80309200:u8",
        "slot5_instruction_0x0:0x80309214:u32",
        "slot5_target_0x4:0x80309218:u8",
        "slot5_instr_param_0x6:0x8030921A:u16",
        "slot5_attack_result_0xc:0x80309220:u8",
    };
}

std::vector<std::string_view> first_battle_counter_state_samples()
{
    return {
        "slot0_action_marker_0x0:0x80309730:u8",
        "slot0_critical_marker_0x8:0x80309738:u8",
        "slot1_action_marker_0x0:0x80309740:u8",
        "slot1_critical_marker_0x8:0x80309748:u8",
        "slot4_action_marker_0x0:0x80309770:u8",
        "slot4_critical_marker_0x8:0x80309778:u8",
        "slot5_action_marker_0x0:0x80309780:u8",
        "slot5_critical_marker_0x8:0x80309788:u8",
    };
}

std::array<int, 4> first_battle_focus_slots()
{
    return { 0, 1, 4, 5 };
}

std::vector<std::string> first_battle_pre_handler_frame_memory_samples()
{
    std::vector<std::string> samples;
    samples.push_back(memory_sample("rng_seed_before", addr::AddrRegistry::base(addr::core::RNG_SEED), "u32"));
    samples.push_back("turn_phase_8034733c:0x8034733C:u32");
    samples.push_back("battle_input_state_80347338:0x80347338:u32");
    samples.push_back("active_actor_slot_80347334:0x80347334:u8");
    samples.push_back("action_sequence_80347335:0x80347335:u8");

    for (const int slot : first_battle_focus_slots()) {
        const auto slot_text = std::to_string(slot);
        const auto slot_offset = static_cast<std::uint32_t>(slot);
        samples.push_back(memory_sample(
            "slot" + slot_text + "_movement_buffer_ptr",
            0x80309700u + slot_offset * 4u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_combatant_thread_ptr",
            0x80309E24u + slot_offset * 4u,
            "u32"));
        samples.push_back(memory_sample(
            "pos" + slot_text + "_x_bits",
            0x8030980Cu + slot_offset * 0x10u,
            "u32"));
        samples.push_back(memory_sample(
            "pos" + slot_text + "_z_bits",
            0x80309810u + slot_offset * 0x10u,
            "u32"));
    }

    for (const auto sample : first_battle_queued_instruction_samples()) {
        samples.emplace_back(sample);
    }
    for (const auto sample : first_battle_counter_state_samples()) {
        samples.emplace_back(sample);
    }
    return samples;
}

std::vector<std::string> first_battle_pre_handler_frame_addrprog_samples()
{
    std::vector<std::string> samples;
    for (const int slot : first_battle_focus_slots()) {
        const auto slot_text = std::to_string(slot);
        const auto slot_offset = static_cast<std::uint32_t>(slot);
        const auto movement_base = hex_u32(0x80309700u + slot_offset * 4u);
        const auto movement_prefix = "slot" + slot_text + "_movement";

        samples.push_back(movement_prefix + "_callback_0x00:" + movement_base + ":load_ptr32|+0x00:u32");
        samples.push_back(movement_prefix + "_state_0x19:" + movement_base + ":load_ptr32|+0x19:u8");
        samples.push_back(movement_prefix + "_order_bits_0x20:" + movement_base + ":load_ptr32|+0x20:u32");
        samples.push_back(movement_prefix + "_worksheet_ptr_0x24:" + movement_base + ":load_ptr32|+0x24:u32");
        samples.push_back(movement_prefix + "_worksheet_slot_0x00:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x00:u8");
        samples.push_back(movement_prefix + "_worksheet_flags_0x04:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x04:u32");
        samples.push_back(movement_prefix + "_worksheet_cur_grid_x_0x0c:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x0c:u8");
        samples.push_back(movement_prefix + "_worksheet_cur_grid_z_0x0d:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x0d:u8");
        samples.push_back(movement_prefix + "_worksheet_prev_grid_x_0x0e:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x0e:u8");
        samples.push_back(movement_prefix + "_worksheet_prev_grid_z_0x0f:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x0f:u8");
        samples.push_back(movement_prefix + "_worksheet_pending_handler_0x10:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x10:u32");
        samples.push_back(movement_prefix + "_worksheet_path_index_0x15:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x15:u8");
        samples.push_back(movement_prefix + "_worksheet_status_0x16:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x16:u8");
        samples.push_back(movement_prefix + "_worksheet_setup_byte_0x50:" + movement_base + ":load_ptr32|+0x24|load_ptr32|+0x50:u8");
    }
    return samples;
}

std::vector<std::string> first_battle_float_motion_addrprog_samples()
{
    auto samples = first_battle_pre_handler_frame_addrprog_samples();
    for (const int slot : first_battle_focus_slots()) {
        const auto slot_text = std::to_string(slot);
        const auto slot_offset = static_cast<std::uint32_t>(slot);
        const auto thread_base = hex_u32(0x80309E24u + slot_offset * 4u);
        const auto prefix = "slot" + slot_text;
        const auto thread_chain = thread_base + ":load_ptr32";
        const auto cw_chain = thread_chain + "|+0x24|load_ptr32";
        const auto iw_chain = cw_chain + "|+0x4c|load_ptr32";

        samples.push_back(prefix + "_thread_callback_0x00:" + thread_chain + "|+0x00:u32");
        samples.push_back(prefix + "_thread_state_0x19:" + thread_chain + "|+0x19:u8");
        samples.push_back(prefix + "_thread_payload_0x24:" + thread_chain + "|+0x24:u32");
        samples.push_back(prefix + "_cw_slot_0x00:" + cw_chain + "|+0x00:u8");
        samples.push_back(prefix + "_cw_cur_x_0x1c:" + cw_chain + "|+0x1c:u32");
        samples.push_back(prefix + "_cw_cur_y_0x20:" + cw_chain + "|+0x20:u32");
        samples.push_back(prefix + "_cw_cur_z_0x24:" + cw_chain + "|+0x24:u32");
        samples.push_back(prefix + "_iw_ptr_0x4c:" + cw_chain + "|+0x4c:u32");
        samples.push_back(prefix + "_iw_slot_0x00:" + iw_chain + "|+0x00:u8");
        samples.push_back(prefix + "_iw_target_0x04:" + iw_chain + "|+0x04:u8");
        samples.push_back(prefix + "_iw_action_mode_0x06:" + iw_chain + "|+0x06:u16");
        samples.push_back(prefix + "_iw_action_row_0xe4:" + iw_chain + "|+0xe4:u16");
        samples.push_back(prefix + "_iw_flags_0xec:" + iw_chain + "|+0xec:u32");
        samples.push_back(prefix + "_iw_flags_0xf0:" + iw_chain + "|+0xf0:u32");
        samples.push_back(prefix + "_iw_snapshot_x_0xf8:" + iw_chain + "|+0xf8:u32");
        samples.push_back(prefix + "_iw_snapshot_z_0x100:" + iw_chain + "|+0x100:u32");
        samples.push_back(prefix + "_iw_move_inc_x_0x104:" + iw_chain + "|+0x104:u32");
        samples.push_back(prefix + "_iw_move_inc_y_0x108:" + iw_chain + "|+0x108:u32");
        samples.push_back(prefix + "_iw_move_inc_z_0x10c:" + iw_chain + "|+0x10c:u32");
        samples.push_back(prefix + "_iw_target_x_0x110:" + iw_chain + "|+0x110:u32");
        samples.push_back(prefix + "_iw_target_y_0x114:" + iw_chain + "|+0x114:u32");
        samples.push_back(prefix + "_iw_target_z_0x118:" + iw_chain + "|+0x118:u32");
        samples.push_back(prefix + "_iw_speed_0x12c:" + iw_chain + "|+0x12c:u32");
        samples.push_back(prefix + "_iw_alt_speed_0x130:" + iw_chain + "|+0x130:u32");
    }
    return samples;
}

std::vector<std::string_view> movement_path_list_samples_from_r3()
{
    return {
        "movement_dist_to_target_0x14:r3:0x14:u8",
        "movement_path_node0_x_0x17:r3:0x17:u8",
        "movement_path_node0_z_0x18:r3:0x18:u8",
        "movement_path_node1_x_0x19:r3:0x19:u8",
        "movement_path_node1_z_0x1a:r3:0x1a:u8",
        "movement_path_node2_x_0x1b:r3:0x1b:u8",
        "movement_path_node2_z_0x1c:r3:0x1c:u8",
        "movement_path_node3_x_0x1d:r3:0x1d:u8",
        "movement_path_node3_z_0x1e:r3:0x1e:u8",
        "movement_path_node4_x_0x1f:r3:0x1f:u8",
        "movement_path_node4_z_0x20:r3:0x20:u8",
        "movement_path_node5_x_0x21:r3:0x21:u8",
        "movement_path_node5_z_0x22:r3:0x22:u8",
        "movement_path_node6_x_0x23:r3:0x23:u8",
        "movement_path_node6_z_0x24:r3:0x24:u8",
        "movement_path_node7_x_0x25:r3:0x25:u8",
        "movement_path_node7_z_0x26:r3:0x26:u8",
    };
}

std::vector<std::string_view> movement_commit_samples_from_r3()
{
    auto samples = std::vector<std::string_view>{
        "movement_cur_x_0x0c:r3:0x0c:u8",
        "movement_cur_z_0x0d:r3:0x0d:u8",
        "movement_pending_handler_0x10:r3:0x10:u32",
        "movement_path_index_0x15:r3:0x15:u8",
        "movement_status_0x16:r3:0x16:u8",
    };
    const auto path_samples = movement_path_list_samples_from_r3();
    samples.insert(samples.end(), path_samples.begin(), path_samples.end());
    return samples;
}

std::vector<std::string_view> float_motion_instruction_samples_from_r31()
{
    return {
        "inst_slot_0x00:r31:0x00:u8",
        "inst_target_0x04:r31:0x04:u8",
        "inst_action_mode_0x06:r31:0x06:u16",
        "inst_action_row_0xe4:r31:0xe4:u16",
        "inst_flags_0xec:r31:0xec:u32",
        "inst_flags_0xf0:r31:0xf0:u32",
        "inst_snapshot_x_0xf8:r31:0xf8:u32",
        "inst_snapshot_y_0xfc:r31:0xfc:u32",
        "inst_snapshot_z_0x100:r31:0x100:u32",
        "inst_move_inc_x_0x104:r31:0x104:u32",
        "inst_move_inc_y_0x108:r31:0x108:u32",
        "inst_move_inc_z_0x10c:r31:0x10c:u32",
        "inst_target_x_0x110:r31:0x110:u32",
        "inst_target_y_0x114:r31:0x114:u32",
        "inst_target_z_0x118:r31:0x118:u32",
        "inst_speed_0x12c:r31:0x12c:u32",
        "inst_alt_speed_0x130:r31:0x130:u32",
    };
}

std::vector<std::string_view> float_motion_worker_loaded_samples()
{
    return {
        "worker_callback_0x00:r28:0x00:u32",
        "worker_state_0x19:r28:0x19:u8",
        "worker_order_bits_0x20:r28:0x20:u32",
        "worker_payload_0x24:r28:0x24:u32",
        "payload_frame_or_index_0x00:r31:0x00:u16",
        "payload_primary_thread_0x04:r31:0x04:u32",
        "payload_secondary_thread_0x08:r31:0x08:u32",
        "payload_vector_x_0x0c:r31:0x0c:u32",
        "payload_vector_y_0x10:r31:0x10:u32",
        "payload_vector_z_0x14:r31:0x14:u32",
        "payload_action_record_0x2c:r31:0x2c:u32",
        "payload_phase_0x30:r31:0x30:u8",
    };
}

std::vector<std::string> float_motion_worker_actor_addrprog_samples()
{
    return {
        "primary_cw_slot_0x00:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x00:u8",
        "primary_cw_cur_x_0x1c:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x1c:u32",
        "primary_cw_cur_y_0x20:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x20:u32",
        "primary_cw_cur_z_0x24:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x24:u32",
        "primary_iw_action_mode_0x06:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
        "primary_iw_flags_0xec:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0xec:u32",
        "primary_iw_flags_0xf0:r31:+0x04|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0xf0:u32",
        "secondary_cw_slot_0x00:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x00:u8",
        "secondary_cw_cur_x_0x1c:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x1c:u32",
        "secondary_cw_cur_y_0x20:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x20:u32",
        "secondary_cw_cur_z_0x24:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x24:u32",
        "secondary_iw_action_mode_0x06:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
        "secondary_iw_flags_0xec:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0xec:u32",
        "secondary_iw_flags_0xf0:r31:+0x08|load_ptr32|+0x24|load_ptr32|+0x4c|load_ptr32|+0xf0:u32",
    };
}

std::vector<std::string_view> float_motion_store_samples_from_r3()
{
    return {
        "dest_cw_slot_0x00:r3:0x00:u8",
        "dest_cw_cur_x_0x1c:r3:0x1c:u32",
        "dest_cw_cur_y_0x20:r3:0x20:u32",
        "dest_cw_cur_z_0x24:r3:0x24:u32",
        "delta_x_bits_stack_0x08:r1:0x08:u32",
        "delta_z_bits_stack_0x10:r1:0x10:u32",
    };
}

std::vector<std::string_view> float_motion_worker_gprs()
{
    return {
        "thread_arg_or_dest_cw:3",
        "thread_saved:28",
        "instruction_wksht:30",
        "payload:31",
    };
}

std::vector<std::string_view> setup_action_gprs()
{
    return {
        "actor_slot:29",
        "handler_pc:31",
    };
}

std::vector<std::string_view> pc_handler_gprs()
{
    return {
        "actor_slot:30",
        "target_slot:31",
    };
}

std::vector<std::string_view> pc_movement_helper_gprs()
{
    return {
        "actor_slot:30",
        "target_slot:31",
        "helper_result:3",
        "selected_worker_pc:0",
    };
}

std::vector<std::string_view> enemy_handler_gprs()
{
    return {
        "actor_slot:30",
        "target_slot:31",
    };
}

std::vector<std::string_view> enemy_movement_helper_gprs()
{
    return {
        "helper_result:3",
        "actor_slot:30",
        "target_slot:31",
    };
}

std::vector<std::string_view> enemy_worker_select_gprs()
{
    return {
        "selected_worker_pc:0",
        "actor_slot:30",
        "target_slot:31",
    };
}

std::vector<std::string_view> enemy_setup_gprs()
{
    return {
        "actor_slot:30",
        "target_slot:31",
        "setup_rand:3",
    };
}

std::vector<std::string_view> counter_gate_gprs()
{
    return {
        "attacker_slot:27",
        "target_slot:29",
        "target_slot_x4:30",
        "target_instance:28",
    };
}

std::vector<std::string_view> counter_roll_compare_gprs()
{
    return {
        "counter_rand_mod100:3",
        "target_current_counter_chance:0",
        "attacker_slot:27",
        "target_slot:29",
        "target_slot_x4:30",
        "target_instance:28",
    };
}

std::vector<std::string_view> counter_return_gprs()
{
    return {
        "counter_result:3",
        "attacker_slot:27",
        "target_slot:29",
        "target_slot_x4:30",
        "target_instance:28",
    };
}

std::vector<std::string_view> counter_followup_gprs()
{
    return {
        "counter_result:3",
        "attacker_slot:31",
        "target_slot:30",
    };
}

std::vector<std::string_view> counter_target_instance_samples()
{
    return {
        "target_status_flags:r28:0x1c:u32",
        "target_movement_flags:r28:0xae:u16",
        "target_current_counter_chance:r28:0xb4:u16",
        "target_base_counter_chance:r28:0xba:u16",
        "target_counter_chance_increment:r28:0x08:u16",
    };
}

std::vector<std::string_view> attack_result_gate_gprs()
{
    return {
        "instr_param_0x6:0",
        "r3_return_or_arg:3",
        "attack_result:26",
        "active_slot:28",
        "r29_attack_context:29",
        "target_slot_word:31",
    };
}

std::vector<std::string_view> attack_result_rng_gprs()
{
    return {
        "r0_instr_param_candidate:0",
        "r3_return_or_arg:3",
        "attack_result:26",
        "active_slot:28",
        "r29_attack_context:29",
        "target_slot_word:31",
    };
}

std::vector<std::string_view> concat(
    std::vector<std::string_view> lhs,
    const std::vector<std::string_view>& rhs)
{
    lhs.insert(lhs.end(), rhs.begin(), rhs.end());
    return lhs;
}

void write_predictor_validation_rng_checkpoint(
    std::ostringstream& out,
    std::string_view pc,
    std::string_view owner)
{
    if (pc == "800513D4") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
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
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "FUN_80052b24",
            owner,
            true,
            {},
            {"r30_worksheet:30", "r31_target_buffer:31"},
            action_view_mode0e_rng_samples());
    } else if (pc == "8008BC68") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "Battle::HandleECInst_8008b9e0",
            "enemy_setup",
            true,
            first_battle_queued_instruction_samples(),
            enemy_setup_gprs());
    } else if (pc == "80081A88") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "Battle::AtkMethods::shouldCounter_800819d0",
            "counter_roll",
            true,
            concat(first_battle_queued_instruction_samples(), first_battle_counter_state_samples()),
            counter_gate_gprs(),
            counter_target_instance_samples());
    } else if (pc == "80010BDC") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "getAttackResult_80010b8c",
            "hit",
            true,
            first_battle_queued_instruction_samples(),
            attack_result_rng_gprs());
    } else if (pc == "80010C44") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "getAttackResult_80010b8c",
            "crit",
            true,
            first_battle_queued_instruction_samples(),
            attack_result_rng_gprs());
    } else if (pc == "8002BAD8") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "enemyDropItem_8002ba8c",
            "row",
            true,
            first_battle_enemy_id_samples(),
            drop_rng_gprs(),
            drop_rng_samples());
    } else if (is_combat_effect_burst_pc(pc)) {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
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
            section_id(std::string(owner), std::string(pc)),
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
            section_id(std::string(owner), std::string(pc)),
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
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "RNG",
            owner,
            true);
    }
}

void write_action_source_selection_checkpoints(std::ostringstream& out)
{
    write_checkpoint(
        out,
        "action_source_selection_entry_8006782C",
        "8006782C",
        "action_source_selection_entry",
        "FUN_8006782c",
        "action_source_selection_entry",
        false,
        action_source_globals(),
        {"r3_callback_thread:3"});

    write_checkpoint(
        out,
        "action_source_selection_candidate_80067A9C",
        "80067A9C",
        "action_source_selection_candidate",
        "FUN_8006782c",
        "source_selection",
        false,
        action_source_globals(),
        {
            "r3_selected_element:3",
            "r4_action_source_state:4",
            "r5_selected_source_slot_candidate:5",
        },
        action_source_state_r4_samples());

    write_checkpoint(
        out,
        "action_source_selection_mode_gate_80067AD0",
        "80067AD0",
        "action_source_selection_mode_gate",
        "FUN_8006782c",
        "source_selection",
        false,
        action_source_globals(),
        {
            "r0_selected_source_slot_fallback:0",
            "r3_action_source_state:3",
            "r5_occupied_slot_count:5",
        },
        action_source_state_r3_samples());

    write_checkpoint(
        out,
        "action_source_selection_source_slot_80067B50",
        "80067B50",
        "action_source_selection_source_slot",
        "FUN_8006782c",
        "source_selection",
        false,
        action_source_globals(),
        {"r3_callback_thread:3", "r27_callback_thread:27"},
        {},
        action_source_state_global_addrprog_samples());

    write_checkpoint(
        out,
        "action_source_state_downstream_read_80067BD0",
        "80067BD0",
        "action_source_state_downstream_read",
        "FUN_8006782c",
        "action_source_state_downstream_read",
        false,
        action_source_globals(),
        {"r3_action_source_state:3"},
        action_source_state_r3_samples());
}

void write_counter_internal_checkpoints(std::ostringstream& out)
{
    const auto counter_memory =
        concat(first_battle_queued_instruction_samples(), first_battle_counter_state_samples());
    write_checkpoint(
        out,
        "counter_gate_entry_800819D0",
        "800819D0",
        "counter_gate_entry",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_gate",
        false,
        counter_memory,
        {"attacker_slot:3", "target_slot:4"});

    write_checkpoint(
        out,
        "counter_gate_inputs_800819FC",
        "800819FC",
        "counter_gate_inputs",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_gate_inputs",
        false,
        counter_memory,
        counter_gate_gprs(),
        counter_target_instance_samples());

    write_checkpoint(
        out,
        "counter_roll_compare_80081AB0",
        "80081AB0",
        "counter_roll_compare",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_roll_compare",
        false,
        counter_memory,
        counter_roll_compare_gprs(),
        counter_target_instance_samples());

    write_checkpoint(
        out,
        "counter_gate_queue_write_complete_80081B54",
        "80081B54",
        "counter_gate_queue_write_complete",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_gate_result",
        false,
        counter_memory,
        counter_return_gprs(),
        counter_target_instance_samples());

    write_checkpoint(
        out,
        "counter_gate_success_80081B7C",
        "80081B7C",
        "counter_gate_success",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_gate_result",
        false,
        counter_memory,
        counter_return_gprs(),
        counter_target_instance_samples());

    write_checkpoint(
        out,
        "counter_gate_return_80081B80",
        "80081B80",
        "counter_gate_return",
        "Battle::AtkMethods::shouldCounter_800819d0",
        "counter_gate_result",
        false,
        counter_memory,
        counter_return_gprs(),
        counter_target_instance_samples());

    write_checkpoint(
        out,
        "counter_after_should_return_80081D78",
        "80081D78",
        "counter_after_should_return",
        "Battle::AtkMethods::performAttack_80081b94",
        "counter_gate_return",
        false,
        counter_memory,
        counter_followup_gprs());

    write_checkpoint(
        out,
        "counter_followup_call_prepare_80081D80",
        "80081D80",
        "counter_followup_call_prepare",
        "Battle::AtkMethods::performAttack_80081b94",
        "counter_follow_up",
        false,
        counter_memory,
        counter_followup_gprs());

    write_checkpoint(
        out,
        "counter_followup_return_80081D88",
        "80081D88",
        "counter_followup_return",
        "Battle::AtkMethods::performAttack_80081b94",
        "counter_follow_up",
        false,
        counter_memory,
        counter_followup_gprs());
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
        } else if (pc == "8008BC68") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "Battle::HandleECInst_8008b9e0",
                "enemy_setup",
                true,
                first_battle_queued_instruction_samples(),
                enemy_setup_gprs());
        } else if (pc == "80010BDC") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "getAttackResult_80010b8c",
                "hit",
                true,
                first_battle_queued_instruction_samples(),
                attack_result_rng_gprs());
        } else if (pc == "80010C44") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "getAttackResult_80010b8c",
                "crit",
                true,
                first_battle_queued_instruction_samples(),
                attack_result_rng_gprs());
        } else if (pc == "80081A88") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "Battle::AtkMethods::shouldCounter_800819d0",
                "counter_roll",
                true,
                concat(first_battle_queued_instruction_samples(), first_battle_counter_state_samples()),
                counter_gate_gprs(),
                counter_target_instance_samples());
        } else if (pc == "8002BAD8") {
            write_checkpoint(
                out,
                section_id(owner, pc),
                pc,
                owner,
                "enemyDropItem_8002ba8c",
                "row",
                true,
                first_battle_enemy_id_samples(),
                drop_rng_gprs(),
                drop_rng_samples());
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
        "damage_apply_death_call_8002DD14",
        "8002DD14",
        "damage_apply_death_call",
        "zzDealDamage_8002dc14",
        "damage_apply",
        false,
        first_battle_enemy_id_samples(),
        damage_apply_gprs());

    write_checkpoint(
        out,
        "death_handler_hp_gate_8002BC80",
        "8002BC80",
        "death_handler_hp_gate",
        "HandleCombatantDeath_8002bc4c",
        "death_handler_gate",
        false,
        first_battle_enemy_id_samples(),
        death_handler_gate_gprs());

    write_checkpoint(
        out,
        "enemy_drop_call_8002BD20",
        "8002BD20",
        "enemy_drop_call",
        "HandleCombatantDeath_8002bc4c",
        "enemy_drop_call",
        false,
        first_battle_enemy_id_samples(),
        enemy_drop_call_gprs());

    write_checkpoint(
        out,
        "enemy_drop_entry_8002BA8C",
        "8002BA8C",
        "enemy_drop_entry",
        "enemyDropItem_8002ba8c",
        "enemy_drop_entry",
        false,
        first_battle_enemy_id_samples(),
        drop_entry_gprs());

    write_action_source_selection_checkpoints(out);

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

    write_counter_internal_checkpoints(out);

    write_action_view_selector_query_checkpoints(out);

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

std::string build_first_battle_predictor_validation_profile_ini()
{
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_predictor_validation\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    for (const auto& [pc, owner] : known_rng_callsite_owners()) {
        if (is_excluded_from_default_live_profile(pc)) {
            continue;
        }
        write_predictor_validation_rng_checkpoint(out, pc, owner);
    }

    write_checkpoint(
        out,
        "setup_action_800708C0",
        "800708C0",
        "setup_action",
        "Battle::Run::setupAction_800708c0",
        "setup_action",
        false,
        first_battle_queued_instruction_samples(),
        setup_action_gprs());

    write_checkpoint(
        out,
        "pc_handler_entry_80086C68",
        "80086C68",
        "pc_handler",
        "Battle::HandlePCInst_80086c68",
        "pc_handler",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "pc_attack_param_initial_zero_80086D18",
        "80086D18",
        "pc_attack_param_initial_zero",
        "Battle::HandlePCInst_80086c68",
        "instr_param_set",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "pc_attack_helper_80083728_return_80085670",
        "80085670",
        "pc_attack_helper_80083728_return",
        "FUN_800855ac",
        "movement_helper_return",
        false,
        first_battle_queued_instruction_samples(),
        pc_movement_helper_gprs());

    write_checkpoint(
        out,
        "pc_attack_helper_80082340_return_80085680",
        "80085680",
        "pc_attack_helper_80082340_return",
        "FUN_800855ac",
        "movement_distance_return",
        false,
        first_battle_queued_instruction_samples(),
        pc_movement_helper_gprs());

    for (const auto& [pc, id] : {
             std::pair<std::string_view, std::string_view>{"80085608", "pc_attack_fallback_param_set_a_80085608"},
             std::pair<std::string_view, std::string_view>{"800856C4", "pc_attack_fallback_param_set_b_800856C4"},
             std::pair<std::string_view, std::string_view>{"800856F8", "pc_attack_fallback_param_set_c_800856F8"},
         }) {
        write_checkpoint(
            out,
            id,
            pc,
            "pc_attack_fallback_param_set",
            "FUN_800855ac",
            "instr_param_set",
            false,
            first_battle_queued_instruction_samples(),
            pc_movement_helper_gprs());
    }

    write_checkpoint(
        out,
        "pc_attack_closest_combatant_return_800856D8",
        "800856D8",
        "pc_attack_closest_combatant_return",
        "FUN_800855ac",
        "retarget_helper_return",
        false,
        first_battle_queued_instruction_samples(),
        pc_movement_helper_gprs());

    write_checkpoint(
        out,
        "pc_check_target_adjacent_return_80086E34",
        "80086E34",
        "pc_check_target_adjacent_return",
        "Battle::HandlePCInst_80086c68",
        "target_adjacent",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "pc_attack_param_reset_zero_80086E4C",
        "80086E4C",
        "pc_attack_param_reset_zero",
        "Battle::HandlePCInst_80086c68",
        "instr_param_set",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "pc_direct_attack_worker_call_80086F48",
        "80086F48",
        "pc_direct_attack_worker_call",
        "Battle::HandlePCInst_80086c68",
        "worker_select",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "pc_fallback_attack_worker_entry_80085CE0",
        "80085CE0",
        "pc_fallback_attack_worker_entry",
        "FUN_80085ce0",
        "worker_select",
        false,
        first_battle_queued_instruction_samples(),
        pc_handler_gprs());

    write_checkpoint(
        out,
        "enemy_handler_entry_8008B9E0",
        "8008B9E0",
        "enemy_handler",
        "Battle::HandleECInst_8008b9e0",
        "enemy_handler",
        false,
        first_battle_queued_instruction_samples(),
        enemy_handler_gprs());

    for (const auto& [pc, id, checkpoint] : {
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008A660", "soldier_attack_param_store_a_8008A660", "soldier_ai_instr_param_set"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008A678", "soldier_attack_param_store_b_8008A678", "soldier_ai_instr_param_set"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008A690", "soldier_attack_param_store_c_8008A690", "soldier_ai_instr_param_set"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BCB0", "enemy_helper_8008a174_return_8008BCB0", "movement_helper_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BCCC", "enemy_helper_80082340_return_8008BCCC", "movement_distance_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BD0C", "enemy_helper_8008a174_return_8008BD0C", "movement_helper_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BD24", "enemy_helper_8008a280_return_8008BD24", "movement_scope_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BD40", "enemy_check_target_adjacent_return_8008BD40", "target_adjacent"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BD64", "enemy_helper_8008a174_return_8008BD64", "movement_helper_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BE4C", "enemy_helper_8008a280_return_8008BE4C", "movement_scope_return"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BDAC", "enemy_direct_worker_select_8008BDAC", "worker_select"},
             std::tuple<std::string_view, std::string_view, std::string_view>{"8008BDDC", "enemy_fallback_worker_select_8008BDDC", "worker_select"},
         }) {
        const auto helper_gprs =
            checkpoint == "worker_select" ? enemy_worker_select_gprs()
            : checkpoint == "movement_helper_return"
                || checkpoint == "movement_distance_return"
                || checkpoint == "movement_scope_return"
                || checkpoint == "target_adjacent"
                ? enemy_movement_helper_gprs()
                : enemy_handler_gprs();
        write_checkpoint(
            out,
            id,
            pc,
            id,
            "Battle::HandleECInst_8008b9e0",
            checkpoint,
            false,
            first_battle_queued_instruction_samples(),
            helper_gprs);
    }

    write_checkpoint(
        out,
        "attack_resolution_begin_80081B94",
        "80081B94",
        "attack_resolution_begin",
        "Battle::AtkMethods::performAttack_80081b94",
        "attack_begin",
        false,
        first_battle_queued_instruction_samples(),
        attack_resolution_begin_gprs());

    write_checkpoint(
        out,
        "attack_result_return_80081BE8",
        "80081BE8",
        "attack_result_return",
        "Battle::AtkMethods::performAttack_80081b94",
        "attack_result_return",
        false,
        first_battle_queued_instruction_samples(),
        {"attack_result:3", "actor_slot:4", "target_slot:29"});

    write_checkpoint(
        out,
        "attack_result_write_80081C48",
        "80081C48",
        "attack_result_write",
        "Battle::AtkMethods::performAttack_80081b94",
        "attack_result_write",
        false,
        first_battle_queued_instruction_samples(),
        {"attack_result:0", "actor_slot:4", "target_slot:29"});

    write_checkpoint(
        out,
        "crit_gate_branch_80010C40",
        "80010C40",
        "crit_gate_branch",
        "getAttackResult_80010b8c",
        "crit_gate",
        false,
        first_battle_queued_instruction_samples(),
        attack_result_gate_gprs());

    write_checkpoint(
        out,
        "crit_gate_return_80010CA4",
        "80010CA4",
        "crit_gate_return",
        "getAttackResult_80010b8c",
        "crit_gate_return",
        false,
        first_battle_queued_instruction_samples(),
        attack_result_gate_gprs());

    write_checkpoint(
        out,
        "damage_apply_death_call_8002DD14",
        "8002DD14",
        "damage_apply_death_call",
        "zzDealDamage_8002dc14",
        "damage_apply",
        false,
        concat(first_battle_enemy_id_samples(), first_battle_queued_instruction_samples()),
        damage_apply_gprs());

    write_checkpoint(
        out,
        "death_handler_hp_gate_8002BC80",
        "8002BC80",
        "death_handler_hp_gate",
        "HandleCombatantDeath_8002bc4c",
        "death_handler_gate",
        false,
        first_battle_enemy_id_samples(),
        death_handler_gate_gprs());

    write_checkpoint(
        out,
        "enemy_drop_call_8002BD20",
        "8002BD20",
        "enemy_drop_call",
        "HandleCombatantDeath_8002bc4c",
        "enemy_drop_call",
        false,
        first_battle_enemy_id_samples(),
        enemy_drop_call_gprs());

    write_checkpoint(
        out,
        "enemy_drop_entry_8002BA8C",
        "8002BA8C",
        "enemy_drop_entry",
        "enemyDropItem_8002ba8c",
        "enemy_drop_entry",
        false,
        first_battle_enemy_id_samples(),
        drop_entry_gprs());

    write_counter_internal_checkpoints(out);

    write_checkpoint(
        out,
        "counter_followup_dispatch_80082134",
        "80082134",
        "counter_followup_dispatch",
        "Battle::AtkMethods::setupTurnAction_80082134",
        "counter_followup_dispatch",
        false,
        concat(first_battle_queued_instruction_samples(), first_battle_counter_state_samples()),
        {"actor_slot:3", "target_slot:4"});

    write_checkpoint(
        out,
        "counter_followup_action_80081DE0",
        "80081DE0",
        "counter_followup_action",
        "Battle::AtkMethods::performAttack_80081b94",
        "counter_followup_action",
        false,
        concat(first_battle_queued_instruction_samples(), first_battle_counter_state_samples()),
        {"actor_slot:3", "target_slot:4"});

    write_action_source_selection_checkpoints(out);

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

    write_action_view_selector_query_checkpoints(out);

    return out.str();
}

std::string build_first_battle_action_view_resource_profile_ini()
{
    const auto cache_samples = std0_cache_global_samples();

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_action_view_resource_materialization\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    write_checkpoint(
        out,
        "battle_std_resource_pair_entry_80067E8C",
        "80067E8C",
        "battle_std_resource_pair_entry",
        "STD::LoadBattleCombatantStdResourcePair_80067e8c",
        "std_resource_pair_entry",
        false,
        {},
        {
            "file_dir_arg:3",
            "filename_arg:4",
        },
        std_resource_string_word_samples_from_r4());

    write_checkpoint(
        out,
        "battle_std_root_store_80067FC4",
        "80067FC4",
        "battle_std_root_store",
        "STD::LoadBattleCombatantStdResourcePair_80067e8c",
        "std_root_store",
        false,
        {},
        {
            "copied_std_root:31",
            "loaded_resource:5",
        },
        {
            "loaded_resource_std_root_before_0x30:r5:0x30:u32",
            "copied_root_rows_ptr_0x0c:r31:0x0c:u32",
        });

    write_checkpoint(
        out,
        "battle_std0_materializer_call_80067FD8",
        "80067FD8",
        "battle_std0_materializer_call",
        "STD::LoadBattleCombatantStdResourcePair_80067e8c",
        "std0_materializer_call",
        false,
        {},
        {
            "std0_filename_arg:3",
            "loaded_resource_root_field_ptr:4",
        },
        std_resource_string_word_samples_from_r3(),
        std_root_pointer_samples_from_r4());

    write_checkpoint_owned(
        out,
        "std0_cache_lookup_80035D80",
        "80035D80",
        "std0_cache_lookup",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_cache_lookup",
        false,
        cache_samples,
        {
            "cache_key_expected:4",
            "cache_slot_index:5",
            "cache_key_candidate:0",
            "cache_base:3",
            "root_field_ptr:24",
        });

    write_checkpoint_owned(
        out,
        "std0_cache_table_read_80035DA8",
        "80035DA8",
        "std0_cache_table_read",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_cache_table_read",
        false,
        cache_samples,
        {
            "cache_slot_index:5",
            "cache_table_ptr_before_load:27",
            "root_field_ptr:24",
        });

    write_checkpoint(
        out,
        "std0_cache_result_store_80035E0C",
        "80035E0C",
        "std0_cache_result_store",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_cache_result_store",
        false,
        {},
        {
            "cached_table_ptr:27",
            "root_field_ptr:24",
            "temporary_root_before_free:26",
        },
        {
            "root_field_before_store:r24:0x00:u32",
        });

    write_checkpoint_owned(
        out,
        "std0_transient_handoff_read_80035E34",
        "80035E34",
        "std0_transient_handoff_read",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_transient_handoff_read",
        false,
        cache_samples,
        {
            "source_filename_arg:23",
            "root_field_ptr:24",
            "source_root_before_materialize:27",
        });

    write_checkpoint_owned(
        out,
        "std0_transient_handoff_clear_80035E48",
        "80035E48",
        "std0_transient_handoff_clear",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_transient_handoff_clear",
        false,
        cache_samples,
        {
            "transient_handoff_value:4",
            "selected_loaded_buffer:23",
            "root_field_ptr:24",
        });

    write_checkpoint(
        out,
        "std0_find_loaded_resource_call_80035E54",
        "80035E54",
        "std0_find_loaded_resource_call",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_find_loaded_resource_call",
        false,
        {},
        {
            "source_filename_arg:3",
            "root_field_ptr:24",
        },
        std_resource_string_word_samples_from_r3());

    write_checkpoint(
        out,
        "std0_materialized_result_store_80035FA0",
        "80035FA0",
        "std0_materialized_result_store",
        "STD::LoadStd0EntryTable_80035d4c",
        "std0_materialized_result_store",
        false,
        {},
        {
            "materialized_table_ptr:30",
            "root_field_ptr:24",
            "loaded_source_buffer:25",
        },
        {
            "root_field_before_store:r24:0x00:u32",
            "materialized_prefix_location_0x00:r30:0x00:u16",
            "materialized_prefix_opcode_0x02:r30:0x02:u16",
            "materialized_rows_ptr_0x0c:r30:0x0c:u32",
        });

    write_checkpoint_owned(
        out,
        "std0_cache_producer_materialize_8006DF8C",
        "8006DF8C",
        "std0_cache_producer_materialize",
        "Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc",
        "std0_cache_producer_materialize",
        false,
        cache_samples,
        {
            "loaded_file_ptr_arg:3",
            "queued_resource_record:30",
            "cache_index_candidate:31",
        });

    write_checkpoint_owned(
        out,
        "std0_cache_producer_table_store_8006DFA4",
        "8006DFA4",
        "std0_cache_producer_table_store",
        "Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc",
        "std0_cache_producer_table_store",
        false,
        cache_samples,
        {
            "materialized_table_ptr:3",
            "cache_index_candidate:31",
        });

    write_checkpoint_owned(
        out,
        "std0_cache_producer_key_store_8006DFC4",
        "8006DFC4",
        "std0_cache_producer_key_store",
        "Battle::Resource::ProcessQueuedBattleResourceFile_8006ddbc",
        "std0_cache_producer_key_store",
        false,
        cache_samples,
        {
            "filename_key:0",
            "cache_index_candidate:31",
        });

    write_action_view_selector_query_checkpoints(out);

    return out.str();
}

std::string build_first_battle_turn_order_validation_profile_ini()
{
    constexpr std::uint32_t kActionQueueBase = 0x80302B48u;
    constexpr std::uint32_t kActionQueueStride = 0x0Cu;
    constexpr std::uint32_t kExecutionOrderBase = 0x803092F4u;
    constexpr int kMaxFirstBattleQueueEntries = 8;

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_turn_order_validation\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    write_checkpoint(
        out,
        "turn_order_priority_jitter_800711F8",
        "800711F8",
        "turn_order_priority_jitter",
        "Battle::setupTurn_80070c18",
        "priority",
        true,
        {},
        {
            "actor_slot:29",
            "queue_count:22",
            "action_queue_base:31",
        });

    write_checkpoint(
        out,
        "turn_order_qsort_call_80071408",
        "80071408",
        "turn_order_qsort_call",
        "Battle::setupTurn_80070c18",
        "qsort_call",
        false,
        {},
        {
            "action_queue_base_arg:3",
            "queued_count:4",
            "qsort_elem_size_arg:5",
            "qsort_comparator_arg:6",
            "queued_count_r29:29",
        });

    for (int i = 0; i < kMaxFirstBattleQueueEntries; ++i) {
        const auto base = kActionQueueBase + static_cast<std::uint32_t>(i) * kActionQueueStride;
        const auto suffix = std::to_string(i);

        write_checkpoint_owned(
            out,
            "turn_order_qsort_input_entry_" + suffix + "_80071408",
            "80071408",
            "turn_order_qsort_input_entry_" + suffix,
            "Battle::setupTurn_80070c18",
            "qsort_input",
            false,
            {
                memory_sample("record_word0", base + 0x0u, "u32"),
                memory_sample("slot", base + 0x0u, "u8"),
                memory_sample("assigned_priority", base + 0x4u, "u32"),
                memory_sample("record_word8", base + 0x8u, "u32"),
            });

        write_checkpoint_owned(
            out,
            "turn_order_qsort_output_entry_" + suffix + "_8007140C",
            "8007140C",
            "turn_order_qsort_output_entry_" + suffix,
            "Battle::setupTurn_80070c18",
            "qsort_output",
            false,
            {
                memory_sample("record_word0", base + 0x0u, "u32"),
                memory_sample("slot", base + 0x0u, "u8"),
                memory_sample("assigned_priority", base + 0x4u, "u32"),
                memory_sample("record_word8", base + 0x8u, "u32"),
            });

        write_checkpoint_owned(
            out,
            "turn_order_execution_order_entry_" + suffix + "_8007154C",
            "8007154C",
            "turn_order_execution_order_entry_" + suffix,
            "Battle::setupTurn_80070c18",
            "execution_order",
            false,
            {
                memory_sample("slot", kExecutionOrderBase + static_cast<std::uint32_t>(i), "u8"),
            });
    }

    return out.str();
}

std::string build_first_battle_field6_watch_profile_ini()
{
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_field6_watchpoints\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    write_static_watchpoint(out, "queued_slot0_instr_param_0x6", 0x8030917Au, "u16", "access", "input_macro");
    write_static_watchpoint(out, "queued_slot1_instr_param_0x6", 0x8030919Au, "u16", "access", "input_macro");
    write_static_watchpoint(out, "queued_slot4_instr_param_0x6", 0x803091FAu, "u16", "access", "input_macro");
    write_static_watchpoint(out, "queued_slot5_instr_param_0x6", 0x8030921Au, "u16", "access", "input_macro");

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
    write_dynamic_watchpoint(
        out,
        "actor_field6_action_view_query_call_8001331C",
        "8001331C",
        "r28",
        "0x6",
        "u16",
        "access",
        "input_macro");

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
    write_dynamic_watchpoint(
        out,
        "actor_field6_action_view_query_result_80013320",
        "80013320",
        "r28",
        "0x6",
        "u16",
        "access",
        "input_macro");

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
    write_dynamic_watchpoint(
        out,
        "actor_field6_action_view_spawn_80013334",
        "80013334",
        "r28",
        "0x6",
        "u16",
        "access",
        "input_macro");

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
    write_dynamic_watchpoint(out, "source_field6_bridge_8006778C", "8006778C", "r29", "0x6", "u16", "access", "input_macro");
    write_dynamic_watchpoint(out, "actor_field6_bridge_8006778C", "8006778C", "r30", "0x6", "u16", "access", "input_macro");

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
            "r29_serialized_command:29",
        },
        sst_case2_action_field6_store_samples());
    write_dynamic_watchpoint(out, "sst_case2_source_field6_8000C4C8", "8000C4C8", "r29", "0x6", "u16", "access", "input_macro");
    write_dynamic_watchpoint(out, "sst_case2_dest_field6_8000C4C8", "8000C4C8", "r3", "0x6", "u16", "access", "input_macro");

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
            "r30_serialized_command:30",
        },
        sst_case8_action_field6_store_samples());
    write_dynamic_watchpoint(out, "sst_case8_source_field6_8000C6E8", "8000C6E8", "r30", "0x0a", "u16", "access", "input_macro");
    write_dynamic_watchpoint(out, "sst_case8_dest_field6_8000C6E8", "8000C6E8", "r3", "0x6", "u16", "access", "input_macro");

    return out.str();
}

std::string build_first_battle_action_view_selector_coverage_profile_ini()
{
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_action_view_selector_coverage\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n\n";

    write_action_view_selector_coverage_checkpoints(out);

    return out.str();
}

std::string build_first_battle_thread_list_profile_ini()
{
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_thread_list_ordering\n";
    out << "schema_version=1\n";
    out << "memory=rng_seed_before:" << hex_u32(addr::AddrRegistry::base(addr::core::RNG_SEED)) << ":u32\n";
    out << "linked_list=" << thread_runner_list_snapshot_sample() << "\n\n";

    write_checkpoint(
        out,
        "thread_runner_entry_800136DC",
        "800136DC",
        "thread_runner_entry",
        "FUN_800136dc",
        "thread_runner_entry",
        false,
        {},
        {
            "thread_arg:3",
        },
        {},
        {},
        {},
        1);

    write_checkpoint(
        out,
        "setup_turn_action_entry_80082134",
        "80082134",
        "setup_turn_action_entry",
        "setupTurnAction_80082134",
        "setup_turn_action",
        false,
        {},
        {
            "actor_slot_arg:3",
        });

    write_checkpoint(
        out,
        "action_view_update_entry_80051264",
        "80051264",
        "action_view_update_entry",
        "UpdateActionViewRecord_80051264",
        "action_view_update",
        false,
        {},
        {
            "action_thread_arg:3",
        });

    write_checkpoint(
        out,
        "action_view_tail_draw_gate_80051320",
        "80051320",
        "action_view_tail_draw_gate",
        "UpdateActionViewRecord_80051264",
        "action_view_tail",
        false,
        {},
        action_view_update_gprs(),
        action_view_worksheet_from_r31_samples());

    write_checkpoint(
        out,
        "action_view_pathing_tail_gate_800514B0",
        "800514B0",
        "action_view_pathing_tail_gate",
        "UpdateActionViewRecord_80051264",
        "action_view_pathing_tail",
        false,
        {},
        action_view_update_gprs(),
        action_view_worksheet_from_r31_samples());

    write_checkpoint(
        out,
        "effect_record_spawn_copy_entry_8003BA08",
        "8003BA08",
        "effect_record_spawn_copy_entry",
        "FUN_8003ba08",
        "effect_record_spawn_copy",
        false,
        {},
        {
            "effect_record_arg:3",
            "parent_action_thread_arg:4",
        },
        {},
        {},
        {},
        8);

    write_checkpoint(
        out,
        "combat_effect_worker_entry_80042B10",
        "80042B10",
        "combat_effect_worker_entry",
        "FUN_80042b10",
        "combat_effect_worker",
        false,
        {},
        {
            "effect_thread_arg:3",
        });

    write_checkpoint(
        out,
        "combat_effect_rng_key_gate_80042EB8",
        "80042EB8",
        "combat_effect_rng_key_gate",
        "FUN_80042b10",
        "combat_effect_key_gate",
        false,
        {},
        {
            "effect_record:29",
            "effect_thread:30",
        },
        combat_effect_burst_samples());

    write_checkpoint(
        out,
        "position_line_score_entry_8001AB60",
        "8001AB60",
        "position_line_score_entry",
        "FUN_8001ab60",
        "position_line_score",
        false,
        {},
        {
            "position_arg0:3",
            "position_arg1:4",
            "position_arg2:5",
        });

    return out.str();
}

std::string build_first_battle_pre_handler_frame_pathing_profile_ini()
{
    const auto memory_samples = first_battle_pre_handler_frame_memory_samples();
    const auto addrprog_samples = first_battle_pre_handler_frame_addrprog_samples();

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_pre_handler_frame_pathing\n";
    out << "schema_version=1\n";
    out << "memory=";
    write_owned_csv(out, memory_samples);
    out << "\n";
    out << "addrprog=";
    write_owned_csv(out, addrprog_samples);
    out << "\n";
    out << "linked_list=" << thread_runner_list_snapshot_sample() << "\n\n";

    write_checkpoint(
        out,
        "setup_action_pc_handler_store_80070A54",
        "80070A54",
        "setup_action_pc_handler_store",
        "Battle::Run::setupAction_800708c0",
        "setup_action_handler_install",
        false,
        {},
        {
            "actor_slot:30",
            "actor_slot_x4:31",
            "movement_worksheet:3",
            "handler_pc:0",
        },
        {
            "selected_movement_worksheet_slot_0x00:r3:0x00:u8",
            "selected_movement_worksheet_flags_0x04:r3:0x04:u32",
            "selected_movement_worksheet_pending_handler_before_0x10:r3:0x10:u32",
        },
        {},
        {},
        8);

    write_checkpoint(
        out,
        "setup_action_enemy_handler_store_80070A74",
        "80070A74",
        "setup_action_enemy_handler_store",
        "Battle::Run::setupAction_800708c0",
        "setup_action_handler_install",
        false,
        {},
        {
            "actor_slot:30",
            "actor_slot_x4:31",
            "movement_worksheet:3",
            "handler_pc:0",
        },
        {
            "selected_movement_worksheet_slot_0x00:r3:0x00:u8",
            "selected_movement_worksheet_flags_0x04:r3:0x04:u32",
            "selected_movement_worksheet_pending_handler_before_0x10:r3:0x10:u32",
        },
        {},
        {},
        8);

    write_checkpoint(
        out,
        "setup_action_before_passive_scheduler_80070B54",
        "80070B54",
        "setup_action_before_passive_scheduler",
        "Battle::Run::setupAction_800708c0",
        "setup_action_before_passive_scheduler",
        false,
        {},
        {
            "actor_slot_arg:3",
            "selected_movement_worksheet:4",
        },
        {
            "selected_movement_worksheet_slot_0x00:r4:0x00:u8",
            "selected_movement_worksheet_flags_after_clear_0x04:r4:0x04:u32",
            "selected_movement_worksheet_pending_handler_0x10:r4:0x10:u32",
        },
        {},
        {},
        8);

    write_checkpoint(
        out,
        "movement_handler_promote_80080244",
        "80080244",
        "movement_handler_promote",
        "FUN_800801a8",
        "movement_handler_promote",
        false,
        {},
        {
            "slot:31",
            "movement_buffer:6",
            "movement_worksheet:3",
            "pending_handler_pc:0",
        },
        {
            "movement_buffer_callback_before_0x00:r6:0x00:u32",
            "movement_buffer_state_0x19:r6:0x19:u8",
            "movement_buffer_order_bits_0x20:r6:0x20:u32",
            "selected_movement_worksheet_slot_0x00:r3:0x00:u8",
            "selected_movement_worksheet_flags_0x04:r3:0x04:u32",
            "selected_movement_worksheet_pending_handler_0x10:r3:0x10:u32",
            "selected_movement_worksheet_cur_grid_x_0x0c:r3:0x0c:u8",
            "selected_movement_worksheet_cur_grid_z_0x0d:r3:0x0d:u8",
            "selected_movement_worksheet_prev_grid_x_0x0e:r3:0x0e:u8",
            "selected_movement_worksheet_prev_grid_z_0x0f:r3:0x0f:u8",
            "selected_movement_worksheet_path_index_0x15:r3:0x15:u8",
            "selected_movement_worksheet_status_0x16:r3:0x16:u8",
        },
        {},
        {},
        20);

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        {},
        {},
        {},
        {},
        {},
        1200,
        0x80080244u);

    write_checkpoint(
        out,
        "movement_commit_entry_8008178C",
        "8008178C",
        "movement_commit_entry",
        "FUN_8008178c",
        "movement_commit_entry",
        false,
        {},
        {
            "movement_wksht:3",
            "next_grid_x:4",
            "next_grid_z:5",
            "slot:6",
        },
        {
            "movement_cur_x_0x0c:r3:0x0c:u8",
            "movement_cur_z_0x0d:r3:0x0d:u8",
            "movement_prev_x_0x0e:r3:0x0e:u8",
            "movement_prev_z_0x0f:r3:0x0f:u8",
            "movement_pending_handler_0x10:r3:0x10:u32",
            "movement_path_index_0x15:r3:0x15:u8",
            "movement_status_0x16:r3:0x16:u8",
        },
        {},
        {},
        160);

    write_checkpoint(
        out,
        "movement_posholder_x_store_800819A0",
        "800819A0",
        "movement_posholder_x_store",
        "FUN_8008178c",
        "movement_posholder_x_store",
        false,
        {},
        {
            "posholder_row:5",
            "slot:31",
            "grid_x:29",
            "grid_z:30",
        },
        {
            "posholder_x_before:r5:0x1c:u32",
            "posholder_z_current:r5:0x20:u32",
        },
        {},
        {},
        160);

    write_checkpoint(
        out,
        "movement_posholder_z_store_800819B8",
        "800819B8",
        "movement_posholder_z_store",
        "FUN_8008178c",
        "movement_posholder_z_store",
        false,
        {},
        {
            "posholder_row:4",
            "slot:31",
            "grid_x:29",
            "grid_z:30",
        },
        {
            "posholder_x_current:r4:0x1c:u32",
            "posholder_z_before:r4:0x20:u32",
        },
        {},
        {},
        160);

    write_checkpoint(
        out,
        "bridge_link_read_8001AD2C",
        "8001AD2C",
        "position_bridge_link_read",
        "FUN_8001ab60",
        "position_bridge_link_read",
        false,
        {},
        {
            "thread:28",
            "combatant_wksht:31",
            "instr_wksht:30",
        },
        {
            "cw_slot_0x00:r31:0x00:u8",
            "cw_cur_x_0x1c:r31:0x1c:u32",
            "cw_cur_y_0x20:r31:0x20:u32",
            "cw_cur_z_0x24:r31:0x24:u32",
            "iw_slot_0x00:r30:0x00:u8",
            "iw_target_0x04:r30:0x04:u8",
            "iw_action_mode_0x06:r30:0x06:u16",
            "iw_linked_thread_0x1dc:r30:0x1dc:u32",
        },
        {},
        {},
        2400);

    write_checkpoint(
        out,
        "bridge_cw_x_store_8001AD54",
        "8001AD54",
        "position_bridge_cw_x_store",
        "FUN_8001ab60",
        "position_bridge_cw_x_store",
        false,
        {},
        {
            "posholder_row:3",
            "linked_slot_times_16:0",
            "thread:28",
            "combatant_wksht:31",
            "instr_wksht:30",
        },
        {
            "posholder_x_bits:r3:0x1c:u32",
            "posholder_z_bits:r3:0x20:u32",
            "cw_cur_x_before:r31:0x1c:u32",
            "cw_cur_y:r31:0x20:u32",
            "cw_cur_z:r31:0x24:u32",
        },
        {},
        {},
        2400);

    write_checkpoint(
        out,
        "bridge_cw_z_store_8001AD5C",
        "8001AD5C",
        "position_bridge_cw_z_store",
        "FUN_8001ab60",
        "position_bridge_cw_z_store",
        false,
        {},
        {
            "posholder_row:3",
            "linked_slot_times_16:0",
            "thread:28",
            "combatant_wksht:31",
            "instr_wksht:30",
        },
        {
            "posholder_x_bits:r3:0x1c:u32",
            "posholder_z_bits:r3:0x20:u32",
            "cw_cur_x_after:r31:0x1c:u32",
            "cw_cur_y:r31:0x20:u32",
            "cw_cur_z_before:r31:0x24:u32",
        },
        {},
        {},
        2400);

    write_checkpoint(
        out,
        "bridge_snapshot_call_8001AD68",
        "8001AD68",
        "position_bridge_snapshot_call",
        "FUN_8001ab60",
        "position_bridge_snapshot_call",
        false,
        {},
        {
            "source_vector:3",
            "dest_vector:4",
            "thread:28",
            "combatant_wksht:31",
            "instr_wksht:30",
        },
        {
            "cw_cur_x_after:r31:0x1c:u32",
            "cw_cur_y:r31:0x20:u32",
            "cw_cur_z_after:r31:0x24:u32",
            "iw_snap_x_before:r30:0xf8:u32",
            "iw_snap_y_before:r30:0xfc:u32",
            "iw_snap_z_before:r30:0x100:u32",
        },
        {},
        {},
        2400);

    write_checkpoint(
        out,
        "pc_handler_entry_80086C68",
        "80086C68",
        "pc_handler_entry",
        "Battle::HandlePCInst_80086c68",
        "pc_handler_entry",
        false,
        {},
        pc_handler_gprs(),
        {},
        {},
        {},
        8);

    write_checkpoint(
        out,
        "enemy_handler_entry_8008B9E0",
        "8008B9E0",
        "enemy_handler_entry",
        "Battle::HandleECInst_8008b9e0",
        "enemy_handler_entry",
        false,
        {},
        enemy_handler_gprs(),
        {},
        {},
        {},
        8);

    return out.str();
}

std::string build_first_battle_float_motion_profile_ini()
{
    const auto memory_samples = first_battle_pre_handler_frame_memory_samples();
    const auto addrprog_samples = first_battle_float_motion_addrprog_samples();
    const auto worker_actor_addrprog = float_motion_worker_actor_addrprog_samples();

    std::vector<std::string_view> worker_actor_addrprog_views;
    worker_actor_addrprog_views.reserve(worker_actor_addrprog.size());
    for (const auto& sample : worker_actor_addrprog) {
        worker_actor_addrprog_views.push_back(sample);
    }

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_float_motion\n";
    out << "schema_version=1\n";
    out << "memory=";
    write_owned_csv(out, memory_samples);
    out << "\n";
    out << "addrprog=";
    write_owned_csv(out, addrprog_samples);
    out << "\n";
    out << "linked_list=" << thread_runner_list_snapshot_sample() << "\n\n";

    write_checkpoint(
        out,
        "movement_handler_promote_80080244",
        "80080244",
        "movement_handler_promote",
        "FUN_800801a8",
        "movement_handler_promote",
        false,
        {},
        {
            "slot:31",
            "movement_buffer:6",
            "movement_worksheet:3",
            "pending_handler_pc:0",
        },
        {
            "movement_buffer_callback_before_0x00:r6:0x00:u32",
            "movement_buffer_state_0x19:r6:0x19:u8",
            "movement_buffer_order_bits_0x20:r6:0x20:u32",
            "selected_movement_worksheet_slot_0x00:r3:0x00:u8",
            "selected_movement_worksheet_pending_handler_0x10:r3:0x10:u32",
            "selected_movement_worksheet_cur_grid_x_0x0c:r3:0x0c:u8",
            "selected_movement_worksheet_cur_grid_z_0x0d:r3:0x0d:u8",
        },
        {},
        {},
        40);

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        {},
        {},
        {},
        {},
        {},
        2400,
        0x80080244u);

    write_checkpoint(
        out,
        "float_motion_setup_alt_speed_call_8001FBEC",
        "8001FBEC",
        "float_motion_setup_alt_speed_call",
        "FUN_8001fabc",
        "float_motion_increment_setup",
        false,
        {},
        {
            "move_increment_dest:3",
            "angle:4",
            "angle_saved:27",
            "combatant_wksht:30",
            "instruction_wksht:31",
        },
        float_motion_instruction_samples_from_r31(),
        {},
        {},
        240);

    write_checkpoint(
        out,
        "float_motion_setup_base_speed_call_8001FC00",
        "8001FC00",
        "float_motion_setup_base_speed_call",
        "FUN_8001fabc",
        "float_motion_increment_setup",
        false,
        {},
        {
            "move_increment_dest:3",
            "angle:4",
            "angle_saved:27",
            "combatant_wksht:30",
            "instruction_wksht:31",
        },
        float_motion_instruction_samples_from_r31(),
        {},
        {},
        240);

    write_checkpoint(
        out,
        "float_motion_setup_after_increment_8001FC04",
        "8001FC04",
        "float_motion_setup_after_increment",
        "FUN_8001fabc",
        "float_motion_increment_ready",
        false,
        {},
        {
            "combatant_wksht:30",
            "instruction_wksht:31",
        },
        float_motion_instruction_samples_from_r31(),
        {},
        {},
        240);

    write_checkpoint(
        out,
        "float_motion_worker_loaded_800506D8",
        "800506D8",
        "float_motion_worker_loaded",
        "FUN_800506b0",
        "float_motion_worker_loaded",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_worker_loaded_samples(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_primary_step_call_8005096C",
        "8005096C",
        "float_motion_primary_step_call",
        "FUN_800506b0",
        "float_motion_step_delta_setup",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_worker_loaded_samples(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_primary_x_store_before_80050984",
        "80050984",
        "float_motion_primary_x_store_before",
        "FUN_800506b0",
        "float_motion_raw_position_store_before",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_primary_x_store_after_80050988",
        "80050988",
        "float_motion_primary_x_store_after",
        "FUN_800506b0",
        "float_motion_raw_position_store_after",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_primary_z_store_before_8005099C",
        "8005099C",
        "float_motion_primary_z_store_before",
        "FUN_800506b0",
        "float_motion_raw_position_store_before",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_primary_z_store_after_800509A0",
        "800509A0",
        "float_motion_primary_z_store_after",
        "FUN_800506b0",
        "float_motion_raw_position_store_after",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_secondary_step_call_800509FC",
        "800509FC",
        "float_motion_secondary_step_call",
        "FUN_800506b0",
        "float_motion_step_delta_setup",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_worker_loaded_samples(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_secondary_x_store_before_80050A14",
        "80050A14",
        "float_motion_secondary_x_store_before",
        "FUN_800506b0",
        "float_motion_raw_position_store_before",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_secondary_x_store_after_80050A18",
        "80050A18",
        "float_motion_secondary_x_store_after",
        "FUN_800506b0",
        "float_motion_raw_position_store_after",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_secondary_z_store_before_80050A2C",
        "80050A2C",
        "float_motion_secondary_z_store_before",
        "FUN_800506b0",
        "float_motion_raw_position_store_before",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    write_checkpoint(
        out,
        "float_motion_secondary_z_store_after_80050A30",
        "80050A30",
        "float_motion_secondary_z_store_after",
        "FUN_800506b0",
        "float_motion_raw_position_store_after",
        false,
        {},
        float_motion_worker_gprs(),
        float_motion_store_samples_from_r3(),
        worker_actor_addrprog_views,
        {},
        2400);

    const auto movement_commit_samples = movement_commit_samples_from_r3();
    write_checkpoint(
        out,
        "movement_commit_entry_8008178C",
        "8008178C",
        "movement_commit_entry",
        "FUN_8008178c",
        "movement_commit_entry",
        false,
        {},
        {
            "movement_wksht:3",
            "next_grid_x:4",
            "next_grid_z:5",
            "slot:6",
        },
        movement_commit_samples,
        {},
        {},
        240);

    write_checkpoint(
        out,
        "position_sync_entry_8001AB60",
        "8001AB60",
        "position_sync_entry",
        "FUN_8001ab60",
        "position_sync_entry",
        false,
        {},
        {
            "thread_arg:3",
        },
        {},
        {},
        {},
        2400);

    return out.str();
}

std::string build_first_battle_move_increment_read_watch_profile_ini()
{
    const auto memory_samples = first_battle_pre_handler_frame_memory_samples();
    const auto addrprog_samples = first_battle_float_motion_addrprog_samples();

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_move_increment_read_watch\n";
    out << "schema_version=1\n";
    out << "memory=";
    write_owned_csv(out, memory_samples);
    out << "\n";
    out << "addrprog=";
    write_owned_csv(out, addrprog_samples);
    out << "\n";
    out << "linked_list=" << thread_runner_list_snapshot_sample() << "\n\n";

    write_dynamic_watchpoint(
        out,
        "move_increment_x_read_after_8001FC04",
        "8001FC04",
        "r31",
        "0x104",
        "u32",
        "read",
        "normal");
    write_dynamic_watchpoint(
        out,
        "move_increment_y_read_after_8001FC04",
        "8001FC04",
        "r31",
        "0x108",
        "u32",
        "read",
        "normal");
    write_dynamic_watchpoint(
        out,
        "move_increment_z_read_after_8001FC04",
        "8001FC04",
        "r31",
        "0x10c",
        "u32",
        "read",
        "normal");

    write_checkpoint(
        out,
        "movement_handler_promote_80080244",
        "80080244",
        "movement_handler_promote",
        "FUN_800801a8",
        "movement_handler_promote",
        false,
        {},
        {
            "slot:31",
            "movement_buffer:6",
            "movement_worksheet:3",
            "pending_handler_pc:0",
        },
        {
            "movement_buffer_callback_before_0x00:r6:0x00:u32",
            "movement_buffer_state_0x19:r6:0x19:u8",
            "movement_buffer_order_bits_0x20:r6:0x20:u32",
            "selected_movement_worksheet_slot_0x00:r3:0x00:u8",
            "selected_movement_worksheet_pending_handler_0x10:r3:0x10:u32",
            "selected_movement_worksheet_cur_grid_x_0x0c:r3:0x0c:u8",
            "selected_movement_worksheet_cur_grid_z_0x0d:r3:0x0d:u8",
        },
        {},
        {},
        80);

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        {},
        {},
        {},
        {},
        {},
        2400,
        0x8001FC04u);

    write_checkpoint(
        out,
        "float_motion_setup_after_increment_8001FC04",
        "8001FC04",
        "float_motion_setup_after_increment",
        "FUN_8001fabc",
        "move_increment_watch_armed",
        false,
        {},
        {
            "combatant_wksht:30",
            "instruction_wksht:31",
        },
        float_motion_instruction_samples_from_r31(),
        {},
        {},
        240);

    write_checkpoint(
        out,
        "movement_commit_entry_8008178C",
        "8008178C",
        "movement_commit_entry",
        "FUN_8008178c",
        "movement_commit_entry",
        false,
        {},
        {
            "movement_wksht:3",
            "next_grid_x:4",
            "next_grid_z:5",
            "slot:6",
        },
        {
            "movement_cur_x_0x0c:r3:0x0c:u8",
            "movement_cur_z_0x0d:r3:0x0d:u8",
            "movement_pending_handler_0x10:r3:0x10:u32",
            "movement_path_index_0x15:r3:0x15:u8",
            "movement_status_0x16:r3:0x16:u8",
        },
        {},
        {},
        400);

    write_checkpoint(
        out,
        "position_sync_entry_8001AB60",
        "8001AB60",
        "position_sync_entry",
        "FUN_8001ab60",
        "position_sync_entry",
        false,
        {},
        {
            "thread_arg:3",
        },
        {},
        {},
        {},
        2400);

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

int write_first_battle_predictor_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-predictor-validation-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_predictor_validation_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle predictor-validation capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_turn_order_validation_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-turn-order-validation-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_turn_order_validation_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle turn-order validation capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_field6_watch_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-field6-watch-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_field6_watch_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle field6 watch capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_action_view_resource_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-action-view-resource-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_action_view_resource_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle action-view resource capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_action_view_selector_coverage_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-action-view-selector-coverage-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_action_view_selector_coverage_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle action-view selector coverage capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_thread_list_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-thread-list-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_thread_list_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle thread-list capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_pre_handler_frame_pathing_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-pre-handler-frame-pathing-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_pre_handler_frame_pathing_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle pre-handler frame pathing capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_float_motion_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-float-motion-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_float_motion_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle float-motion capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_move_increment_read_watch_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-move-increment-read-watch-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_move_increment_read_watch_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle move-increment read-watch capture profile: "
        << output_path.string() << "\n";
    return 0;
}

} // namespace savor::predict
