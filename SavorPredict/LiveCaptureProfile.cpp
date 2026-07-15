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
    std::string_view scope = {},
    bool owns_rng_draw = false)
{
    out << "[watchpoint." << id << "]\n";
    out << "address=" << hex_u32(address) << "\n";
    out << "size=" << size << "\n";
    out << "access=" << access << "\n";
    if (owns_rng_draw) {
        out << "owns_rng_draw=true\n";
    }
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

void write_dynamic_addrprog_watchpoint(
    std::ostringstream& out,
    std::string_view id,
    std::string_view pc,
    std::string_view addrprog,
    std::string_view size,
    std::string_view access,
    std::string_view scope = {})
{
    out << "[dynamic_watchpoint." << id << "]\n";
    out << "pc=0x" << pc << "\n";
    out << "addrprog=" << addrprog << "\n";
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

std::string view_placement_frame_thread_list_snapshot_sample(std::uint32_t max_nodes)
{
    return "thread_list:head_ptr=0x80311A84,next=0x04,max=" + std::to_string(max_nodes) + ","
        "fields=callback@0x00:u32|next@0x04:u32|parent@0x08:u32|"
        "flags@0x18:u8|depth@0x1b:u8|order_bits@0x20:u32|payload_word@0x24:u32";
}

std::string pc_worker_thread_list_snapshot_sample(std::uint32_t max_nodes)
{
    return "thread_list:head_ptr=0x80311A84,next=0x04,max=" + std::to_string(max_nodes) + ","
        "fields=callback@0x00:u32|next@0x04:u32|parent@0x08:u32|"
        "flags@0x18:u8|state@0x19:u8|depth@0x1b:u8|"
        "order_bits@0x20:u32|payload_word@0x24:u32";
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

std::vector<std::string> action_view_service_lifecycle_memory_samples()
{
    std::vector<std::string> samples = {
        memory_sample("rng_seed_before", addr::AddrRegistry::base(addr::core::RNG_SEED), "u32"),
        "turn_phase_8034733c:0x8034733C:u32",
        "battle_input_state_80347338:0x80347338:u32",
        "active_actor_slot_80347334:0x80347334:u8",
        "action_sequence_80347335:0x80347335:u8",
        "global_camera_override_80347394:0x80347394:u32",
        "global_camera_flags_803472F4:0x803472F4:u32",
        "view_interrupt_flags_80309F10:0x80309F10:u32",
        "battle_child_count_8030A2D0:0x8030A2D0:u32",
        "battle_service_flags_8030A2D8:0x8030A2D8:u32",
        "battle_root_thread_803475A8:0x803475A8:u32",
    };
    for (int slot = 0; slot < 12; ++slot) {
        samples.push_back(memory_sample(
            "slot" + std::to_string(slot) + "_combatant_thread_ptr",
            0x80309E24u + static_cast<std::uint32_t>(slot) * 4u,
            "u32"));
    }
    return samples;
}

std::vector<std::string> action_view_service_lifecycle_slot_addrprog_samples()
{
    std::vector<std::string> samples;
    samples.reserve(12 * 13);
    for (int slot = 0; slot < 12; ++slot) {
        const auto prefix = "slot" + std::to_string(slot);
        const auto root = hex_u32(0x80309E24u + static_cast<std::uint32_t>(slot) * 4u);
        const auto thread = root + ":load_ptr32";
        const auto worksheet = thread + "|+0x24|load_ptr32";
        const auto instruction = worksheet + "|+0x4c|load_ptr32";
        samples.push_back(prefix + "_thread_callback_0x00:" + thread + "|+0x00:u32");
        samples.push_back(prefix + "_thread_state_0x19:" + thread + "|+0x19:u8");
        samples.push_back(prefix + "_thread_flags_0x18:" + thread + "|+0x18:u8");
        samples.push_back(prefix + "_iw_slot_0x00:" + instruction + "|+0x00:u8");
        samples.push_back(prefix + "_iw_target_0x04:" + instruction + "|+0x04:u8");
        samples.push_back(prefix + "_iw_action_mode_0x06:" + instruction + "|+0x06:u16");
        samples.push_back(prefix + "_iw_action_subtype_0x08:" + instruction + "|+0x08:u16");
        samples.push_back(prefix + "_iw_owner_gate_0x50:" + instruction + "|+0x50:u32");
        samples.push_back(prefix + "_iw_service_slot_0x5a:" + instruction + "|+0x5a:u8");
        samples.push_back(prefix + "_iw_flags_0xec:" + instruction + "|+0xec:u32");
        samples.push_back(prefix + "_iw_flags_0xf0:" + instruction + "|+0xf0:u32");
        samples.push_back(prefix + "_iw_resource_ptr_0x1dc:" + instruction + "|+0x1dc:u32");
        samples.push_back(prefix + "_iw_visual_timer_0x164:" + instruction + "|+0x164:u32");
    }
    return samples;
}

std::vector<std::string> action_view_pathing_loop_slot_addrprog_samples()
{
    std::vector<std::string> samples;
    samples.reserve(12 * 13);
    for (int slot = 0; slot < 12; ++slot) {
        const auto prefix = "slot" + std::to_string(slot);
        const auto root = hex_u32(0x80309E24u + static_cast<std::uint32_t>(slot) * 4u);
        const auto thread = root + ":load_ptr32";
        const auto worksheet = thread + "|+0x24|load_ptr32";
        const auto instruction = worksheet + "|+0x4c|load_ptr32";
        samples.push_back(prefix + "_thread_callback_0x00:" + thread + "|+0x00:u32");
        samples.push_back(prefix + "_cw_cur_x_0x1c:" + worksheet + "|+0x1c:u32");
        samples.push_back(prefix + "_cw_cur_y_0x20:" + worksheet + "|+0x20:u32");
        samples.push_back(prefix + "_cw_cur_z_0x24:" + worksheet + "|+0x24:u32");
        samples.push_back(prefix + "_iw_slot_0x00:" + instruction + "|+0x00:u8");
        samples.push_back(prefix + "_iw_target_0x04:" + instruction + "|+0x04:u8");
        samples.push_back(prefix + "_iw_action_mode_0x06:" + instruction + "|+0x06:u16");
        samples.push_back(prefix + "_iw_flags_0xec:" + instruction + "|+0xec:u32");
        samples.push_back(prefix + "_iw_flags_0xf0:" + instruction + "|+0xf0:u32");
        samples.push_back(prefix + "_iw_extent_0x15c:" + instruction + "|+0x15c:u32");
    }
    return samples;
}

std::vector<std::string> thread_pathing_timing_memory_samples()
{
    auto samples = action_view_service_lifecycle_memory_samples();
    samples.emplace_back("thread_runner_previous_80311A78:0x80311A78:u32");
    samples.emplace_back("thread_runner_current_80311A7C:0x80311A7C:u32");
    samples.emplace_back("movement_completion_override_80347348:0x80347348:u32");
    samples.emplace_back("movement_completion_mask_80347374:0x80347374:u16");
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto slot_offset = static_cast<std::uint32_t>(slot);
        samples.push_back(memory_sample(
            "slot" + slot_text + "_movement_thread_ptr",
            0x80309700u + slot_offset * 4u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_posholder_x_bits",
            0x8030980Cu + slot_offset * 0x10u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_posholder_z_bits",
            0x80309810u + slot_offset * 0x10u,
            "u32"));
    }
    return samples;
}

std::vector<std::string> thread_pathing_timing_state_addrprog_samples()
{
    auto samples = action_view_pathing_loop_slot_addrprog_samples();
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto combatant_root =
            hex_u32(0x80309E24u + static_cast<std::uint32_t>(slot) * 4u);
        const auto combatant_thread = combatant_root + ":load_ptr32";
        const auto combatant_worksheet = combatant_thread + "|+0x24|load_ptr32";
        const auto instruction = combatant_worksheet + "|+0x4c|load_ptr32";
        const auto movement_root =
            hex_u32(0x80309700u + static_cast<std::uint32_t>(slot) * 4u);
        const auto movement_thread = movement_root + ":load_ptr32";
        const auto movement_worksheet = movement_thread + "|+0x24|load_ptr32";
        const auto prefix = "slot" + slot_text;

        samples.push_back(prefix + "_cw_facing_0x2c:"
            + combatant_worksheet + "|+0x2c:u32");
        samples.push_back(prefix + "_iw_move_inc_x_0x104:"
            + instruction + "|+0x104:u32");
        samples.push_back(prefix + "_iw_move_inc_y_0x108:"
            + instruction + "|+0x108:u32");
        samples.push_back(prefix + "_iw_move_inc_z_0x10c:"
            + instruction + "|+0x10c:u32");
        samples.push_back(prefix + "_iw_target_x_0x110:"
            + instruction + "|+0x110:u32");
        samples.push_back(prefix + "_iw_target_y_0x114:"
            + instruction + "|+0x114:u32");
        samples.push_back(prefix + "_iw_target_z_0x118:"
            + instruction + "|+0x118:u32");
        samples.push_back(prefix + "_iw_turn_current_0x11c:"
            + instruction + "|+0x11c:u32");
        samples.push_back(prefix + "_iw_turn_target_0x120:"
            + instruction + "|+0x120:u32");

        samples.push_back(prefix + "_movement_callback_0x00:"
            + movement_thread + "|+0x00:u32");
        samples.push_back(prefix + "_movement_thread_flags_0x18:"
            + movement_thread + "|+0x18:u8");
        samples.push_back(prefix + "_movement_thread_state_0x19:"
            + movement_thread + "|+0x19:u8");
        samples.push_back(prefix + "_movement_order_bits_0x20:"
            + movement_thread + "|+0x20:u32");
        samples.push_back(prefix + "_movement_worksheet_ptr:"
            + movement_thread + "|+0x24:u32");
        samples.push_back(prefix + "_movement_slot_0x00:"
            + movement_worksheet + "|+0x00:u16");
        samples.push_back(prefix + "_movement_flags_0x04:"
            + movement_worksheet + "|+0x04:u32");
        samples.push_back(prefix + "_movement_current_x_0x0c:"
            + movement_worksheet + "|+0x0c:u8");
        samples.push_back(prefix + "_movement_current_z_0x0d:"
            + movement_worksheet + "|+0x0d:u8");
        samples.push_back(prefix + "_movement_previous_x_0x0e:"
            + movement_worksheet + "|+0x0e:u8");
        samples.push_back(prefix + "_movement_previous_z_0x0f:"
            + movement_worksheet + "|+0x0f:u8");
        samples.push_back(prefix + "_movement_pending_handler_0x10:"
            + movement_worksheet + "|+0x10:u32");
        samples.push_back(prefix + "_movement_distance_0x14:"
            + movement_worksheet + "|+0x14:u8");
        samples.push_back(prefix + "_movement_path_index_0x15:"
            + movement_worksheet + "|+0x15:u8");
        samples.push_back(prefix + "_movement_reachability_0x16:"
            + movement_worksheet + "|+0x16:u8");
        for (int node = 0; node < 11; ++node) {
            const auto node_text = std::to_string(node);
            const auto node_offset = static_cast<std::uint32_t>(0x17 + node * 2);
            samples.push_back(prefix + "_movement_path_node" + node_text + "_x:"
                + movement_worksheet + "|+" + hex_u32(node_offset) + ":u8");
            samples.push_back(prefix + "_movement_path_node" + node_text + "_z:"
                + movement_worksheet + "|+" + hex_u32(node_offset + 1) + ":u8");
        }
    }
    return samples;
}

std::vector<std::string> action_view_pathing_turn_worksheet_addrprog_samples(
    std::string_view base_gpr)
{
    const auto base = std::string(base_gpr);
    const auto actor = base + ":+0x74|load_ptr32";
    const auto actor_iw = actor + "|+0x24|load_ptr32|+0x4c|load_ptr32";
    const auto payload = base + ":+0x178|load_ptr32";
    return {
        "turn_camera_position_x_0x20:" + base + ":+0x20:u32",
        "turn_camera_position_y_0x24:" + base + ":+0x24:u32",
        "turn_camera_position_z_0x28:" + base + ":+0x28:u32",
        "turn_camera_angle_x_0x2c:" + base + ":+0x2c:u32",
        "turn_camera_angle_y_0x30:" + base + ":+0x30:u32",
        "turn_camera_angle_z_0x34:" + base + ":+0x34:u32",
        "turn_center_x_0x38:" + base + ":+0x38:u32",
        "turn_center_y_0x3c:" + base + ":+0x3c:u32",
        "turn_center_z_0x40:" + base + ":+0x40:u32",
        "turn_actor_thread_0x74:" + base + ":+0x74:u32",
        "turn_distance_0x88:" + base + ":+0x88:u32",
        "turn_vector_0x8c_x:" + base + ":+0x8c:u32",
        "turn_vector_0x8c_y:" + base + ":+0x90:u32",
        "turn_vector_0x8c_z:" + base + ":+0x94:u32",
        "turn_vector_0x98_x:" + base + ":+0x98:u32",
        "turn_vector_0x98_y:" + base + ":+0x9c:u32",
        "turn_vector_0x98_z:" + base + ":+0xa0:u32",
        "turn_pitch_0xd4:" + base + ":+0xd4:u32",
        "turn_yaw_0xd8:" + base + ":+0xd8:u32",
        "turn_yaw_step_0xdc:" + base + ":+0xdc:u32",
        "turn_path_x_0xf4:" + base + ":+0xf4:u32",
        "turn_path_y_0xf8:" + base + ":+0xf8:u32",
        "turn_path_z_0xfc:" + base + ":+0xfc:u32",
        "turn_saved_center_x_0x100:" + base + ":+0x100:u32",
        "turn_saved_center_y_0x104:" + base + ":+0x104:u32",
        "turn_saved_center_z_0x108:" + base + ":+0x108:u32",
        "turn_control_0x10e:" + base + ":+0x10e:u16",
        "turn_payload_ptr_0x178:" + base + ":+0x178:u32",
        "turn_payload_mode_0x22:" + payload + "|+0x22:u16",
        "turn_actor_slot_0x00:" + actor_iw + "|+0x00:u8",
        "turn_target_slot_0x04:" + actor_iw + "|+0x04:u8",
        "turn_actor_mode_0x06:" + actor_iw + "|+0x06:u16",
    };
}

std::vector<std::string> action_view_controller_addrprog_samples(std::string_view base_gpr)
{
    const auto base = std::string(base_gpr);
    const auto payload = base + ":+0x24|load_ptr32";
    const auto selector = payload + "|+0x6c|load_ptr32";
    return {
        "controller_thread_callback_0x00:" + base + ":+0x00:u32",
        "controller_thread_state_0x19:" + base + ":+0x19:u8",
        "controller_thread_flags_0x18:" + base + ":+0x18:u8",
        "controller_thread_order_bits_0x20:" + base + ":+0x20:u32",
        "controller_payload_ptr_0x24:" + base + ":+0x24:u32",
        "controller_selector_ptr_0x6c:" + payload + "|+0x6c:u32",
        "selector_requested_mode_0x00:" + selector + "|+0x00:u8",
        "selector_previous_mode_0x01:" + selector + "|+0x01:u8",
        "selector_active_slot_0x02:" + selector + "|+0x02:u16",
        "selector_target_slot_0x04:" + selector + "|+0x04:u16",
        "selector_effective_mode_0x2f:" + selector + "|+0x2f:u8",
        "selector_state_0x30:" + selector + "|+0x30:u16",
    };
}

std::vector<std::string> action_view_record_addrprog_samples(std::string_view base_gpr)
{
    const auto base = std::string(base_gpr);
    const auto worksheet = base + ":+0x24|load_ptr32";
    const auto origin = worksheet + "|+0x74|load_ptr32";
    const auto origin_iw = origin + "|+0x24|load_ptr32|+0x4c|load_ptr32";
    const auto payload = worksheet + "|+0x178|load_ptr32";
    return {
        "record_thread_callback_0x00:" + base + ":+0x00:u32",
        "record_thread_state_0x19:" + base + ":+0x19:u8",
        "record_thread_flags_0x18:" + base + ":+0x18:u8",
        "record_thread_order_bits_0x20:" + base + ":+0x20:u32",
        "record_worksheet_ptr_0x24:" + base + ":+0x24:u32",
        "record_origin_thread_ptr_0x74:" + worksheet + "|+0x74:u32",
        "record_payload_ptr_0x178:" + worksheet + "|+0x178:u32",
        "record_worksheet_flags_0x68:" + worksheet + "|+0x68:u32",
        "record_turn_timer_0x70:" + worksheet + "|+0x70:u16",
        "record_substate_0x10e:" + worksheet + "|+0x10e:u16",
        "record_saved_mode_0x110:" + worksheet + "|+0x110:u16",
        "record_effective_mode_0x112:" + worksheet + "|+0x112:u16",
        "record_origin_callback_0x00:" + origin + "|+0x00:u32",
        "record_origin_iw_slot_0x00:" + origin_iw + "|+0x00:u8",
        "record_origin_iw_target_0x04:" + origin_iw + "|+0x04:u8",
        "record_origin_iw_mode_0x06:" + origin_iw + "|+0x06:u16",
        "record_origin_iw_flags_0xec:" + origin_iw + "|+0xec:u32",
        "record_origin_iw_flags_0xf0:" + origin_iw + "|+0xf0:u32",
        "record_payload_primary_0x00:" + payload + "|+0x00:u16",
        "record_payload_secondary_0x02:" + payload + "|+0x02:u16",
        "record_payload_variant_0x04:" + payload + "|+0x04:u16",
        "record_payload_low_flags_0x06:" + payload + "|+0x06:u16",
        "record_payload_flags_0x10:" + payload + "|+0x10:u32",
        "record_payload_start_frame_0x18:" + payload + "|+0x18:u16",
        "record_payload_end_frame_0x1c:" + payload + "|+0x1c:u16",
        "record_payload_hold_0x1e:" + payload + "|+0x1e:u16",
        "record_payload_step_0x20:" + payload + "|+0x20:u16",
        "record_payload_mode_0x22:" + payload + "|+0x22:u16",
    };
}

std::vector<std::string> action_service_child_addrprog_samples(std::string_view base_gpr)
{
    const auto base = std::string(base_gpr);
    const auto payload = base + ":+0x24|load_ptr32";
    const auto origin = payload + "|+0x08|load_ptr32";
    const auto origin_iw = origin + "|+0x24|load_ptr32|+0x4c|load_ptr32";
    const auto selected_target = payload + "|+0x0c|load_ptr32";
    const auto selected_target_iw = selected_target + "|+0x24|load_ptr32|+0x4c|load_ptr32";
    return {
        "service_thread_callback_0x00:" + base + ":+0x00:u32",
        "service_thread_state_0x19:" + base + ":+0x19:u8",
        "service_thread_flags_0x18:" + base + ":+0x18:u8",
        "service_thread_order_bits_0x20:" + base + ":+0x20:u32",
        "service_payload_ptr_0x24:" + base + ":+0x24:u32",
        "service_derived_mode_0x02:" + payload + "|+0x02:u16",
        "service_derived_subtype_0x04:" + payload + "|+0x04:u16",
        "service_selected_slot_0x06:" + payload + "|+0x06:u16",
        "service_origin_thread_ptr_0x08:" + payload + "|+0x08:u32",
        "service_selected_target_thread_ptr_0x0c:" + payload + "|+0x0c:u32",
        "service_command_mode_0x12:" + payload + "|+0x12:u16",
        "service_command_subtype_0x14:" + payload + "|+0x14:u16",
        "service_sync_flags_0x16:" + payload + "|+0x16:u16",
        "service_flags_0x20:" + payload + "|+0x20:u32",
        "service_delay_0x24:" + payload + "|+0x24:u16",
        "service_forced_mode_0x26:" + payload + "|+0x26:u16",
        "service_origin_callback_0x00:" + origin + "|+0x00:u32",
        "service_origin_iw_slot_0x00:" + origin_iw + "|+0x00:u8",
        "service_origin_iw_target_0x04:" + origin_iw + "|+0x04:u8",
        "service_origin_iw_mode_0x06:" + origin_iw + "|+0x06:u16",
        "service_origin_iw_owner_gate_0x50:" + origin_iw + "|+0x50:u32",
        "service_origin_iw_flags_0xec:" + origin_iw + "|+0xec:u32",
        "service_selected_target_iw_slot_0x00:" + selected_target_iw + "|+0x00:u8",
        "service_selected_target_iw_mode_0x06:" + selected_target_iw + "|+0x06:u16",
    };
}

std::vector<std::string> view_eligibility_candidate_row_addrprog_samples()
{
    constexpr int kRowsToSample = 32;
    constexpr int kWordsPerRow = 7;

    std::vector<std::string> samples;
    samples.reserve(kRowsToSample * kWordsPerRow);
    const auto hex_offset = [](int value) {
        std::ostringstream out;
        out << std::uppercase << std::hex << value;
        return out.str();
    };
    for (int row = 0; row < kRowsToSample; ++row) {
        std::ostringstream row_prefix;
        row_prefix << "candidate_row" << std::setw(2) << std::setfill('0') << row;
        const auto base = row * 0x1c;
        samples.push_back(row_prefix.str() + "_id:0x80347398:load_ptr32|+0x"
            + hex_offset(base) + ":u32");
        samples.push_back(row_prefix.str() + "_loaded_ptr:0x80347398:load_ptr32|+0x"
            + hex_offset(base + 4) + ":u32");
        for (int offset = 8; offset < 0x1c; offset += 4) {
            std::ostringstream name;
            name << row_prefix.str() << "_word" << std::uppercase << std::hex << std::setw(2)
                << std::setfill('0') << offset;
            samples.push_back(name.str() + ":0x80347398:load_ptr32|+0x"
                + hex_offset(base + offset) + ":u32");
        }
    }
    return samples;
}

std::vector<std::string> view_eligibility_slot_status_addrprog_samples()
{
    std::vector<std::string> samples;
    samples.reserve(12 * 3);
    for (int slot = 0; slot < 12; ++slot) {
        const auto root = hex_u32(0x80302AB0u + static_cast<std::uint32_t>(slot) * 4u);
        const auto prefix = "slot" + std::to_string(slot) + "_eligibility";
        samples.push_back(prefix + "_flags_0x0:" + root + ":load_ptr32|+0x0:u32");
        samples.push_back(prefix + "_field1_0x4:" + root + ":load_ptr32|+0x4:u32");
        samples.push_back(prefix + "_status_0x44:" + root + ":load_ptr32|+0x44:u16");
    }
    return samples;
}

std::vector<std::string> view_eligibility_state_addrprog_samples()
{
    auto samples = view_eligibility_candidate_row_addrprog_samples();
    auto slot_samples = view_eligibility_slot_status_addrprog_samples();
    samples.insert(samples.end(), slot_samples.begin(), slot_samples.end());
    return samples;
}

std::vector<std::string> view_eligibility_root_memory_samples()
{
    std::vector<std::string> samples = {
        "rng_seed_current_803469A8:0x803469A8:u32",
        "candidate_resource_list_root_80347398:0x80347398:u32",
        "resource_reference_list_root_8034739C:0x8034739C:u32",
        "view_placement_flags_80309F10:0x80309F10:u32",
    };
    samples.reserve(samples.size() + 12);
    for (int slot = 0; slot < 12; ++slot) {
        std::ostringstream name;
        name << "slot" << slot << "_combatant_root_80302A" << std::uppercase << std::hex
            << std::setw(2) << std::setfill('0') << 0xb0 + slot * 4;
        samples.push_back(memory_sample(
            name.str(),
            0x80302AB0u + static_cast<std::uint32_t>(slot) * 4u,
            "u32"));
    }
    return samples;
}

std::string view_eligibility_resource_reference_list_snapshot()
{
    return "resource_reference_list:head_ptr=0x8034739C,next=0x10,max=64,"
        "fields=id@0x00:u32|active_count@0x04:u32|next@0x10:u32";
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
    const std::vector<std::string_view>& action_view_chain_sample_views,
    std::uint32_t max_hits = 0)
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
            action_view_chain_sample_views,
            {},
            max_hits);
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

void write_action_view_selector_query_only_checkpoints(
    std::ostringstream& out,
    std::uint32_t max_hits = 0)
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
        selector_query_sample_views,
        {},
        max_hits);

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
        action_view_chain_sample_views,
        {},
        max_hits);

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
        selector_query_sample_views,
        {},
        max_hits);

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
        action_view_chain_sample_views,
        {},
        max_hits);

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
        selector_query_sample_views,
        {},
        max_hits);

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
        action_view_chain_sample_views,
        {},
        max_hits);
}

void write_action_view_selector_query_checkpoints(std::ostringstream& out)
{
    write_action_view_selector_query_only_checkpoints(out);

    const auto action_view_chain_samples = action_view_chain_addrprog_samples();
    const auto action_view_chain_sample_views = as_string_views(action_view_chain_samples);
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

void write_action_view_selector_coverage_checkpoints(
    std::ostringstream& out,
    std::uint32_t max_hits = 0)
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
                chain_sample_views,
                {},
                max_hits);
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

    write_action_view_selector_helper_checkpoints(out, chain_sample_views, max_hits);
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
        "movement_path_node8_x_0x27:r3:0x27:u8",
        "movement_path_node8_z_0x28:r3:0x28:u8",
        "movement_path_node9_x_0x29:r3:0x29:u8",
        "movement_path_node9_z_0x2a:r3:0x2a:u8",
        "movement_path_node10_x_0x2b:r3:0x2b:u8",
        "movement_path_node10_z_0x2c:r3:0x2c:u8",
        "movement_path_terminator_candidate_0x2d:r3:0x2d:u8",
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
    std::string_view owner,
    bool owns_rng_draw)
{
    if (pc == "800513D4") {
        write_checkpoint(
            out,
            section_id(std::string(owner), std::string(pc)),
            pc,
            owner,
            "UpdateActionViewRecord",
            owner,
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw,
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
            owns_rng_draw);
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

std::vector<std::string> view_placement_cache_memory_samples()
{
    std::vector<std::string> samples;
    samples.push_back(memory_sample(
        "rng_seed_current",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32"));
    samples.emplace_back("view_interrupt_flags_80309F10:0x80309F10:u32");
    samples.emplace_back("view_cache_center_x_bits_80309F88:0x80309F88:u32");
    samples.emplace_back("view_cache_center_y_bits_80309F8C:0x80309F8C:u32");
    samples.emplace_back("view_cache_center_z_bits_80309F90:0x80309F90:u32");
    samples.emplace_back("view_cache_distance_bits_80309FD8:0x80309FD8:u32");
    samples.emplace_back("view_cache_angle_bits_8030A028:0x8030A028:u32");
    samples.emplace_back("view_cache_control_8030A062:0x8030A062:u16");

    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        samples.push_back(memory_sample(
            "slot" + slot_text + "_combatant_thread_ptr",
            0x80309E24u + static_cast<std::uint32_t>(slot) * 4u,
            "u32"));
    }
    return samples;
}

std::vector<std::string> view_placement_frame_thread_memory_samples()
{
    std::vector<std::string> samples;
    samples.push_back(memory_sample(
        "rng_seed_current",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32"));
    samples.emplace_back("turn_phase_8034733c:0x8034733C:u32");
    samples.emplace_back("battle_input_state_80347338:0x80347338:u32");
    samples.emplace_back("active_actor_slot_80347334:0x80347334:u8");
    samples.emplace_back("action_sequence_80347335:0x80347335:u8");
    samples.emplace_back("view_interrupt_flags_80309F10:0x80309F10:u32");
    samples.emplace_back("view_cache_center_x_bits_80309F88:0x80309F88:u32");
    samples.emplace_back("view_cache_center_y_bits_80309F8C:0x80309F8C:u32");
    samples.emplace_back("view_cache_center_z_bits_80309F90:0x80309F90:u32");
    samples.emplace_back("view_cache_distance_bits_80309FD8:0x80309FD8:u32");
    samples.emplace_back("view_cache_angle_bits_8030A028:0x8030A028:u32");
    samples.emplace_back("view_cache_control_8030A062:0x8030A062:u16");
    return samples;
}

std::vector<std::string> view_placement_cache_addrprog_samples()
{
    std::vector<std::string> samples;
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto thread_base = hex_u32(0x80309E24u + static_cast<std::uint32_t>(slot) * 4u);
        const auto prefix = "slot" + slot_text;
        const auto thread_chain = thread_base + ":load_ptr32";
        const auto combatant_chain = thread_chain + "|+0x24|load_ptr32";
        const auto instruction_chain = combatant_chain + "|+0x4c|load_ptr32";

        samples.push_back(prefix + "_combatant_worksheet_ptr:" + thread_chain + "|+0x24:u32");
        samples.push_back(prefix + "_instruction_worksheet_ptr:" + combatant_chain + "|+0x4c:u32");
        samples.push_back(prefix + "_cw_current_x_0x1c:" + combatant_chain + "|+0x1c:u32");
        samples.push_back(prefix + "_cw_current_y_0x20:" + combatant_chain + "|+0x20:u32");
        samples.push_back(prefix + "_cw_current_z_0x24:" + combatant_chain + "|+0x24:u32");
        samples.push_back(prefix + "_iw_geometry_flags_0xec:" + instruction_chain + "|+0xec:u32");
        samples.push_back(prefix + "_iw_saved_x_0x13c:" + instruction_chain + "|+0x13c:u32");
        samples.push_back(prefix + "_iw_saved_y_0x140:" + instruction_chain + "|+0x140:u32");
        samples.push_back(prefix + "_iw_saved_z_0x144:" + instruction_chain + "|+0x144:u32");
        samples.push_back(prefix + "_iw_geometry_extent_0x15c:" + instruction_chain + "|+0x15c:u32");
    }
    return samples;
}

std::vector<std::string> battle_thread_root_motion_addrprog_samples()
{
    std::vector<std::string> samples;
    for (int root_index = 0; root_index < 12; ++root_index) {
        const auto root_text = std::to_string(root_index);
        const auto thread_base = hex_u32(
            0x80309E24u + static_cast<std::uint32_t>(root_index) * 4u);
        const auto prefix = "root" + root_text;
        const auto thread_chain = thread_base + ":load_ptr32";
        const auto combatant_chain = thread_chain + "|+0x24|load_ptr32";
        const auto instruction_chain = combatant_chain + "|+0x4c|load_ptr32";

        samples.push_back(prefix + "_combatant_worksheet_ptr:" + thread_chain + "|+0x24:u32");
        samples.push_back(prefix + "_cw_owner_0x00:" + combatant_chain + "|+0x00:u32");
        samples.push_back(prefix + "_cw_cur_x_0x1c:" + combatant_chain + "|+0x1c:u32");
        samples.push_back(prefix + "_cw_cur_y_0x20:" + combatant_chain + "|+0x20:u32");
        samples.push_back(prefix + "_cw_cur_z_0x24:" + combatant_chain + "|+0x24:u32");
        samples.push_back(prefix + "_cw_facing_angle_0x2c:" + combatant_chain + "|+0x2c:u32");
        samples.push_back(prefix + "_iw_slot_0x00:" + instruction_chain + "|+0x00:u8");
        samples.push_back(prefix + "_iw_action_mode_0x06:" + instruction_chain + "|+0x06:u16");
        samples.push_back(prefix + "_iw_move_inc_x_0x104:" + instruction_chain + "|+0x104:u32");
        samples.push_back(prefix + "_iw_move_inc_y_0x108:" + instruction_chain + "|+0x108:u32");
        samples.push_back(prefix + "_iw_move_inc_z_0x10c:" + instruction_chain + "|+0x10c:u32");
        samples.push_back(prefix + "_iw_target_x_0x110:" + instruction_chain + "|+0x110:u32");
        samples.push_back(prefix + "_iw_target_y_0x114:" + instruction_chain + "|+0x114:u32");
        samples.push_back(prefix + "_iw_target_z_0x118:" + instruction_chain + "|+0x118:u32");
        samples.push_back(prefix + "_iw_speed_0x12c:" + instruction_chain + "|+0x12c:u32");
        samples.push_back(prefix + "_iw_alt_speed_0x130:" + instruction_chain + "|+0x130:u32");
    }
    return samples;
}

std::vector<std::string> movement_destination_stop_memory_samples()
{
    std::vector<std::string> samples;
    samples.emplace_back("turn_phase_8034733c:0x8034733C:u32");
    samples.emplace_back("movement_completion_override_80347348:0x80347348:u32");
    samples.emplace_back("movement_completion_mask_80347374:0x80347374:u16");
    samples.emplace_back("battle_input_state_80347338:0x80347338:u32");
    samples.emplace_back("active_actor_slot_80347334:0x80347334:u8");
    samples.emplace_back("action_sequence_80347335:0x80347335:u8");
    samples.emplace_back("base_grid_ptr_80347350:0x80347350:u32");
    samples.emplace_back("active_grid_ptr_80347354:0x80347354:u32");
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto slot_offset = static_cast<std::uint32_t>(slot);
        samples.push_back(memory_sample(
            "slot" + slot_text + "_posholder_x_bits",
            0x8030980Cu + slot_offset * 0x10u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_posholder_z_bits",
            0x80309810u + slot_offset * 0x10u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_controller_state",
            0x80309730u + slot_offset * 0x10u,
            "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_controller_byte_0x09",
            0x80309739u + slot_offset * 0x10u,
            "u8"));
    }
    return samples;
}

std::vector<std::string> movement_destination_stop_grid_addrprog_samples()
{
    std::vector<std::string> samples;
    samples.reserve(242);
    for (std::uint32_t index = 0; index < 121; ++index) {
        const auto suffix = std::to_string(index);
        const auto offset = hex_u32(index);
        samples.push_back(
            "base_grid_" + suffix + ":0x80347350:load_ptr32|+" + offset + ":u8");
        samples.push_back(
            "active_grid_" + suffix + ":0x80347354:load_ptr32|+" + offset + ":u8");
    }
    return samples;
}

std::vector<std::string> movement_destination_stop_frame_addrprog_samples()
{
    auto samples = movement_destination_stop_grid_addrprog_samples();
    const auto root_motion = battle_thread_root_motion_addrprog_samples();
    samples.insert(samples.end(), root_motion.begin(), root_motion.end());
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto root = hex_u32(0x80309700u + static_cast<std::uint32_t>(slot) * 4u);
        const auto movement_thread = root + ":load_ptr32";
        const auto worksheet = movement_thread + "|+0x24|load_ptr32";
        const auto instance = worksheet + "|+0x08|load_ptr32";
        const auto prefix = "slot" + slot_text + "_movement";
        samples.push_back(prefix + "_worksheet_ptr:" + movement_thread + "|+0x24:u32");
        samples.push_back(prefix + "_slot_0x00:" + worksheet + "|+0x00:u8");
        samples.push_back(prefix + "_flags_0x04:" + worksheet + "|+0x04:u32");
        samples.push_back(prefix + "_instance_ptr_0x08:" + worksheet + "|+0x08:u32");
        samples.push_back(prefix + "_width_0xac:" + instance + "|+0xac:u8");
        samples.push_back(prefix + "_depth_0xad:" + instance + "|+0xad:u8");
        samples.push_back(prefix + "_movement_flags_0xae:" + instance + "|+0xae:u16");
        samples.push_back(prefix + "_current_x_0x0c:" + worksheet + "|+0x0c:u8");
        samples.push_back(prefix + "_current_z_0x0d:" + worksheet + "|+0x0d:u8");
        samples.push_back(prefix + "_previous_x_0x0e:" + worksheet + "|+0x0e:u8");
        samples.push_back(prefix + "_previous_z_0x0f:" + worksheet + "|+0x0f:u8");
        samples.push_back(prefix + "_distance_0x14:" + worksheet + "|+0x14:u8");
        samples.push_back(prefix + "_path_index_0x15:" + worksheet + "|+0x15:u8");
        samples.push_back(prefix + "_reachability_status_0x16:" + worksheet + "|+0x16:u8");
        for (int node = 0; node < 11; ++node) {
            const auto node_text = std::to_string(node);
            const auto node_offset = static_cast<std::uint32_t>(0x17 + node * 2);
            samples.push_back(prefix + "_path_node" + node_text + "_x:"
                + worksheet + "|+" + hex_u32(node_offset) + ":u8");
            samples.push_back(prefix + "_path_node" + node_text + "_z:"
                + worksheet + "|+" + hex_u32(node_offset + 1) + ":u8");
        }
        samples.push_back(prefix + "_path_terminator_candidate_0x2d:"
            + worksheet + "|+0x2d:u8");
    }
    return samples;
}

std::vector<std::string> pc_worker_selector_memory_samples()
{
    auto samples = movement_destination_stop_memory_samples();
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto queued = 0x80309174u + static_cast<std::uint32_t>(slot) * 0x20u;
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_instruction_0x00", queued, "u32"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_target_0x04", queued + 0x04u, "u8"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_instr_param_0x06", queued + 0x06u, "u16"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_queued_result_0x09", queued + 0x09u, "u8"));
        samples.push_back(memory_sample(
            "slot" + slot_text + "_movement_thread_ptr",
            0x80309700u + static_cast<std::uint32_t>(slot) * 4u,
            "u32"));
    }
    return samples;
}

std::vector<std::string> pc_worker_selector_addrprog_samples()
{
    std::vector<std::string> samples;
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        const auto root = hex_u32(0x80309700u + static_cast<std::uint32_t>(slot) * 4u);
        const auto movement_thread = root + ":load_ptr32";
        const auto worksheet = movement_thread + "|+0x24|load_ptr32";
        const auto instance = worksheet + "|+0x08|load_ptr32";
        const auto prefix = "slot" + slot_text + "_movement";
        samples.push_back(prefix + "_callback_0x00:" + movement_thread + "|+0x00:u32");
        samples.push_back(prefix + "_thread_state_0x19:" + movement_thread + "|+0x19:u8");
        samples.push_back(prefix + "_thread_order_bits_0x20:" + movement_thread + "|+0x20:u32");
        samples.push_back(prefix + "_worksheet_ptr:" + movement_thread + "|+0x24:u32");
        samples.push_back(prefix + "_slot_0x00:" + worksheet + "|+0x00:u16");
        samples.push_back(prefix + "_flags_0x04:" + worksheet + "|+0x04:u32");
        samples.push_back(prefix + "_instance_status_flags_0x1c:"
            + instance + "|+0x1c:u32");
        samples.push_back(prefix + "_current_x_0x0c:" + worksheet + "|+0x0c:u8");
        samples.push_back(prefix + "_current_z_0x0d:" + worksheet + "|+0x0d:u8");
        samples.push_back(prefix + "_previous_x_0x0e:" + worksheet + "|+0x0e:u8");
        samples.push_back(prefix + "_previous_z_0x0f:" + worksheet + "|+0x0f:u8");
        samples.push_back(prefix + "_pending_handler_0x10:" + worksheet + "|+0x10:u32");
        samples.push_back(prefix + "_distance_0x14:" + worksheet + "|+0x14:u8");
        samples.push_back(prefix + "_path_index_0x15:" + worksheet + "|+0x15:u8");
        samples.push_back(prefix + "_reachability_status_0x16:" + worksheet + "|+0x16:u8");
        for (int node = 0; node < 11; ++node) {
            const auto node_text = std::to_string(node);
            const auto node_offset = static_cast<std::uint32_t>(0x17 + node * 2);
            samples.push_back(prefix + "_path_node" + node_text + "_x:"
                + worksheet + "|+" + hex_u32(node_offset) + ":u8");
            samples.push_back(prefix + "_path_node" + node_text + "_z:"
                + worksheet + "|+" + hex_u32(node_offset + 1) + ":u8");
        }
        samples.push_back(prefix + "_path_terminator_candidate_0x2d:"
            + worksheet + "|+0x2d:u8");
    }
    return samples;
}

std::vector<std::string_view> pc_worker_thread_r3_addrprog_samples()
{
    return {
        "worker_thread_callback_0x00:r3:+0x00:u32",
        "worker_thread_flags_0x18:r3:+0x18:u8",
        "worker_thread_state_0x19:r3:+0x19:u8",
        "worker_thread_order_bits_0x20:r3:+0x20:u32",
        "worker_thread_payload_0x24:r3:+0x24:u32",
        "worker_iw_slot_0x00:r3:+0x24|load_ptr32|+0x00:u16",
        "worker_iw_flags_0x04:r3:+0x24|load_ptr32|+0x04:u32",
        "worker_iw_target_0x04:r3:+0x24|load_ptr32|+0x04:u8",
        "worker_iw_current_x_0x0c:r3:+0x24|load_ptr32|+0x0c:u8",
        "worker_iw_current_z_0x0d:r3:+0x24|load_ptr32|+0x0d:u8",
        "worker_iw_distance_0x14:r3:+0x24|load_ptr32|+0x14:u8",
        "worker_iw_path_index_0x15:r3:+0x24|load_ptr32|+0x15:u8",
        "worker_iw_status_0x16:r3:+0x24|load_ptr32|+0x16:u8",
    };
}

std::vector<std::string> movement_scratch_grid_samples_from_r3()
{
    std::vector<std::string> samples;
    samples.reserve(121);
    for (std::uint32_t index = 0; index < 121; ++index) {
        samples.push_back(
            "movement_scratch_grid_" + std::to_string(index)
            + ":r3:" + hex_u32(0x53u + index) + ":u8");
    }
    return samples;
}

std::vector<std::string> view_placement_semantic_memory_samples()
{
    auto samples = view_placement_frame_thread_memory_samples();
    for (int slot = 0; slot < 12; ++slot) {
        const auto slot_text = std::to_string(slot);
        samples.push_back(memory_sample(
            "slot" + slot_text + "_combatant_thread_ptr",
            0x80309E24u + static_cast<std::uint32_t>(slot) * 4u,
            "u32"));
    }
    samples.emplace_back("view_geometry_zero_bits_803481B8:0x803481B8:u32");
    samples.emplace_back("view_geometry_radius_normal_bits_803481BC:0x803481BC:u32");
    samples.emplace_back("view_geometry_negate_bits_803481CC:0x803481CC:u32");
    samples.emplace_back("view_geometry_min_seed_bits_803481D0:0x803481D0:u32");
    samples.emplace_back("view_geometry_max_seed_bits_803481D4:0x803481D4:u32");
    samples.emplace_back("view_geometry_radius_large_bits_803481D8:0x803481D8:u32");
    samples.emplace_back("view_geometry_half_bits_803481DC:0x803481DC:u32");
    samples.emplace_back("view_geometry_distance_scale_bits_80348210:0x80348210:u32");
    return samples;
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
        write_predictor_validation_rng_checkpoint(out, pc, owner, true);
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

std::string build_first_battle_view_eligibility_profile_ini()
{
    const auto root_memory_samples = view_eligibility_root_memory_samples();
    const auto state_addrprog_samples = view_eligibility_state_addrprog_samples();
    const auto state_addrprog_views = as_string_views(state_addrprog_samples);
    const auto resource_reference_list = view_eligibility_resource_reference_list_snapshot();
    const std::vector<std::string_view> resource_reference_list_views = { resource_reference_list };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_view_eligibility\n";
    out << "schema_version=1\n";
    out << "memory=";
    write_owned_csv(out, root_memory_samples);
    out << "\n\n";

    // Dolphin reports the mutated seed for write watchpoints. The reduced analysis
    // treats its decoded value as post-write and relies on the captured call stack.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    write_checkpoint(
        out,
        "view_eligibility_pre_call_800144E8",
        "800144E8",
        "view_eligibility_pre_call",
        "FUN_80014474",
        "view_eligibility_pre_call",
        false,
        {},
        {
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        {
            "view_placement_ctrl_state_0x30:r30:0x30:u16",
            "view_placement_angle_0x08:r30:0x08:u32",
        },
        state_addrprog_views,
        resource_reference_list_views,
        16);

    write_checkpoint(
        out,
        "view_eligibility_gate_entry_8006D1E8",
        "8006D1E8",
        "view_eligibility_gate_entry",
        "FUN_8006d1c4",
        "gate_entry",
        false,
        {},
        {
            "candidate_row:5",
            "reference_list_head:4",
        },
        {
            "candidate_row_id:r5:0x00:u32",
            "candidate_row_loaded_ptr:r5:0x04:u32",
        },
        {},
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_candidate_unloaded_8006D250",
        "8006D250",
        "view_eligibility_candidate_unloaded",
        "FUN_8006d1c4",
        "candidate_unloaded_false_return",
        false,
        {},
        {
            "candidate_row:5",
            "candidate_resource_id:6",
        },
        {},
        {
            "candidate_row_id:r5:+0x00:u32",
            "candidate_row_loaded_ptr:r5:+0x04:u32",
        },
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_reference_active_8006D248",
        "8006D248",
        "view_eligibility_reference_active",
        "FUN_8006d1c4",
        "reference_active_false_return",
        false,
        {},
        {
            "reference_node:3",
            "candidate_row:5",
            "candidate_resource_id:6",
        },
        {},
        {
            "reference_node_id:r3:+0x00:u32",
            "reference_node_active_count:r3:+0x04:u32",
            "reference_node_next:r3:+0x10:u32",
            "candidate_row_id:r5:+0x00:u32",
        },
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_aggregate_return_8006D268",
        "8006D268",
        "view_eligibility_aggregate_return",
        "FUN_8006d1c4",
        "aggregate_count_return",
        false,
        {},
        {
            "aggregate_count_return:3",
        },
        {},
        {},
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_aggregate_nonzero_8006D270",
        "8006D270",
        "view_eligibility_aggregate_nonzero",
        "FUN_8006d1c4",
        "aggregate_nonzero_false_return",
        false,
        {},
        {
            "aggregate_count_return:3",
        },
        {},
        {},
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_slot_status_return_8006D288",
        "8006D288",
        "view_eligibility_slot_status_return",
        "FUN_8006d1c4",
        "slot_status_return",
        false,
        {},
        {
            "slot_index:31",
            "slot_status_return:3",
        },
        {},
        {},
        {},
        96,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_slot_status_three_8006D294",
        "8006D294",
        "view_eligibility_slot_status_three",
        "FUN_8006d1c4",
        "slot_status_three_false_return",
        false,
        {},
        {
            "slot_index:31",
            "slot_status_return:3",
        },
        {},
        {},
        {},
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_gate_return_8006D2B0",
        "8006D2B0",
        "view_eligibility_gate_return",
        "FUN_8006d1c4",
        "gate_return",
        false,
        {},
        {
            "view_eligibility_result:3",
        },
        {},
        state_addrprog_views,
        resource_reference_list_views,
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_eligibility_call_return_800144EC",
        "800144EC",
        "view_eligibility_call_return",
        "FUN_80014474",
        "view_eligibility_call_return",
        false,
        {},
        {
            "view_eligibility_result:3",
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        {
            "view_placement_ctrl_state_0x30:r30:0x30:u16",
        },
        state_addrprog_views,
        resource_reference_list_views,
        16,
        0x800144E8u);

    write_checkpoint(
        out,
        "view_placement_rng_call_800145C8",
        "800145C8",
        "view_placement_rng_call",
        "FUN_80014474",
        "cache_miss_rng_call",
        false,
        {},
        {
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        {
            "view_placement_angle_before_rng_0x08:r30:0x08:u32",
            "view_placement_extent_0x0c:r30:0x0c:u32",
        },
        {},
        {},
        16,
        0x800144E8u);

    return out.str();
}

std::string build_first_battle_view_placement_cache_profile_ini()
{
    const auto memory_samples = view_placement_cache_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto addrprog_samples = view_placement_cache_addrprog_samples();
    const auto addrprog_views = as_string_views(addrprog_samples);

    const std::vector<std::string_view> placement_gprs = {
        "view_placement_ctrl:30",
        "view_placement_record:31",
    };
    const std::vector<std::string_view> placement_locals = {
        "view_placement_angle_0x08:r30:0x08:u32",
        "view_placement_distance_0x0c:r30:0x0c:u32",
        "view_placement_state_0x30:r30:0x30:u16",
        "view_candidate_center_x_0x38:r31:0x38:u32",
        "view_candidate_center_y_0x3c:r31:0x3c:u32",
        "view_candidate_center_z_0x40:r31:0x40:u32",
        "geometry_width_0x08:r1:0x08:u32",
        "geometry_depth_0x0c:r1:0x0c:u32",
        "geometry_center_x_0x10:r1:0x10:u32",
        "geometry_center_y_0x14:r1:0x14:u32",
        "geometry_center_z_0x18:r1:0x18:u32",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_view_placement_cache\n";
    out << "schema_version=1\n";
    out << "memory=";
    write_owned_csv(out, memory_samples);
    out << "\n";
    out << "addrprog=";
    write_owned_csv(out, addrprog_samples);
    out << "\n\n";

    // Dolphin reports the post-write seed. The call stack identifies the RNG
    // caller, so this value must not be interpreted as a pre-write seed.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    write_checkpoint(
        out,
        "view_placement_entry_80014498",
        "80014498",
        "view_placement_entry",
        "FUN_80014474",
        "view_placement",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_readiness_return_800144EC",
        "800144EC",
        "view_placement_readiness_return",
        "FUN_80014474",
        "view_placement_readiness",
        false,
        memory_views,
        {
            "view_eligibility_result:3",
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_geometry_call_80014504",
        "80014504",
        "view_placement_geometry_call",
        "FUN_80014474",
        "view_placement_geometry",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_geometry_result_80014548",
        "80014548",
        "view_placement_geometry_result",
        "FUN_80014474",
        "view_placement_cache_key",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_cache_decision_800145BC",
        "800145BC",
        "view_placement_cache_decision",
        "FUN_80014474",
        "view_placement_cache_decision",
        false,
        memory_views,
        {
            "cache_hit_flag_r0:0",
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_rng_call_800145C8",
        "800145C8",
        "view_placement_rng_call",
        "FUN_80014474",
        "view_placement_cache_miss_rng",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_rng_value_800608E0",
        "800608E0",
        "view_placement_rng_value",
        "FUN_800608c8",
        "view_placement_cache_miss_rng_value",
        false,
        memory_views,
        {
            "rng_rand15_value:3",
            "angle_result_ptr:31",
        },
        {
            "angle_before_mapping_bits:r31:0x00:u32",
        },
        {},
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_rng_angle_selected_8006093C",
        "8006093C",
        "view_placement_rng_angle_selected",
        "FUN_800608c8",
        "view_placement_cache_miss_rng_result",
        false,
        memory_views,
        {
            "angle_result_ptr:31",
        },
        {
            "selected_angle_bits:r31:0x00:u32",
        },
        {},
        {},
        24);

    write_checkpoint(
        out,
        "view_placement_post_selection_800145CC",
        "800145CC",
        "view_placement_post_selection",
        "FUN_80014474",
        "view_placement_post_selection",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_cache_publish_from_14474_80014704",
        "80014704",
        "view_cache_publish_from_14474",
        "FUN_80014474",
        "view_placement_cache_publish",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_cache_publish_from_136DC_800139D8",
        "800139D8",
        "view_cache_publish_from_136dc",
        "FUN_800136dc",
        "view_placement_cache_publish",
        false,
        memory_views,
        {
            "runner_view_placement_ctrl:29",
            "runner_view_placement_record:26",
        },
        {
            "runner_view_angle_0x08:r29:0x08:u32",
            "runner_view_distance_0x0c:r29:0x0c:u32",
            "runner_view_center_x_0x38:r26:0x38:u32",
            "runner_view_center_y_0x3c:r26:0x3c:u32",
            "runner_view_center_z_0x40:r26:0x40:u32",
        },
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_cache_publish_from_121D8_80012530",
        "80012530",
        "view_cache_publish_from_121d8",
        "FUN_800121d8",
        "view_placement_cache_publish",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        addrprog_views,
        {},
        24);

    write_checkpoint(
        out,
        "view_geometry_mld_slot_return_80011544",
        "80011544",
        "view_geometry_mld_slot_return",
        "FUN_800114ac",
        "view_placement_geometry_candidate_gate",
        false,
        memory_views,
        {
            "geometry_slot_index:31",
            "mld_slot_return:3",
            "combatant_thread:25",
            "instruction_worksheet:24",
        },
        {
            "candidate_geometry_flags_0xec:r24:0xec:u32",
            "candidate_extent_0x15c:r24:0x15c:u32",
        },
        {},
        {},
        96);

    return out.str();
}

std::string build_first_battle_view_placement_frame_thread_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    const auto memory_samples = view_placement_frame_thread_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto thread_list_sample = view_placement_frame_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = { thread_list_sample };

    const std::vector<std::string_view> placement_gprs = {
        "view_placement_ctrl:30",
        "view_placement_record:31",
    };
    const std::vector<std::string_view> placement_locals = {
        "view_placement_angle_0x08:r30:0x08:u32",
        "view_placement_distance_0x0c:r30:0x0c:u32",
        "view_placement_state_0x30:r30:0x30:u16",
        "view_candidate_center_x_0x38:r31:0x38:u32",
        "view_candidate_center_y_0x3c:r31:0x3c:u32",
        "view_candidate_center_z_0x40:r31:0x40:u32",
        "geometry_width_0x08:r1:0x08:u32",
        "geometry_depth_0x0c:r1:0x0c:u32",
        "geometry_center_x_0x10:r1:0x10:u32",
        "geometry_center_y_0x14:r1:0x14:u32",
        "geometry_center_z_0x18:r1:0x18:u32",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_view_placement_frame_thread\n";
    out << "schema_version=1\n\n";

    // Dolphin reports the mutated seed for write watchpoints. It is retained
    // only for draw indexing and caller attribution, never as a pre-write value.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);
    write_static_watchpoint(
        out,
        "view_cache_control_write_8030A062",
        0x8030A062u,
        "u16",
        "write",
        "normal");

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        memory_views,
        {},
        {},
        {},
        thread_list_views,
        2400);

    write_checkpoint(
        out,
        "view_placement_entry_80014498",
        "80014498",
        "view_placement_entry",
        "FUN_80014474",
        "view_placement",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_readiness_return_800144EC",
        "800144EC",
        "view_placement_readiness_return",
        "FUN_80014474",
        "view_placement_readiness",
        false,
        memory_views,
        {
            "view_eligibility_result:3",
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        placement_locals,
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_geometry_result_80014548",
        "80014548",
        "view_placement_geometry_result",
        "FUN_80014474",
        "view_placement_cache_key",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_cache_decision_800145BC",
        "800145BC",
        "view_placement_cache_decision",
        "FUN_80014474",
        "view_placement_cache_decision",
        false,
        memory_views,
        {
            "cache_hit_flag_r0:0",
            "view_placement_ctrl:30",
            "view_placement_record:31",
        },
        placement_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_placement_rng_call_800145C8",
        "800145C8",
        "view_placement_rng_call",
        "FUN_80014474",
        "view_placement_cache_miss_rng",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_placement_rng_value_800608E0",
        "800608E0",
        "view_placement_rng_value",
        "FUN_800608c8",
        "view_placement_cache_miss_rng_value",
        false,
        memory_views,
        {
            "rng_rand15_value:3",
            "angle_result_ptr:31",
        },
        {
            "angle_before_mapping_bits:r31:0x00:u32",
        },
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_rng_angle_selected_8006093C",
        "8006093C",
        "view_placement_rng_angle_selected",
        "FUN_800608c8",
        "view_placement_cache_miss_rng_result",
        false,
        memory_views,
        {
            "angle_result_ptr:31",
        },
        {
            "selected_angle_bits:r31:0x00:u32",
        },
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_post_selection_800145CC",
        "800145CC",
        "view_placement_post_selection",
        "FUN_80014474",
        "view_placement_post_selection",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_14474_80014704",
        "80014704",
        "view_cache_publish_from_14474",
        "FUN_80014474",
        "view_placement_cache_publish",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_136DC_800139D8",
        "800139D8",
        "view_cache_publish_from_136dc",
        "FUN_800136dc",
        "view_placement_cache_publish",
        false,
        memory_views,
        {
            "runner_view_thread_context:28",
            "runner_view_placement_ctrl:29",
            "runner_view_placement_record:26",
        },
        {
            "runner_view_angle_0x08:r29:0x08:u32",
            "runner_view_distance_0x0c:r29:0x0c:u32",
            "runner_view_center_x_0x38:r26:0x38:u32",
            "runner_view_center_y_0x3c:r26:0x3c:u32",
            "runner_view_center_z_0x40:r26:0x40:u32",
        },
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_121D8_80012530",
        "80012530",
        "view_cache_publish_from_121d8",
        "FUN_800121d8",
        "view_placement_cache_publish",
        false,
        memory_views,
        placement_gprs,
        placement_locals,
        {},
        thread_list_views,
        512);

    return out.str();
}

static std::string build_first_battle_view_placement_profile_ini(
    std::uint32_t thread_list_max_nodes,
    bool include_predictor_comparison)
{
    const auto memory_samples = view_placement_semantic_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto addrprog_samples = view_placement_cache_addrprog_samples();
    const auto addrprog_views = as_string_views(addrprog_samples);
    auto frame_addrprog_samples = std::vector<std::string>{};
    if (include_predictor_comparison) {
        frame_addrprog_samples = addrprog_samples;
        const auto motion_samples = first_battle_float_motion_addrprog_samples();
        frame_addrprog_samples.insert(
            frame_addrprog_samples.end(),
            motion_samples.begin(),
            motion_samples.end());
        const auto root_motion_samples = battle_thread_root_motion_addrprog_samples();
        frame_addrprog_samples.insert(
            frame_addrprog_samples.end(),
            root_motion_samples.begin(),
            root_motion_samples.end());
    }
    const auto frame_addrprog_views = as_string_views(frame_addrprog_samples);
    const auto thread_list_sample =
        view_placement_frame_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = { thread_list_sample };

    const std::vector<std::string_view> geometry_stack_results = {
        "geometry_half_x_bits:r1:0x08:u32",
        "geometry_half_z_bits:r1:0x0c:u32",
        "geometry_center_x_bits:r1:0x10:u32",
        "geometry_center_y_bits:r1:0x14:u32",
        "geometry_center_z_bits:r1:0x18:u32",
    };
    const std::vector<std::string_view> direct_view_gprs = {
        "direct_view_controller:30",
        "direct_view_record:31",
    };
    const std::vector<std::string_view> direct_view_locals = {
        "direct_view_angle_bits_0x08:r30:0x08:u32",
        "direct_view_distance_bits_0x0c:r30:0x0c:u32",
        "direct_view_state_0x30:r30:0x30:u16",
        "direct_view_center_x_bits_0x38:r31:0x38:u32",
        "direct_view_center_y_bits_0x3c:r31:0x3c:u32",
        "direct_view_center_z_bits_0x40:r31:0x40:u32",
    };
    const std::vector<std::string_view> placement_function_gprs = {
        "placement_function_controller:30",
        "placement_function_record:31",
    };
    const std::vector<std::string_view> placement_function_locals = {
        "placement_function_angle_bits_0x08:r30:0x08:u32",
        "placement_function_distance_bits_0x0c:r30:0x0c:u32",
        "placement_function_state_0x30:r30:0x30:u16",
        "placement_function_center_x_bits_0x38:r31:0x38:u32",
        "placement_function_center_y_bits_0x3c:r31:0x3c:u32",
        "placement_function_center_z_bits_0x40:r31:0x40:u32",
    };
    const std::vector<std::string_view> runner_gprs = {
        "runner_thread_context:28",
        "runner_controller:29",
        "runner_record:26",
    };
    const std::vector<std::string_view> runner_locals = {
        "runner_angle_bits_0x08:r29:0x08:u32",
        "runner_distance_bits_0x0c:r29:0x0c:u32",
        "runner_state_0x30:r29:0x30:u16",
        "runner_center_x_bits_0x38:r26:0x38:u32",
        "runner_center_y_bits_0x3c:r26:0x3c:u32",
        "runner_center_z_bits_0x40:r26:0x40:u32",
        "runner_thread_callback_0x00:r28:0x00:u32",
        "runner_thread_payload_0x24:r28:0x24:u32",
    };
    const std::vector<std::string_view> workspace_copy_source = {
        "workspace_copy_source_center_x_0x38:r29:0x38:u32",
        "workspace_copy_source_center_y_0x3c:r29:0x3c:u32",
        "workspace_copy_source_center_z_0x40:r29:0x40:u32",
        "workspace_copy_source_distance_0x88:r29:0x88:u32",
        "workspace_copy_source_angle_0xd8:r29:0xd8:u32",
        "workspace_copy_source_control_0x112:r29:0x112:u16",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name="
        << (include_predictor_comparison
            ? "first_battle_predictor_live_comparison"
            : "first_battle_view_placement_semantic_hooks")
        << "\n";
    out << "schema_version=1\n\n";

    // Dolphin write watchpoints expose the post-write value. The stack and
    // decoded source instruction provide attribution for both watched fields.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);
    write_static_watchpoint(
        out,
        "view_cache_control_write_8030A062",
        0x8030A062u,
        "u16",
        "write",
        "normal");

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        memory_views,
        {},
        {},
        frame_addrprog_views,
        thread_list_views,
        2400);

    if (include_predictor_comparison) {
        // The seed write watchpoint is the sole draw-index owner. These
        // checkpoints add semantic operands without double-counting a draw.
        for (const auto& [pc, owner] : known_rng_callsite_owners()) {
            if (!is_excluded_from_default_live_profile(pc)) {
                write_predictor_validation_rng_checkpoint(out, pc, owner, false);
            }
        }

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
                "selected_movement_worksheet_path_index_0x15:r3:0x15:u8",
                "selected_movement_worksheet_status_0x16:r3:0x16:u8",
            },
            {},
            {},
            80);

        write_checkpoint(
            out,
            "float_motion_setup_after_increment_8001FC04",
            "8001FC04",
            "float_motion_setup_after_increment",
            "FUN_8001fabc",
            "move_increment_ready",
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
            movement_commit_samples_from_r3(),
            {},
            {},
            400);
    }

    write_checkpoint(
        out,
        "view_geometry_entry_800114AC",
        "800114AC",
        "view_geometry_entry",
        "FUN_800114ac",
        "geometry_inputs",
        false,
        memory_views,
        {
            "geometry_center_output:3",
            "geometry_half_x_output:4",
            "geometry_half_z_output:5",
            "geometry_use_saved_position:6",
        },
        {},
        addrprog_views,
        {},
        1024);

    write_checkpoint(
        out,
        "view_geometry_mld_slot_return_80011544",
        "80011544",
        "view_geometry_mld_slot_return",
        "FUN_800114ac",
        "geometry_mld_inclusion_gate",
        false,
        memory_views,
        {
            "geometry_slot_index:31",
            "mld_slot_return:3",
            "combatant_thread:25",
            "instruction_worksheet:24",
        },
        {
            "candidate_geometry_flags_0xec:r24:0xec:u32",
            "candidate_geometry_extent_0x15c:r24:0x15c:u32",
        },
        {},
        {},
        4096);

    write_checkpoint(
        out,
        "placement_function_geometry_return_80012254",
        "80012254",
        "placement_function_geometry_return",
        "FUN_800121d8",
        "geometry_result",
        false,
        memory_views,
        placement_function_gprs,
        geometry_stack_results,
        addrprog_views,
        {},
        512);

    write_checkpoint(
        out,
        "placement_function_cache_decision_80012308",
        "80012308",
        "placement_function_cache_decision",
        "FUN_800121d8",
        "cache_decision",
        false,
        memory_views,
        {
            "cache_hit_flag_r0:0",
            "placement_function_controller:30",
            "placement_function_record:31",
        },
        placement_function_locals,
        addrprog_views,
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "placement_function_cache_miss_call_80012314",
        "80012314",
        "placement_function_cache_miss_call",
        "FUN_800121d8",
        "cache_miss_rng_call",
        false,
        memory_views,
        placement_function_gprs,
        placement_function_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_121D8_80012530",
        "80012530",
        "view_cache_publish_from_121d8",
        "FUN_800121d8",
        "cache_publication",
        false,
        memory_views,
        placement_function_gprs,
        placement_function_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "runner_geometry_return_800138DC",
        "800138DC",
        "runner_geometry_return",
        "FUN_800136dc",
        "geometry_result",
        false,
        memory_views,
        runner_gprs,
        geometry_stack_results,
        addrprog_views,
        {},
        512);

    write_checkpoint(
        out,
        "runner_cache_draw_80013920",
        "80013920",
        "runner_cache_draw",
        "FUN_800136dc",
        "initial_state_rng_call",
        false,
        memory_views,
        runner_gprs,
        runner_locals,
        addrprog_views,
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_136DC_800139D8",
        "800139D8",
        "view_cache_publish_from_136dc",
        "FUN_800136dc",
        "cache_publication",
        false,
        memory_views,
        runner_gprs,
        runner_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_placement_entry_80014498",
        "80014498",
        "view_placement_entry",
        "FUN_80014474",
        "direct_view",
        false,
        memory_views,
        direct_view_gprs,
        direct_view_locals,
        {},
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_readiness_return_800144EC",
        "800144EC",
        "view_placement_readiness_return",
        "FUN_80014474",
        "readiness_result",
        false,
        memory_views,
        {
            "view_eligibility_result:3",
            "direct_view_controller:30",
            "direct_view_record:31",
        },
        direct_view_locals,
        {},
        {},
        512);

    write_checkpoint(
        out,
        "direct_view_geometry_return_80014508",
        "80014508",
        "direct_view_geometry_return",
        "FUN_80014474",
        "geometry_result",
        false,
        memory_views,
        direct_view_gprs,
        geometry_stack_results,
        addrprog_views,
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_geometry_result_80014548",
        "80014548",
        "view_placement_geometry_result",
        "FUN_80014474",
        "cache_key",
        false,
        memory_views,
        direct_view_gprs,
        direct_view_locals,
        addrprog_views,
        {},
        512);

    write_checkpoint(
        out,
        "view_placement_cache_decision_800145BC",
        "800145BC",
        "view_placement_cache_decision",
        "FUN_80014474",
        "cache_decision",
        false,
        memory_views,
        {
            "cache_hit_flag_r0:0",
            "direct_view_controller:30",
            "direct_view_record:31",
        },
        direct_view_locals,
        addrprog_views,
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_placement_rng_call_800145C8",
        "800145C8",
        "view_placement_rng_call",
        "FUN_80014474",
        "cache_miss_rng_call",
        false,
        memory_views,
        direct_view_gprs,
        direct_view_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_placement_rng_value_800608E0",
        "800608E0",
        "view_placement_rng_value",
        "FUN_800608c8",
        "cache_rng_value",
        false,
        memory_views,
        {
            "rng_rand15_value:3",
            "angle_result_ptr:31",
        },
        {
            "angle_before_mapping_bits:r31:0x00:u32",
        },
        {},
        {},
        1024);

    write_checkpoint(
        out,
        "view_placement_rng_angle_selected_8006093C",
        "8006093C",
        "view_placement_rng_angle_selected",
        "FUN_800608c8",
        "cache_rng_result",
        false,
        memory_views,
        {
            "angle_result_ptr:31",
        },
        {
            "selected_angle_bits:r31:0x00:u32",
        },
        {},
        {},
        1024);

    write_checkpoint(
        out,
        "view_placement_post_selection_800145CC",
        "800145CC",
        "view_placement_post_selection",
        "FUN_80014474",
        "cache_selection_complete",
        false,
        memory_views,
        direct_view_gprs,
        direct_view_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "view_cache_publish_from_14474_80014704",
        "80014704",
        "view_cache_publish_from_14474",
        "FUN_80014474",
        "cache_publication",
        false,
        memory_views,
        direct_view_gprs,
        direct_view_locals,
        {},
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "active_record_reset_before_memset_80014B68",
        "80014B68",
        "active_record_reset_before_memset",
        "Battle::View::SetActiveRecord_80014ab8",
        "workspace_reset_begin",
        false,
        memory_views,
        {
            "workspace_reset_destination:3",
            "workspace_reset_fill_value:4",
            "workspace_reset_size:5",
        },
        {},
        {},
        thread_list_views,
        1024);

    write_checkpoint(
        out,
        "active_record_reset_after_memset_80014B6C",
        "80014B6C",
        "active_record_reset_after_memset",
        "Battle::View::SetActiveRecord_80014ab8",
        "workspace_reset_complete",
        false,
        memory_views,
        {},
        {},
        {},
        thread_list_views,
        1024);

    write_checkpoint(
        out,
        "workspace_copy_before_loop_80052AE8",
        "80052AE8",
        "workspace_copy_before_loop",
        "FUN_8005259c",
        "workspace_copy_begin",
        false,
        memory_views,
        {
            "workspace_copy_source:29",
        },
        workspace_copy_source,
        {},
        thread_list_views,
        1024);

    write_checkpoint(
        out,
        "workspace_copy_after_loop_80052B04",
        "80052B04",
        "workspace_copy_after_loop",
        "FUN_8005259c",
        "workspace_copy_complete",
        false,
        memory_views,
        {
            "workspace_copy_source:29",
        },
        workspace_copy_source,
        {},
        thread_list_views,
        1024);

    return out.str();
}

std::string build_first_battle_view_placement_semantic_hooks_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    return build_first_battle_view_placement_profile_ini(
        thread_list_max_nodes,
        false);
}

std::string build_first_battle_predictor_live_comparison_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    return build_first_battle_view_placement_profile_ini(
        thread_list_max_nodes,
        true);
}

std::string build_first_battle_movement_destination_stop_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    const auto memory_samples = movement_destination_stop_memory_samples();
    const auto frame_addrprog_samples = movement_destination_stop_frame_addrprog_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto frame_addrprog_views = as_string_views(frame_addrprog_samples);
    const auto scratch_samples = movement_scratch_grid_samples_from_r3();
    std::vector<std::string> reachability_grid_samples;
    reachability_grid_samples.reserve(121);
    for (std::uint32_t index = 0; index < 121; ++index) {
        reachability_grid_samples.push_back(
            "movement_reachability_grid_" + std::to_string(index)
            + ":r30:" + hex_u32(0x53u + index) + ":u8");
    }
    const auto reachability_grid_views = as_string_views(reachability_grid_samples);
    auto path_samples = std::vector<std::string>{};
    for (const auto sample : movement_commit_samples_from_r3()) {
        path_samples.emplace_back(sample);
    }
    path_samples.insert(path_samples.end(), scratch_samples.begin(), scratch_samples.end());
    const auto path_views = as_string_views(path_samples);
    const auto thread_list_sample =
        view_placement_frame_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = { thread_list_sample };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_movement_destination_stop\n";
    out << "schema_version=1\n\n";

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "movement_destination_stop_frame",
        false,
        memory_views,
        {},
        {},
        frame_addrprog_views,
        thread_list_views,
        2400);

    write_checkpoint(
        out,
        "movement_reachability_entry_80083728",
        "80083728",
        "movement_reachability_entry",
        "FUN_80083728",
        "movement_path_reachability_begin",
        false,
        memory_views,
        {"actor_slot:3", "target_slot:4"},
        {},
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_reachability_return_80083858",
        "80083858",
        "movement_reachability_return",
        "FUN_80083728",
        "movement_path_reachability_result",
        false,
        memory_views,
        {"reachability_result:3"},
        reachability_grid_views,
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_path_extract_entry_800823E4",
        "800823E4",
        "movement_path_extract_entry",
        "FUN_800823e4",
        "movement_path_extract_begin",
        false,
        memory_views,
        {"movement_worksheet:3"},
        path_views,
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_path_extract_return_800824DC",
        "800824DC",
        "movement_path_extract_return",
        "FUN_800823e4",
        "movement_path_extract_complete",
        false,
        memory_views,
        {"path_distance_result:3", "movement_worksheet_saved:31"},
        {},
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_path_index_adjust_entry_8007FE0C",
        "8007FE0C",
        "movement_path_index_adjust_entry",
        "FUN_8007fe0c",
        "movement_path_straight_run_begin",
        false,
        memory_views,
        {"movement_worksheet:3"},
        path_views,
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_path_index_adjust_return_8007FFD4",
        "8007FFD4",
        "movement_path_index_adjust_return",
        "FUN_8007fe0c",
        "movement_path_straight_run_complete",
        false,
        memory_views,
        {"movement_worksheet_saved:31"},
        {},
        frame_addrprog_views,
        {},
        512);

    const std::array<std::uint32_t, 11> commit_callsites = {{
        0x80086480u,
        0x80086698u,
        0x800871C4u,
        0x800879A8u,
        0x8008816Cu,
        0x800883CCu,
        0x80088434u,
        0x8008C67Cu,
        0x8008C844u,
        0x8008C920u,
        0x8008D56Cu,
    }};
    for (const auto callsite : commit_callsites) {
        std::ostringstream pc;
        pc << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << callsite;
        const auto pc_text = pc.str();
        const auto entry_id = "movement_commit_callsite_" + pc_text;
        write_checkpoint(
            out,
            entry_id,
            pc_text,
            "movement_commit_callsite",
            "movement_commit_caller",
            "movement_commit_before_call",
            false,
            memory_views,
            {
                "movement_worksheet:3",
                "next_grid_x:4",
                "next_grid_z:5",
                "slot:6",
                "caller_context_r28:28",
                "caller_context_r29:29",
                "caller_context_r30:30",
                "caller_context_r31:31",
            },
            movement_commit_samples_from_r3(),
            frame_addrprog_views,
            {},
            512);

        std::ostringstream post_pc;
        post_pc << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
                << (callsite + 4u);
        const auto post_text = post_pc.str();
        write_checkpoint(
            out,
            "movement_commit_post_call_" + post_text,
            post_text,
            "movement_commit_post_call",
            "movement_commit_caller",
            "movement_commit_after_call",
            false,
            memory_views,
            {
                "commit_result:3",
                "caller_context_r28:28",
                "caller_context_r29:29",
                "caller_context_r30:30",
                "caller_context_r31:31",
            },
            {},
            frame_addrprog_views,
            {},
            512);
    }

    write_checkpoint(
        out,
        "movement_posholder_x_store_800819A0",
        "800819A0",
        "movement_posholder_x_store",
        "FUN_8008178c",
        "movement_posholder_publish_x",
        false,
        memory_views,
        {"posholder_row:5", "slot:31", "grid_x:29", "grid_z:30"},
        {"posholder_x_before:r5:0x1c:u32", "posholder_z_current:r5:0x20:u32"},
        frame_addrprog_views,
        {},
        512);
    write_checkpoint(
        out,
        "movement_posholder_z_store_800819B8",
        "800819B8",
        "movement_posholder_z_store",
        "FUN_8008178c",
        "movement_posholder_publish_z",
        false,
        memory_views,
        {"posholder_row:4", "slot:31", "grid_x:29", "grid_z:30"},
        {"posholder_x_current:r4:0x1c:u32", "posholder_z_before:r4:0x20:u32"},
        frame_addrprog_views,
        {},
        512);

    write_checkpoint(
        out,
        "action_motion_target_return_8001FADC",
        "8001FADC",
        "action_motion_target_return",
        "FUN_8001fabc",
        "action_motion_target_selected",
        false,
        memory_views,
        {"target_helper_result:3", "combatant_worksheet:30", "instruction_worksheet:31"},
        {
            "target_vector_x_bits:r1:0x08:u32",
            "target_vector_y_bits:r1:0x0c:u32",
            "target_vector_z_bits:r1:0x10:u32",
        },
        frame_addrprog_views,
        {},
        1024);
    write_checkpoint(
        out,
        "action_motion_setup_complete_8001FC04",
        "8001FC04",
        "action_motion_setup_complete",
        "FUN_8001fabc",
        "action_motion_increment_ready",
        false,
        memory_views,
        {"combatant_worksheet:30", "instruction_worksheet:31"},
        float_motion_instruction_samples_from_r31(),
        frame_addrprog_views,
        {},
        1024);

    const std::array<std::pair<std::string_view, std::string_view>, 5> motion_results = {{
        {"8001EAB8", "action_motion_clamp_x_return"},
        {"8001EB20", "action_motion_clamp_z_return"},
        {"8001EB54", "action_motion_final_result"},
        {"8001B778", "action_motion_caller_consumption"},
        {"8001FC04", "action_motion_setup_post_state"},
    }};
    for (const auto& [pc, name] : motion_results) {
        if (pc == "8001FC04") {
            continue;
        }
        write_checkpoint(
            out,
            std::string(name) + "_" + std::string(pc),
            pc,
            name,
            "action_motion_controller",
            "action_motion_stop_chain",
            false,
            memory_views,
            {
                "result_r3:3",
                "motion_context_r27:27",
                "motion_context_r28:28",
                "motion_context_r29:29",
                "motion_context_r30:30",
                "motion_context_r31:31",
            },
            {},
            frame_addrprog_views,
            {},
            2400);
    }

    write_static_watchpoint(
        out,
        "movement_completion_override_write_80347348",
        0x80347348u,
        "u32",
        "write",
        "normal");

    const std::vector<std::string_view> completion_gate_thread_samples = {
        "movement_thread_state_0x19:r3:+0x19:u8",
        "movement_worksheet_ptr_0x24:r3:+0x24:u32",
        "movement_slot_0x00:r3:+0x24|load_ptr32|+0x00:u16",
        "movement_coord_state_0x50:r3:+0x24|load_ptr32|+0x50:u8",
    };
    write_checkpoint(
        out,
        "movement_completion_gate_entry_80080438",
        "80080438",
        "movement_completion_gate_entry",
        "FUN_80080438",
        "movement_completion_gate",
        false,
        memory_views,
        {"movement_thread:3"},
        {},
        completion_gate_thread_samples,
        {},
        2048);
    write_checkpoint(
        out,
        "movement_completion_gate_true_return_800804AC",
        "800804AC",
        "movement_completion_gate_true_return",
        "FUN_80080438",
        "movement_completion_gate_result",
        false,
        memory_views,
        {"completion_result:3"},
        {},
        {},
        {},
        2048);
    write_checkpoint(
        out,
        "movement_completion_gate_false_return_800804B4",
        "800804B4",
        "movement_completion_gate_false_return",
        "FUN_80080438",
        "movement_completion_gate_result",
        false,
        memory_views,
        {"completion_result:3"},
        {},
        {},
        {},
        2048);

    return out.str();
}

std::string build_first_battle_action_view_service_lifecycle_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    const auto memory_samples = action_view_service_lifecycle_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto slot_samples = action_view_service_lifecycle_slot_addrprog_samples();
    const auto slot_views = as_string_views(slot_samples);
    const auto controller_r28_samples = action_view_controller_addrprog_samples("r28");
    const auto controller_r28_views = as_string_views(controller_r28_samples);
    const auto controller_r31_samples = action_view_controller_addrprog_samples("r31");
    const auto controller_r31_views = as_string_views(controller_r31_samples);
    const auto record_r29_samples = action_view_record_addrprog_samples("r29");
    const auto record_r29_views = as_string_views(record_r29_samples);
    const auto record_r30_samples = action_view_record_addrprog_samples("r30");
    const auto record_r30_views = as_string_views(record_r30_samples);
    const auto record_r31_samples = action_view_record_addrprog_samples("r31");
    const auto record_r31_views = as_string_views(record_r31_samples);
    const auto service_r29_samples = action_service_child_addrprog_samples("r29");
    const auto service_r29_views = as_string_views(service_r29_samples);
    const auto selector_chain_samples = action_view_chain_addrprog_samples();
    const auto selector_chain_views = as_string_views(selector_chain_samples);
    auto controller_selector_samples = controller_r28_samples;
    controller_selector_samples.insert(
        controller_selector_samples.end(),
        selector_chain_samples.begin(),
        selector_chain_samples.end());
    const auto controller_selector_views = as_string_views(controller_selector_samples);
    const auto thread_list_sample =
        view_placement_frame_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = { thread_list_sample };
    const std::vector<std::string_view> child_thread_r3_addrprog = {
        "child_thread_callback_0x00:r3:+0x00:u32",
        "child_thread_state_0x19:r3:+0x19:u8",
        "child_thread_flags_0x18:r3:+0x18:u8",
        "child_thread_order_bits_0x20:r3:+0x20:u32",
        "child_thread_payload_ptr_0x24:r3:+0x24:u32",
    };

    const std::vector<std::string_view> serialized_creator_entry_addrprog = {
        "serialized_payload_ptr_0x0c:r3:+0x0c:u32",
        "serialized_payload_mode_0x22:r3:+0x0c|load_ptr32|+0x22:u16",
        "serialized_payload_flags_0x10:r3:+0x0c|load_ptr32|+0x10:u32",
        "serialized_origin_callback_0x00:r4:+0x00:u32",
        "serialized_origin_iw_slot_0x00:r4:+0x24|load_ptr32|+0x4c|load_ptr32|+0x00:u8",
        "serialized_origin_iw_mode_0x06:r4:+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
    };
    const std::vector<std::string_view> serialized_creator_saved_addrprog = {
        "serialized_payload_mode_0x22:r30:+0x22:u16",
        "serialized_payload_flags_0x10:r30:+0x10:u32",
        "serialized_origin_callback_0x00:r29:+0x00:u32",
        "serialized_origin_iw_slot_0x00:r29:+0x24|load_ptr32|+0x4c|load_ptr32|+0x00:u8",
        "serialized_origin_iw_mode_0x06:r29:+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
    };
    const std::vector<std::string_view> service_creator_entry_addrprog = {
        "command_payload_ptr_0x0c:r3:+0x0c:u32",
        "command_mode_0x02:r3:+0x0c|load_ptr32|+0x02:u16",
        "command_subtype_0x04:r3:+0x0c|load_ptr32|+0x04:u16",
        "command_sync_flags_0x16:r3:+0x0c|load_ptr32|+0x16:u16",
        "command_flags_0x20:r3:+0x0c|load_ptr32|+0x20:u32",
        "command_delay_0x24:r3:+0x0c|load_ptr32|+0x24:u16",
        "command_forced_mode_0x26:r3:+0x0c|load_ptr32|+0x26:u16",
        "command_origin_callback_0x00:r4:+0x00:u32",
        "command_origin_iw_slot_0x00:r4:+0x24|load_ptr32|+0x4c|load_ptr32|+0x00:u8",
        "command_origin_iw_mode_0x06:r4:+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
    };
    const std::vector<std::string_view> service_creator_saved_addrprog = {
        "command_mode_0x02:r31:+0x02:u16",
        "command_subtype_0x04:r31:+0x04:u16",
        "command_sync_flags_0x16:r31:+0x16:u16",
        "command_flags_0x20:r31:+0x20:u32",
        "command_delay_0x24:r31:+0x24:u16",
        "command_forced_mode_0x26:r31:+0x26:u16",
        "command_origin_callback_0x00:r30:+0x00:u32",
        "command_origin_iw_slot_0x00:r30:+0x24|load_ptr32|+0x4c|load_ptr32|+0x00:u8",
        "command_origin_iw_mode_0x06:r30:+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
    };
    const std::vector<std::string_view> synthetic_payload_addrprog = {
        "synthetic_payload_ptr:r1:+0x08:u32",
        "synthetic_payload_mode_0x22:r1:+0x08|load_ptr32|+0x22:u16",
        "synthetic_origin_callback_0x00:r31:+0x00:u32",
        "synthetic_origin_iw_slot_0x00:r31:+0x24|load_ptr32|+0x4c|load_ptr32|+0x00:u8",
        "synthetic_origin_iw_mode_0x06:r31:+0x24|load_ptr32|+0x4c|load_ptr32|+0x06:u16",
    };
    const std::vector<std::string_view> eb4c_addrprog = {
        "eb4c_target_callback_0x00:r30:+0x00:u32",
        "eb4c_target_thread_state_0x19:r30:+0x19:u8",
        "eb4c_target_iw_slot_0x00:r28:+0x00:u8",
        "eb4c_target_iw_target_0x04:r28:+0x04:u8",
        "eb4c_target_iw_mode_0x06:r28:+0x06:u16",
        "eb4c_target_iw_previous_mode_0x0a:r28:+0x0a:u16",
        "eb4c_target_iw_owner_gate_0x50:r28:+0x50:u32",
        "eb4c_target_iw_flags_0xec:r28:+0xec:u32",
        "eb4c_target_iw_visual_timer_0x164:r28:+0x164:u32",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_action_view_service_lifecycle\n";
    out << "schema_version=1\n\n";

    // Dolphin reports the watched seed after the write. This watchpoint is the
    // sole owner of RNG draws; callsite checkpoints only provide attribution.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    write_checkpoint(
        out,
        "setup_turn_action_entry_80082134",
        "80082134",
        "setup_turn_action_entry",
        "setupTurnAction_80082134",
        "capture_activation_boundary",
        false,
        memory_views,
        {"actor_slot_arg:3"},
        {},
        slot_views,
        thread_list_views,
        16);
    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "frame_thread_order",
        false,
        memory_views,
        {},
        {},
        slot_views,
        thread_list_views,
        2400,
        0x80082134u);

    write_checkpoint(
        out,
        "action_view_controller_creator_entry_80014784",
        "80014784",
        "action_view_controller_creator_entry",
        "FUN_80014784",
        "controller_create",
        false,
        memory_views,
        {},
        {},
        {},
        thread_list_views,
        16);
    write_checkpoint(
        out,
        "action_view_controller_child_return_800147B0",
        "800147B0",
        "action_view_controller_child_return",
        "FUN_80014784",
        "controller_child_created",
        false,
        memory_views,
        {"controller_thread_return:3"},
        {},
        child_thread_r3_addrprog,
        thread_list_views,
        16);
    write_checkpoint(
        out,
        "action_view_controller_publication_800147E8",
        "800147E8",
        "action_view_controller_publication",
        "FUN_80014784",
        "controller_payload_ready",
        false,
        memory_views,
        {"controller_thread:31"},
        {},
        controller_r31_views,
        thread_list_views,
        16);
    write_checkpoint(
        out,
        "action_view_controller_selector_call_80013B20",
        "80013B20",
        "action_view_controller_selector_call",
        "FUN_800136dc",
        "selector_before",
        false,
        memory_views,
        {"controller_thread:28", "selector_state:29"},
        {},
        controller_selector_views,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "action_view_controller_selector_return_80013B28",
        "80013B28",
        "action_view_controller_selector_return",
        "FUN_800136dc",
        "selector_after",
        false,
        memory_views,
        {"controller_thread:28", "selector_state:29"},
        {},
        controller_selector_views,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "action_view_controller_direct_view_call_800139F8",
        "800139F8",
        "action_view_controller_direct_view_call",
        "FUN_800136dc",
        "direct_view_invocation",
        false,
        memory_views,
        {"controller_thread:28", "selector_state:29"},
        {},
        controller_r28_views,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "direct_view_cache_draw_call_800145C8",
        "800145C8",
        "direct_view_cache_draw_call",
        "FUN_80014474",
        "direct_view_cache_miss_rng",
        false,
        memory_views,
        {"direct_view_controller:30", "direct_view_record:31"},
        {},
        {},
        thread_list_views,
        512);

    // Keep the field6 classification and branch-specific query checkpoints,
    // but omit the other per-call coverage checkpoints that exhausted the
    // VM's RunToBP hit guard in the broad first attempt.
    write_checkpoint(
        out,
        "action_view_selector_field6_classify_80012FCC",
        "80012FCC",
        "action_view_selector_field6_classify",
        "FUN_80012f58",
        "action_view_selector_entry",
        false,
        action_view_globals(),
        action_view_selector_core_gprs(),
        action_view_category2_gate_samples(),
        selector_chain_views,
        {},
        512);
    write_action_view_selector_query_only_checkpoints(out, 512);

    write_checkpoint(
        out,
        "serialized_action_view_creator_entry_8003C690",
        "8003C690",
        "serialized_action_view_creator_entry",
        "Battle::Gfx::Combatants::SystemCameraHandler_8003c690",
        "record_creator_entry",
        false,
        memory_views,
        {"command_wrapper:3", "origin_thread:4"},
        {},
        serialized_creator_entry_addrprog,
        {},
        512);
    write_checkpoint(
        out,
        "serialized_action_view_gate_result_8003C6C0",
        "8003C6C0",
        "serialized_action_view_gate_result",
        "Battle::Gfx::Combatants::SystemCameraHandler_8003c690",
        "record_creator_gate",
        false,
        memory_views,
        {"gate_result:3", "serialized_payload:30", "origin_thread:29"},
        {},
        serialized_creator_saved_addrprog,
        {},
        512);
    write_checkpoint(
        out,
        "serialized_action_view_child_return_8003C6D8",
        "8003C6D8",
        "serialized_action_view_child_return",
        "Battle::Gfx::Combatants::SystemCameraHandler_8003c690",
        "record_child_created",
        false,
        memory_views,
        {"record_thread_return:3", "serialized_payload:30", "origin_thread:29"},
        {},
        child_thread_r3_addrprog,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "serialized_action_view_publication_8003C738",
        "8003C738",
        "serialized_action_view_publication",
        "Battle::Gfx::Combatants::SystemCameraHandler_8003c690",
        "record_payload_ready",
        false,
        memory_views,
        {"record_thread:31", "serialized_payload:30", "origin_thread:29"},
        {},
        record_r31_views,
        thread_list_views,
        512);

    write_checkpoint(
        out,
        "synthetic_action_view_creator_entry_80053F38",
        "80053F38",
        "synthetic_action_view_creator_entry",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_record_creator_entry",
        false,
        memory_views,
        {"requested_slot:3", "special_mode:4"},
        {},
        {},
        {},
        512);
    write_checkpoint(
        out,
        "synthetic_action_view_origin_return_80053F60",
        "80053F60",
        "synthetic_action_view_origin_return",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_origin_selected",
        false,
        memory_views,
        {"origin_thread_return:3", "origin_thread:31", "special_mode:28"},
        {},
        {},
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "synthetic_action_view_mode_selected_80054038",
        "80054038",
        "synthetic_action_view_mode_selected",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_mode_selected",
        false,
        memory_views,
        {"selected_payload_mode:28", "origin_thread:31"},
        {},
        {},
        {},
        512);
    write_checkpoint(
        out,
        "synthetic_action_view_payload_ready_80054058",
        "80054058",
        "synthetic_action_view_payload_ready",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_payload_ready",
        false,
        memory_views,
        {"selected_payload_mode:28", "origin_thread:31"},
        {},
        synthetic_payload_addrprog,
        {},
        512);
    write_checkpoint(
        out,
        "synthetic_action_view_child_return_8005408C",
        "8005408C",
        "synthetic_action_view_child_return",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_record_child_created",
        false,
        memory_views,
        {"record_thread_return:3", "origin_thread:31", "selected_payload_mode:28"},
        {},
        child_thread_r3_addrprog,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "synthetic_action_view_publication_800540BC",
        "800540BC",
        "synthetic_action_view_publication",
        "Battle::View::SpawnSyntheticActionViewRecord_80053f38",
        "synthetic_record_payload_ready",
        false,
        memory_views,
        {"record_thread:30", "origin_thread:31", "selected_payload_mode:28"},
        {},
        record_r30_views,
        thread_list_views,
        512);

    const auto write_record_checkpoint =
        [&](std::string_view id,
            std::string_view pc,
            std::string_view checkpoint,
            const std::vector<std::string_view>& addrprog,
            bool include_thread_list,
            std::uint32_t max_hits) {
            write_checkpoint(
                out,
                id,
                pc,
                id,
                "Battle::Turn::UpdateActionViewRecord_80051264",
                checkpoint,
                false,
                memory_views,
                {"record_thread:29", "record_worksheet:31", "origin_instruction:30"},
                {},
                addrprog,
                include_thread_list ? thread_list_views : std::vector<std::string_view>{},
                max_hits);
        };
    write_record_checkpoint(
        "action_view_record_state0_helper_80051320",
        "80051320",
        "record_state0_helper",
        record_r29_views,
        false,
        512);
    write_record_checkpoint(
        "action_view_record_mode0_draw_800513D4",
        "800513D4",
        "record_mode0_rng_call",
        record_r29_views,
        true,
        512);
    write_record_checkpoint(
        "action_view_record_effective_mode_write_8005141C",
        "8005141C",
        "record_effective_mode_write",
        record_r29_views,
        false,
        512);
    write_record_checkpoint(
        "action_view_record_mode1_call_800514B0",
        "800514B0",
        "record_mode1_call",
        record_r29_views,
        false,
        512);
    write_record_checkpoint(
        "action_view_record_mode0e_call_800514C8",
        "800514C8",
        "record_mode0e_call",
        record_r29_views,
        false,
        512);
    write_record_checkpoint(
        "action_view_record_cleanup_begin_80051600",
        "80051600",
        "record_cleanup_begin",
        record_r29_views,
        false,
        512);
    write_record_checkpoint(
        "action_view_record_normal_completion_80051698",
        "80051698",
        "record_normal_completion",
        record_r29_views,
        true,
        512);
    write_record_checkpoint(
        "action_view_record_state3_completion_800516BC",
        "800516BC",
        "record_state3_completion",
        record_r29_views,
        true,
        512);

    write_checkpoint(
        out,
        "action_service_creator_entry_8003B1D8",
        "8003B1D8",
        "action_service_creator_entry",
        "Battle::Gfx::Combatants::SetCommandHandler_8003b1d8",
        "service_creator_entry",
        false,
        memory_views,
        {"command_wrapper:3", "origin_thread:4"},
        {},
        service_creator_entry_addrprog,
        {},
        512);
    write_checkpoint(
        out,
        "action_service_gate_result_8003B208",
        "8003B208",
        "action_service_gate_result",
        "Battle::Gfx::Combatants::SetCommandHandler_8003b1d8",
        "service_creator_gate",
        false,
        memory_views,
        {"gate_result:3", "command_payload:31", "origin_thread:30"},
        {},
        service_creator_saved_addrprog,
        {},
        512);
    write_checkpoint(
        out,
        "action_service_child_return_8003B220",
        "8003B220",
        "action_service_child_return",
        "Battle::Gfx::Combatants::SetCommandHandler_8003b1d8",
        "service_child_created",
        false,
        memory_views,
        {"service_thread_return:3", "command_payload:31", "origin_thread:30"},
        {},
        child_thread_r3_addrprog,
        thread_list_views,
        512);
    write_checkpoint(
        out,
        "action_service_publication_8003B2B4",
        "8003B2B4",
        "action_service_publication",
        "Battle::Gfx::Combatants::SetCommandHandler_8003b1d8",
        "service_payload_ready",
        false,
        memory_views,
        {"service_thread:29", "command_payload:31", "origin_thread:30"},
        {},
        service_r29_views,
        thread_list_views,
        512);

    const auto write_service_checkpoint =
        [&](std::string_view id,
            std::string_view pc,
            std::string_view checkpoint,
            bool include_thread_list,
            std::uint32_t max_hits) {
            write_checkpoint(
                out,
                id,
                pc,
                id,
                "FUN_8004281c",
                checkpoint,
                false,
                memory_views,
                {"service_thread:29", "service_payload:31", "origin_instruction:30"},
                {},
                service_r29_views,
                include_thread_list ? thread_list_views : std::vector<std::string_view>{},
                max_hits);
        };
    write_service_checkpoint(
        "action_service_init_800428D4", "800428D4", "service_initialization", false, 512);
    write_service_checkpoint(
        "action_service_delay_80042938", "80042938", "service_delay", false, 2400);
    write_service_checkpoint(
        "action_service_split_80042958", "80042958", "service_path_split", false, 512);
    write_service_checkpoint(
        "action_service_nested_call_80042990", "80042990", "service_nested_call", true, 512);
    write_service_checkpoint(
        "action_service_forced_path_800429AC", "800429AC", "service_forced_path", false, 512);
    write_service_checkpoint(
        "action_service_state3_cleanup_800429D8", "800429D8", "service_state3_cleanup", false, 512);
    write_service_checkpoint(
        "action_service_cleanup_commit_80042A14", "80042A14", "service_cleanup_commit", true, 512);
    write_service_checkpoint(
        "action_service_alt_cleanup_commit_80042A90", "80042A90", "service_alt_cleanup_commit", true, 512);

    const auto nested_gprs = std::vector<std::string_view>{
        "battle_instance:3",
        "requested_mode:4",
        "requested_subtype:5",
        "temporary_target_flag:6",
        "context_r26:26",
        "owner_slot_r27:27",
        "target_instruction_r28:28",
        "selected_mode_r29:29",
        "selected_thread_r30:30",
        "candidate_index_r31:31",
    };
    const auto eb4c_candidate_samples = std::vector<std::string_view>{
        "eb4c_candidate0_mode:r1:0x08:u16",
        "eb4c_candidate1_mode:r1:0x0a:u16",
    };
    const auto write_nested_checkpoint =
        [&](std::string_view id,
            std::string_view pc,
            std::string_view function,
            std::string_view checkpoint,
            bool include_eb4c_state,
            bool include_thread_list) {
            write_checkpoint(
                out,
                id,
                pc,
                id,
                function,
                checkpoint,
                false,
                memory_views,
                nested_gprs,
                include_eb4c_state
                    ? eb4c_candidate_samples
                    : std::vector<std::string_view>{},
                include_eb4c_state ? eb4c_addrprog : std::vector<std::string_view>{},
                include_thread_list ? thread_list_views : std::vector<std::string_view>{},
                512);
        };
    write_nested_checkpoint(
        "action_service_nested_entry_80020B8C",
        "80020B8C",
        "FUN_80020b8c",
        "nested_action_entry",
        false,
        false);
    write_nested_checkpoint(
        "action_service_nested_dispatch_80020D28",
        "80020D28",
        "FUN_80020b8c",
        "nested_action_dispatch",
        false,
        true);
    write_nested_checkpoint(
        "action_service_resolution_entry_8002E5D0",
        "8002E5D0",
        "FUN_8002e5d0",
        "action_resolution_entry",
        false,
        false);
    write_nested_checkpoint(
        "action_service_mode_path_8002E8F8",
        "8002E8F8",
        "FUN_8002e5d0",
        "action_resolution_mode_path",
        false,
        false);
    write_nested_checkpoint(
        "action_service_eb4c_gate_8002E9B0",
        "8002E9B0",
        "FUN_8002e5d0",
        "eb4c_gate",
        false,
        true);
    write_nested_checkpoint(
        "action_service_eb4c_call_8002E9C4",
        "8002E9C4",
        "FUN_8002e5d0",
        "eb4c_call",
        false,
        true);
    write_nested_checkpoint(
        "eb4c_entry_8002EB4C",
        "8002EB4C",
        "FUN_8002eb4c",
        "eb4c_entry",
        false,
        true);
    write_nested_checkpoint(
        "eb4c_initial_candidate_lookup_8002EBA4",
        "8002EBA4",
        "FUN_8002eb4c",
        "eb4c_initial_candidate_lookup",
        true,
        false);
    write_nested_checkpoint(
        "eb4c_fallback_draw_8002EBDC",
        "8002EBDC",
        "FUN_8002eb4c",
        "eb4c_fallback_rng_call",
        true,
        true);
    write_nested_checkpoint(
        "eb4c_selected_candidate_lookup_8002EC08",
        "8002EC08",
        "FUN_8002eb4c",
        "eb4c_selected_candidate_lookup",
        true,
        false);
    write_nested_checkpoint(
        "eb4c_selection_8002EC14",
        "8002EC14",
        "FUN_8002eb4c",
        "eb4c_selection",
        true,
        true);
    write_nested_checkpoint(
        "eb4c_publication_call_8002EC2C",
        "8002EC2C",
        "FUN_8002eb4c",
        "eb4c_publication",
        true,
        true);
    write_nested_checkpoint(
        "eb4c_return_8002EC90",
        "8002EC90",
        "FUN_8002eb4c",
        "eb4c_return",
        true,
        true);

    return out.str();
}

std::string build_first_battle_pc_worker_selector_lifetime_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    const auto memory_samples = pc_worker_selector_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto selector_samples = pc_worker_selector_addrprog_samples();
    const auto selector_views = as_string_views(selector_samples);
    const auto frame_samples = movement_destination_stop_frame_addrprog_samples();
    const auto frame_views = as_string_views(frame_samples);
    const auto thread_list_sample =
        pc_worker_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = {thread_list_sample};
    const std::vector<std::string_view> no_lists;
    const auto worker_r3_views = pc_worker_thread_r3_addrprog_samples();
    const std::vector<std::string_view> fallback_thread_r28 = {
        "fallback_thread_callback_0x00:r28:0x00:u32",
        "fallback_thread_state_0x19:r28:0x19:u8",
        "fallback_thread_payload_0x24:r28:0x24:u32",
        "fallback_iw_slot_0x00:r29:0x00:u16",
        "fallback_iw_flags_0x04:r29:0x04:u32",
        "fallback_iw_distance_0x14:r29:0x14:u8",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_pc_worker_selector_lifetime\n";
    out << "schema_version=1\n\n";

    // Dolphin reports the post-write seed. This watchpoint is the only draw
    // owner; checkpoints below provide semantic attribution only.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    const auto write_root_checkpoint = [&out, &memory_views, &selector_views,
                                        &thread_list_views, &no_lists](
        std::string_view id,
        std::string_view pc,
        std::string_view name,
        std::string_view function,
        std::string_view checkpoint,
        std::initializer_list<std::string_view> gprs,
        std::uint32_t max_hits,
        bool include_list = false) {
        const std::vector<std::string_view> gpr_views(gprs);
        write_checkpoint(
            out,
            id,
            pc,
            name,
            function,
            checkpoint,
            false,
            memory_views,
            gpr_views,
            {},
            selector_views,
            include_list ? thread_list_views : no_lists,
            max_hits,
            0x80070A54u);
    };

    write_root_checkpoint(
        "setup_turn_action_entry_80082134", "80082134",
        "setup_turn_action_entry", "FUN_80082134", "turn_action_capture_activation",
        {"actor_slot:3", "target_slot:4"}, 32, true);
    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        memory_views,
        {},
        {},
        frame_views,
        thread_list_views,
        2400,
        0x80070A54u);

    for (const auto& checkpoint : std::array{
             std::tuple{"setup_action_pc_handler_store_80070A54", "80070A54",
                        "setup_action_pc_handler_store", "setup_action_handler_install"},
             std::tuple{"setup_action_before_initial_relay_80070B54", "80070B54",
                        "setup_action_before_initial_relay", "setup_action_initial_relay"},
             std::tuple{"setup_action_after_initial_relay_80070B58", "80070B58",
                        "setup_action_after_initial_relay", "setup_action_initial_relay"},
             std::tuple{"setup_action_before_dispatch_relay_80070B98", "80070B98",
                        "setup_action_before_dispatch_relay", "setup_action_dispatch_relay"},
             std::tuple{"setup_action_after_dispatch_relay_80070B9C", "80070B9C",
                        "setup_action_after_dispatch_relay", "setup_action_dispatch_relay"}}) {
        write_root_checkpoint(
            std::get<0>(checkpoint), std::get<1>(checkpoint),
            std::get<2>(checkpoint), "Battle::Run::setupAction_800708c0",
            std::get<3>(checkpoint), {"actor_slot:30", "actor_slot_x4:31"},
            64, true);
    }

    write_root_checkpoint(
        "pc_selector_entry_800855AC", "800855AC", "pc_selector_entry",
        "FUN_800855ac", "pc_worker_selector_begin", {"actor_slot:3"}, 128);
    write_root_checkpoint(
        "pc_selector_reachability_call_8008566C", "8008566C",
        "pc_selector_reachability_call", "FUN_800855ac",
        "pc_worker_selector_reachability", {"actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_reachability_return_80085670", "80085670",
        "pc_selector_reachability_return", "FUN_800855ac",
        "pc_worker_selector_reachability", {"result:3", "actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_path_shape_call_8008567C", "8008567C",
        "pc_selector_path_shape_call", "FUN_800855ac",
        "pc_worker_selector_path_shape", {"actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_path_shape_return_80085680", "80085680",
        "pc_selector_path_shape_return", "FUN_800855ac",
        "pc_worker_selector_path_shape", {"result:3", "actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_distance_loaded_8008569C", "8008569C",
        "pc_selector_distance_loaded", "FUN_800855ac",
        "pc_worker_selector_distance", {"distance:0", "worksheet:3", "actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_direct_branch_800856A8", "800856A8",
        "pc_selector_direct_branch", "FUN_800855ac",
        "pc_worker_selector_decision", {"actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_fallback_branch_800856B0", "800856B0",
        "pc_selector_fallback_branch", "FUN_800855ac",
        "pc_worker_selector_decision", {"actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_fallback_write_complete_800856C8", "800856C8",
        "pc_selector_fallback_write_complete", "FUN_800855ac",
        "pc_worker_selector_decision", {"result:3", "actor_slot:27", "target_slot:28"}, 128);
    write_root_checkpoint(
        "pc_selector_return_80085708", "80085708", "pc_selector_return",
        "FUN_800855ac", "pc_worker_selector_return", {"result:3", "actor_slot:27"}, 128);

    for (const auto& checkpoint : std::array{
             std::tuple{"handle_pc_selector_call_80086DD8", "80086DD8", "handle_pc_selector_call"},
             std::tuple{"handle_pc_selector_return_80086DDC", "80086DDC", "handle_pc_selector_return"},
             std::tuple{"handle_pc_param_branch_80086F14", "80086F14", "handle_pc_param_branch"},
             std::tuple{"handle_pc_direct_publish_complete_80086F40", "80086F40", "handle_pc_direct_publish_complete"},
             std::tuple{"handle_pc_direct_immediate_call_80086F48", "80086F48", "handle_pc_direct_immediate_call"},
             std::tuple{"handle_pc_fallback_publish_complete_80086F70", "80086F70", "handle_pc_fallback_publish_complete"},
             std::tuple{"handle_pc_fallback_state_reset_80086F78", "80086F78", "handle_pc_fallback_state_reset"}}) {
        write_root_checkpoint(
            std::get<0>(checkpoint), std::get<1>(checkpoint), std::get<2>(checkpoint),
            "Battle::HandlePCInst_80086c68", "pc_worker_publication",
            {"movement_thread:27", "worksheet:28", "actor_slot:29", "queued_instruction:26", "branch_value:0"},
            256, true);
    }

    const auto write_worker_entry = [&out, &memory_views, &worker_r3_views](
        std::string_view id,
        std::string_view pc,
        std::string_view name,
        std::string_view function,
        std::uint32_t max_hits) {
        write_checkpoint(
            out, id, pc, name, function, "movement_worker_visit", false,
            memory_views, {"thread:3"}, {}, worker_r3_views, {}, max_hits,
            0x80070A54u);
    };
    write_worker_entry(
        "pc_fallback_worker_entry_80085CE0", "80085CE0",
        "pc_fallback_worker_entry", "FUN_80085ce0", 2400);
    write_worker_entry(
        "pc_direct_worker_entry_80086308", "80086308",
        "pc_direct_worker_entry", "FUN_80086308", 2400);
    write_root_checkpoint(
        "pc_direct_commit_call_80086480", "80086480", "pc_direct_commit_call",
        "FUN_80086308", "active_direct_commit", {"movement_worksheet:3", "next_x:4", "next_z:5", "slot:6"}, 512);
    write_worker_entry(
        "pc_worker_terminal_entry_80086C48", "80086C48",
        "pc_worker_terminal_entry", "FUN_80086c48", 512);

    const auto write_fallback_poll = [&out, &memory_views, &selector_views,
                                      &fallback_thread_r28](
        std::string_view id,
        std::string_view pc,
        std::string_view name) {
        write_checkpoint(
            out, id, pc, name, "FUN_80085ce0", "fallback_instruction_poll",
            false, memory_views,
            {"poll_actor_or_result:3", "poll_mode:4", "actor_slot:31", "target_slot:30", "thread:28"},
            fallback_thread_r28, selector_views, {}, 2400, 0x80070A54u);
    };
    for (const auto& checkpoint : std::array{
             std::pair{"pc_fallback_poll_mode7_call_80085F14", "80085F14"},
             std::pair{"pc_fallback_poll_mode7_return_80085F18", "80085F18"},
             std::pair{"pc_fallback_poll_mode12_call_8008604C", "8008604C"},
             std::pair{"pc_fallback_poll_mode12_return_80086050", "80086050"},
             std::pair{"pc_fallback_poll_actor_mode16_call_800860A4", "800860A4"},
             std::pair{"pc_fallback_poll_actor_mode16_return_800860A8", "800860A8"},
             std::pair{"pc_fallback_poll_target_mode16_call_800860B8", "800860B8"},
             std::pair{"pc_fallback_poll_target_mode16_return_800860BC", "800860BC"},
             std::pair{"pc_fallback_poll_mode0_call_8008612C", "8008612C"},
             std::pair{"pc_fallback_poll_mode0_return_80086130", "80086130"},
             std::pair{"pc_fallback_poll_mode0e_call_80086178", "80086178"},
             std::pair{"pc_fallback_poll_mode0e_return_8008617C", "8008617C"},
             std::pair{"pc_fallback_poll_no_target_mode0_call_800862B4", "800862B4"},
             std::pair{"pc_fallback_poll_no_target_mode0_return_800862B8", "800862B8"}}) {
        write_fallback_poll(
            checkpoint.first, checkpoint.second,
            checkpoint.first);
    }

    write_worker_entry(
        "passive_initial_relay_entry_800804B8", "800804B8",
        "passive_initial_relay_entry", "FUN_800804b8", 2400);
    write_worker_entry(
        "passive_relay_entry_800801A8", "800801A8",
        "passive_relay_entry", "FUN_800801a8", 2400);
    write_root_checkpoint(
        "passive_relay_promote_80080244", "80080244", "passive_relay_promote",
        "FUN_800801a8", "passive_relay_publication",
        {"slot:31", "movement_thread:6", "movement_worksheet:3", "deferred_callback:0"},
        2400, true);
    write_worker_entry(
        "passive_dispatch_entry_8008DEEC", "8008DEEC",
        "passive_dispatch_entry", "FUN_8008deec", 2400);
    for (const auto& family : std::array{
             std::tuple{"passive_ambient_pursuit_entry_8008C21C", "8008C21C", "FUN_8008c21c"},
             std::tuple{"passive_ambient_formation_entry_8008C7B0", "8008C7B0", "FUN_8008c7b0"},
             std::tuple{"passive_ambient_idle_entry_8008C98C", "8008C98C", "FUN_8008c98c"},
             std::tuple{"passive_affected_target_entry_8008D3B0", "8008D3B0", "FUN_8008d3b0"},
             std::tuple{"passive_affected_target_worker_entry_8008CDA8", "8008CDA8", "FUN_8008cda8"},
             std::tuple{"passive_pursuit_handoff_entry_8008C6BC", "8008C6BC", "FUN_8008c6bc"},
             std::tuple{"passive_special_entry_8008D610", "8008D610", "FUN_8008d610"},
             std::tuple{"passive_status_entry_8008D960", "8008D960", "FUN_8008d960"}}) {
        write_worker_entry(
            std::get<0>(family), std::get<1>(family),
            std::get<0>(family), std::get<2>(family), 2400);
    }

    write_root_checkpoint(
        "attack_result_return_80081BE8", "80081BE8", "attack_result_return",
        "FUN_80081b94", "attack_result", {"attack_result:3"}, 256);
    write_root_checkpoint(
        "attack_result_write_80081C48", "80081C48", "attack_result_write",
        "FUN_80081c04", "attack_result", {"actor_slot:31", "target_slot:30", "attack_result:3"}, 256);
    write_root_checkpoint(
        "action_view_record_mode1_call_800514B0", "800514B0",
        "action_view_record_mode1_call", "FUN_800512c0", "action_view_pathing",
        {"action_view_record:31"}, 512);

    return out.str();
}

std::string build_first_battle_action_view_pathing_loop_profile_ini()
{
    const auto memory_samples = action_view_service_lifecycle_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto slot_samples = action_view_pathing_loop_slot_addrprog_samples();
    const auto slot_views = as_string_views(slot_samples);
    const auto record_samples = action_view_record_addrprog_samples("r29");
    const auto record_views = as_string_views(record_samples);
    const auto turn_r29_samples = action_view_pathing_turn_worksheet_addrprog_samples("r29");
    const auto turn_r29_views = as_string_views(turn_r29_samples);
    const auto turn_r26_samples = action_view_pathing_turn_worksheet_addrprog_samples("r26");
    const auto turn_r26_views = as_string_views(turn_r26_samples);

    const std::vector<std::string_view> outer_call_reg_memory = {
        "outer_input_x:r3:0x00:u32",
        "outer_input_y:r3:0x04:u32",
        "outer_input_z:r3:0x08:u32",
        "outer_path_x:r5:0x00:u32",
        "outer_path_y:r5:0x04:u32",
        "outer_path_z:r5:0x08:u32",
    };
    const std::vector<std::string_view> candidate_call_reg_memory = {
        "scan_input_x:r3:0x00:u32",
        "scan_input_y:r3:0x04:u32",
        "scan_input_z:r3:0x08:u32",
        "candidate_position_x:r4:0x00:u32",
        "candidate_position_y:r4:0x04:u32",
        "candidate_position_z:r4:0x08:u32",
        "scan_score_before:r1:0x08:u32",
        "scan_path_x:r6:0x00:u32",
        "scan_path_y:r6:0x04:u32",
        "scan_path_z:r6:0x08:u32",
        "candidate_slot_0x00:r25:0x00:u8",
        "candidate_flags_0xec:r25:0xec:u32",
        "candidate_flags_0xf0:r25:0xf0:u32",
        "candidate_extent_0x15c:r25:0x15c:u32",
    };
    const std::vector<std::string_view> candidate_return_reg_memory = {
        "scan_score_after:r1:0x08:u32",
        "candidate_slot_0x00:r25:0x00:u8",
        "candidate_flags_0xec:r25:0xec:u32",
        "candidate_flags_0xf0:r25:0xf0:u32",
        "candidate_extent_0x15c:r25:0x15c:u32",
    };

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_action_view_pathing_loop\n";
    out << "schema_version=1\n\n";

    // Dolphin reports this write after the seed has changed. The watchpoint
    // owns draw indexing; pathing checkpoints provide caller attribution.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    write_checkpoint(
        out,
        "setup_turn_action_entry_80082134",
        "80082134",
        "setup_turn_action_entry",
        "setupTurnAction_80082134",
        "capture_activation_boundary",
        false,
        memory_views,
        {"actor_slot_arg:3"},
        {},
        slot_views,
        {},
        16);

    write_checkpoint(
        out,
        "action_view_record_mode1_call_800514B0",
        "800514B0",
        "action_view_record_mode1_call",
        "Battle::Turn::UpdateActionViewRecord_80051264",
        "record_mode1_call",
        false,
        memory_views,
        {"record_thread:29", "record_worksheet:30", "origin_instruction:31"},
        {},
        record_views,
        {},
        32,
        0x80082134u);

    write_checkpoint(
        out,
        "pathing_outer_loop_entry_800526EC",
        "800526EC",
        "pathing_outer_loop_entry",
        "FUN_8005259c",
        "before_fun_8005174c",
        false,
        memory_views,
        {"turn_worksheet:29"},
        {},
        concat(turn_r29_views, slot_views),
        {},
        32,
        0x80082134u);

    const auto write_outer_call = [&](std::string_view id, std::string_view pc,
                                      std::string_view side) {
        write_checkpoint(
            out,
            id,
            pc,
            "pathing_outer_scan_call",
            "FUN_8005174c",
            side,
            false,
            memory_views,
            {
                "input_reference:3",
                "excluded_slot:4",
                "path_vector:5",
                "output_slot:6",
                "turn_worksheet:26",
                "target_slot:27",
                "actor_slot:28",
                "yaw_iteration:29",
                "actor_thread:30",
                "target_thread:31",
            },
            outer_call_reg_memory,
            turn_r26_views,
            {},
            384,
            0x80082134u);
    };
    write_outer_call("pathing_actor_scan_call_800518A8", "800518A8", "actor_side_scan");
    write_outer_call("pathing_target_scan_call_800518C4", "800518C4", "target_side_scan");

    write_checkpoint(
        out,
        "pathing_candidate_geometry_call_80011724",
        "80011724",
        "pathing_candidate_geometry_call",
        "FUN_80011694",
        "candidate_geometry_call",
        false,
        memory_views,
        {
            "input_reference:3",
            "candidate_position:4",
            "score_stack:5",
            "path_vector:6",
            "candidate_instruction:25",
            "candidate_index:26",
            "excluded_slot:27",
            "thread_cursor:28",
            "saved_input_reference:29",
            "saved_path_vector:30",
            "output_slot:31",
        },
        candidate_call_reg_memory,
        {},
        {},
        4096,
        0x80082134u);

    write_checkpoint(
        out,
        "pathing_candidate_geometry_return_80011728",
        "80011728",
        "pathing_candidate_geometry_return",
        "FUN_80011694",
        "candidate_geometry_return",
        false,
        memory_views,
        {
            "accepted_return:3",
            "candidate_instruction:25",
            "candidate_index:26",
            "excluded_slot:27",
            "thread_cursor:28",
            "saved_input_reference:29",
            "saved_path_vector:30",
            "output_slot:31",
        },
        candidate_return_reg_memory,
        {},
        {},
        4096,
        0x80082134u);

    const auto write_scan_outcome = [&](std::string_view id, std::string_view pc,
                                        std::string_view checkpoint) {
        write_checkpoint(
            out,
            id,
            pc,
            "pathing_scan_outcome",
            "FUN_80011694",
            checkpoint,
            false,
            memory_views,
            {
                "candidate_index_after_scan:26",
                "excluded_slot:27",
                "thread_cursor:28",
                "saved_input_reference:29",
                "saved_path_vector:30",
                "output_slot:31",
            },
            {},
            {},
            {},
            768,
            0x80082134u);
    };
    write_scan_outcome(
        "pathing_scan_zero_score_fallback_80011794",
        "80011794",
        "zero_score_fallback_rng_call");
    write_scan_outcome(
        "pathing_scan_nonzero_return_800117C4",
        "800117C4",
        "nonzero_deterministic_return");

    write_checkpoint(
        out,
        "pathing_outer_loop_return_800519F0",
        "800519F0",
        "pathing_outer_loop_return",
        "FUN_8005174c",
        "outer_loop_complete",
        false,
        memory_views,
        {"turn_worksheet:26", "yaw_iteration_after_loop:29"},
        {},
        turn_r26_views,
        {},
        32,
        0x80082134u);

    return out.str();
}

std::string build_first_battle_thread_pathing_timing_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    const auto memory_samples = thread_pathing_timing_memory_samples();
    const auto memory_views = as_string_views(memory_samples);
    const auto state_samples = thread_pathing_timing_state_addrprog_samples();
    const auto state_views = as_string_views(state_samples);
    const auto record_samples = action_view_record_addrprog_samples("r29");
    const auto record_views = as_string_views(record_samples);
    const auto turn_r29_samples =
        action_view_pathing_turn_worksheet_addrprog_samples("r29");
    const auto turn_r29_views = as_string_views(turn_r29_samples);
    const auto turn_r26_samples =
        action_view_pathing_turn_worksheet_addrprog_samples("r26");
    const auto turn_r26_views = as_string_views(turn_r26_samples);
    const auto thread_list_sample =
        pc_worker_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = {thread_list_sample};
    std::ostringstream out;
    out << "[profile]\n";
    out << "name=first_battle_thread_pathing_timing\n";
    out << "schema_version=1\n";
    out << "capture_only_hit_limit=131072\n\n";

    // Dolphin reports writes after the instruction has committed. The seed
    // watch owns draw indexing; the position watches are correlation-only.
    write_static_watchpoint(
        out,
        "rng_seed_write_803469A8",
        addr::AddrRegistry::base(addr::core::RNG_SEED),
        "u32",
        "write",
        "normal",
        true);

    for (int packed_index = 0; packed_index < 12; ++packed_index) {
        const auto packed_text = std::to_string(packed_index);
        const auto root =
            hex_u32(0x80309E24u + static_cast<std::uint32_t>(packed_index) * 4u);
        const auto worksheet = root + ":load_ptr32|+0x24|load_ptr32";
        write_dynamic_addrprog_watchpoint(
            out,
            "packed" + packed_text + "_cw_cur_x_write",
            "80082134",
            worksheet + "|+0x1c",
            "u32",
            "write",
            "normal");
        write_dynamic_addrprog_watchpoint(
            out,
            "packed" + packed_text + "_cw_cur_z_write",
            "80082134",
            worksheet + "|+0x24",
            "u32",
            "write",
            "normal");
    }

    write_checkpoint(
        out,
        "setup_turn_action_entry_80082134",
        "80082134",
        "setup_turn_action_entry",
        "FUN_80082134",
        "capture_activation_and_position_watch_derivation",
        false,
        memory_views,
        {"actor_slot:3", "target_slot:4"},
        {},
        state_views,
        thread_list_views,
        32);

    write_checkpoint(
        out,
        "battle_case5_after_threads_8000A2FC",
        "8000A2FC",
        "battle_case5_after_threads",
        "Battle::_battleController_8000a118",
        "case5_after_runBattleThreads",
        false,
        memory_views,
        {},
        {},
        state_views,
        thread_list_views,
        2400,
        0x80082134u);

    write_checkpoint(
        out,
        "action_view_record_mode1_call_800514B0",
        "800514B0",
        "action_view_record_mode1_call",
        "Battle::Turn::UpdateActionViewRecord_80051264",
        "record_mode1_call",
        false,
        memory_views,
        {"record_thread:29", "record_worksheet:30", "origin_instruction:31"},
        {},
        concat(record_views, state_views),
        thread_list_views,
        64,
        0x80082134u);

    write_checkpoint(
        out,
        "pathing_outer_loop_entry_800526EC",
        "800526EC",
        "pathing_outer_loop_entry",
        "FUN_8005259c",
        "before_fun_8005174c",
        false,
        memory_views,
        {"turn_worksheet:29"},
        {},
        concat(turn_r29_views, state_views),
        thread_list_views,
        64,
        0x80082134u);

    const auto write_pathing_scan = [&](std::string_view id,
                                        std::string_view pc,
                                        std::string_view side) {
        write_checkpoint(
            out,
            id,
            pc,
            "pathing_outer_scan_call",
            "FUN_8005174c",
            side,
            false,
            memory_views,
            {
                "input_reference:3",
                "excluded_slot:4",
                "path_vector:5",
                "output_slot:6",
                "turn_worksheet:26",
                "target_slot:27",
                "actor_slot:28",
                "yaw_iteration:29",
                "actor_thread:30",
                "target_thread:31",
            },
            {
                "outer_input_x:r3:0x00:u32",
                "outer_input_y:r3:0x04:u32",
                "outer_input_z:r3:0x08:u32",
                "outer_path_x:r5:0x00:u32",
                "outer_path_y:r5:0x04:u32",
                "outer_path_z:r5:0x08:u32",
            },
            concat(turn_r26_views, state_views),
            thread_list_views,
            768,
            0x80082134u);
    };
    write_pathing_scan(
        "pathing_actor_scan_call_800518A8", "800518A8", "actor_side_scan");
    write_pathing_scan(
        "pathing_target_scan_call_800518C4", "800518C4", "target_side_scan");

    write_checkpoint(
        out,
        "pathing_scan_zero_score_fallback_80011794",
        "80011794",
        "pathing_scan_zero_score_fallback",
        "FUN_80011694",
        "zero_score_fallback_rng_call",
        false,
        memory_views,
        {
            "candidate_index_after_scan:26",
            "excluded_slot:27",
            "thread_cursor:28",
            "saved_input_reference:29",
            "saved_path_vector:30",
            "output_slot:31",
        },
        {},
        {},
        {},
        2048,
        0x80082134u);

    write_checkpoint(
        out,
        "movement_commit_entry_8008178C",
        "8008178C",
        "movement_commit_entry",
        "FUN_8008178c",
        "movement_grid_and_posholder_commit",
        false,
        memory_views,
        {
            "movement_worksheet:3",
            "next_grid_x:4",
            "next_grid_z:5",
            "slot:6",
        },
        movement_commit_samples_from_r3(),
        state_views,
        thread_list_views,
        1024,
        0x80082134u);

    write_checkpoint(
        out,
        "action_motion_target_return_8001FADC",
        "8001FADC",
        "action_motion_target_return",
        "FUN_8001fabc",
        "action_motion_target_selected",
        false,
        memory_views,
        {"target_helper_result:3", "combatant_worksheet:30", "instruction_worksheet:31"},
        {
            "target_vector_x_bits:r1:0x08:u32",
            "target_vector_y_bits:r1:0x0c:u32",
            "target_vector_z_bits:r1:0x10:u32",
        },
        state_views,
        {},
        2048,
        0x80082134u);
    write_checkpoint(
        out,
        "action_motion_setup_complete_8001FC04",
        "8001FC04",
        "action_motion_setup_complete",
        "FUN_8001fabc",
        "action_motion_increment_ready",
        false,
        memory_views,
        {"combatant_worksheet:30", "instruction_worksheet:31"},
        float_motion_instruction_samples_from_r31(),
        state_views,
        {},
        2048,
        0x80082134u);

    for (const auto& checkpoint : std::array{
             std::tuple{"action_motion_final_result_8001EB54", "8001EB54",
                        "action_motion_final_result"},
             std::tuple{"action_motion_caller_consumption_8001B778", "8001B778",
                        "action_motion_caller_consumption"}}) {
        write_checkpoint(
            out,
            std::get<0>(checkpoint),
            std::get<1>(checkpoint),
            std::get<2>(checkpoint),
            "action_motion_controller",
            "action_motion_stop_chain",
            false,
            memory_views,
            {
                "result_r3:3",
                "motion_context_r27:27",
                "motion_context_r28:28",
                "motion_context_r29:29",
                "motion_context_r30:30",
                "motion_context_r31:31",
            },
            {},
            state_views,
            {},
            4096,
            0x80082134u);
    }

    return out.str();
}

std::string build_battle_thread_producer_profile_ini(
    std::uint32_t thread_list_max_nodes)
{
    std::vector<std::string> memory_samples = {
        memory_sample("thread_list_head", 0x80311A84u, "u32"),
        memory_sample("thread_runner_current", 0x80311A7Cu, "u32"),
        memory_sample("candidate_resource_rows", 0x80347398u, "u32"),
        memory_sample("resource_reference_list", 0x8034739Cu, "u32"),
        memory_sample("combatant_load_context", 0x8030A114u, "u32"),
        memory_sample("battle_turn_phase", 0x80347340u, "u8"),
    };
    std::vector<std::string> addrprog_samples;
    for (int index = 0; index < 12; ++index) {
        const auto suffix = std::to_string(index);
        const auto movement_root = hex_u32(
            0x80309700u + static_cast<std::uint32_t>(index) * 4u);
        const auto instruction_root = hex_u32(
            0x80309E24u + static_cast<std::uint32_t>(index) * 4u);
        memory_samples.push_back(
            "movement_root" + suffix + ":" + movement_root + ":u32");
        memory_samples.push_back(
            "instruction_root" + suffix + ":" + instruction_root + ":u32");

        const auto movement_thread = movement_root + ":load_ptr32";
        const auto movement_worksheet = movement_thread + "|+0x24|load_ptr32";
        addrprog_samples.push_back(
            "movement_root" + suffix + "_callback:" + movement_thread + "|+0x00:u32");
        addrprog_samples.push_back(
            "movement_root" + suffix + "_payload:" + movement_thread + "|+0x24:u32");
        addrprog_samples.push_back(
            "movement_root" + suffix + "_owner_slot:" + movement_worksheet + "|+0x00:u16");
        addrprog_samples.push_back(
            "movement_root" + suffix + "_combatant_id:" + movement_worksheet + "|+0x02:u16");

        const auto instruction_thread = instruction_root + ":load_ptr32";
        const auto command_worksheet = instruction_thread + "|+0x24|load_ptr32";
        const auto instruction_worksheet = command_worksheet + "|+0x4c|load_ptr32";
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_callback:" + instruction_thread + "|+0x00:u32");
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_payload:" + instruction_thread + "|+0x24:u32");
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_owner_slot:" + instruction_worksheet + "|+0x00:u8");
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_resource_id:" + instruction_worksheet + "|+0x02:u16");
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_action_rows:" + instruction_worksheet + "|+0xdc:u32");
        addrprog_samples.push_back(
            "instruction_root" + suffix + "_load_context:" + instruction_worksheet + "|+0x1dc:u32");
    }
    const auto memory_views = as_string_views(memory_samples);
    const auto addrprog_views = as_string_views(addrprog_samples);
    const auto thread_list_sample =
        pc_worker_thread_list_snapshot_sample(thread_list_max_nodes);
    const std::vector<std::string_view> thread_list_views = {thread_list_sample};

    std::ostringstream out;
    out << "[profile]\n";
    out << "name=battle_thread_producer\n";
    out << "schema_version=1\n\n";

    write_static_watchpoint(
        out, "thread_list_head_write_80311A84", 0x80311A84u,
        "u32", "write", "normal");
    for (int index = 0; index < 12; ++index) {
        write_static_watchpoint(
            out,
            "movement_root" + std::to_string(index) + "_write",
            0x80309700u + static_cast<std::uint32_t>(index) * 4u,
            "u32",
            "write",
            "normal");
        write_static_watchpoint(
            out,
            "instruction_root" + std::to_string(index) + "_write",
            0x80309E24u + static_cast<std::uint32_t>(index) * 4u,
            "u32",
            "write",
            "normal");
    }

    const auto write_producer_checkpoint =
        [&out, &memory_views, &addrprog_views, &thread_list_views](
            std::string_view id,
            std::string_view pc,
            std::string_view name,
            std::string_view function,
            std::string_view checkpoint,
            std::initializer_list<std::string_view> gprs,
            std::uint32_t max_hits) {
            const std::vector<std::string_view> gpr_views(gprs);
            write_checkpoint(
                out,
                id,
                pc,
                name,
                function,
                checkpoint,
                false,
                memory_views,
                gpr_views,
                {},
                addrprog_views,
                thread_list_views,
                max_hits);
        };

    write_producer_checkpoint(
        "battle_case5_after_threads_8000A2FC", "8000A2FC",
        "battle_case5_after_threads", "Battle::_battleController_8000a118",
        "thread_frame_boundary", {}, 2400);

    write_producer_checkpoint(
        "setup_grid_combatant_call_800849D8", "800849D8",
        "setup_grid_combatant_call", "setupGridAndCombatants_800849a8",
        "movement_controller_create_before", {"slot:31"}, 24);
    write_producer_checkpoint(
        "setup_grid_combatant_return_800849DC", "800849DC",
        "setup_grid_combatant_return", "setupGridAndCombatants_800849a8",
        "movement_controller_create_after", {"slot:31"}, 24);
    write_producer_checkpoint(
        "setup_combatant_entry_800842A0", "800842A0",
        "setup_combatant_entry", "setupCombatant_800842a0",
        "movement_controller_create_entry", {"slot:3"}, 24);
    write_producer_checkpoint(
        "setup_combatant_mkchild_call_80084300", "80084300",
        "setup_combatant_mkchild_call", "setupCombatant_800842a0",
        "movement_controller_mkchild_before", {"slot:27", "parent:3", "callback:4"}, 24);
    write_producer_checkpoint(
        "setup_combatant_mkchild_return_80084304", "80084304",
        "setup_combatant_mkchild_return", "setupCombatant_800842a0",
        "movement_controller_mkchild_after", {"slot:27", "thread:3"}, 24);
    write_producer_checkpoint(
        "setup_combatant_return_8008456C", "8008456C",
        "setup_combatant_return", "setupCombatant_800842a0",
        "movement_controller_create_complete", {"slot:27"}, 24);

    write_producer_checkpoint(
        "load_movement_std_entry_80030280", "80030280",
        "load_movement_std_entry", "STD::LoadMovementStdResourceById_80030280",
        "resource_identity_lookup", {"resource_id:3"}, 128);
    write_producer_checkpoint(
        "load_combatant_std_pair_call_80030384", "80030384",
        "load_combatant_std_pair_call", "STD::LoadMovementStdResourceById_80030280",
        "std_resource_publication_before", {"file_dir:3", "mld_name:4", "slot_index:25"}, 128);
    write_producer_checkpoint(
        "load_combatant_std_pair_return_80030388", "80030388",
        "load_combatant_std_pair_return", "STD::LoadMovementStdResourceById_80030280",
        "std_resource_publication_after", {"slot_index:25"}, 128);
    write_producer_checkpoint(
        "load_combatant_std_pair_entry_80021934", "80021934",
        "load_combatant_std_pair_entry", "LoadCombatantStdResourcePair_80021934",
        "std_resource_pair_entry", {"file_dir:3", "resource_name:4"}, 128);

    write_producer_checkpoint(
        "create_std_combatant_thread_entry_8001FE44", "8001FE44",
        "create_std_combatant_thread_entry", "CreateStdActionCombatantThread_8001fe44",
        "instruction_thread_create_entry", {"resource_record:3"}, 128);
    write_producer_checkpoint(
        "create_std_mkchild_call_8001FEB4", "8001FEB4",
        "create_std_mkchild_call", "CreateStdActionCombatantThread_8001fe44",
        "instruction_thread_mkchild_before", {"parent:3", "callback:4", "load_context:26"}, 128);
    write_producer_checkpoint(
        "create_std_mkchild_return_8001FEB8", "8001FEB8",
        "create_std_mkchild_return", "CreateStdActionCombatantThread_8001fe44",
        "instruction_thread_mkchild_after", {"thread:3", "load_context:26"}, 128);
    write_producer_checkpoint(
        "create_std_root_publication_8001FFB8", "8001FFB8",
        "create_std_root_publication", "CreateStdActionCombatantThread_8001fe44",
        "instruction_root_publication", {"thread:31", "load_context:26"}, 128);
    write_producer_checkpoint(
        "create_std_combatant_thread_return_8002005C", "8002005C",
        "create_std_combatant_thread_return", "CreateStdActionCombatantThread_8001fe44",
        "instruction_thread_create_complete", {"thread:31", "load_context:26"}, 128);
    write_producer_checkpoint(
        "mkchild_entry_802268E8", "802268E8",
        "mkchild_entry", "mkChildMenu_802268e8",
        "thread_list_insert_entry", {"parent:3", "callback:4"}, 512);
    write_producer_checkpoint(
        "thread_remove_before_unlink_80226610", "80226610",
        "thread_remove_before_unlink", "Thread::run_threads_8022642c",
        "thread_remove_before", {"current_thread:30"}, 512);
    write_producer_checkpoint(
        "thread_remove_after_unlink_80226628", "80226628",
        "thread_remove_after_unlink", "Thread::run_threads_8022642c",
        "thread_remove_after_unlink", {"removed_thread:30"}, 512);
    write_producer_checkpoint(
        "thread_remove_after_free_8022662C", "8022662C",
        "thread_remove_after_free", "Thread::run_threads_8022642c",
        "thread_remove_complete", {"removed_thread:30"}, 512);

    for (const auto& checkpoint : std::array{
             std::tuple{"resource_queue_worker_entry_8006C124", "8006C124", "resource_queue_worker_entry"},
             std::tuple{"resource_queue_start_8006DC6C", "8006DC6C", "resource_queue_start"},
             std::tuple{"resource_queue_completion_8006E75C", "8006E75C", "resource_queue_completion"},
             std::tuple{"character_mlk_queue_entry_80073164", "80073164", "character_mlk_queue_entry"}}) {
        write_producer_checkpoint(
            std::get<0>(checkpoint),
            std::get<1>(checkpoint),
            std::get<2>(checkpoint),
            std::get<2>(checkpoint),
            "resource_queue_transition",
            {"arg0:3", "arg1:4", "arg2:5"},
            512);
    }

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

int write_first_battle_view_eligibility_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-view-eligibility-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_view_eligibility_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle view-eligibility capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_view_placement_cache_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-view-placement-cache-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_view_placement_cache_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle view-placement cache capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_view_placement_frame_thread_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-view-placement-frame-thread-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-view-placement-frame-thread-profile --list-max must be 128 or 256.\n";
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
    const auto text = build_first_battle_view_placement_frame_thread_profile_ini(thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle view-placement frame/thread capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_view_placement_semantic_hooks_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-view-placement-semantic-hooks-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-view-placement-semantic-hooks-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_view_placement_semantic_hooks_profile_ini(thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle view-placement semantic-hooks capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_predictor_live_comparison_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-predictor-live-comparison-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-predictor-live-comparison-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_predictor_live_comparison_profile_ini(thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle predictor/live comparison capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_movement_destination_stop_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-movement-destination-stop-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-movement-destination-stop-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_movement_destination_stop_profile_ini(thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle movement destination/stop capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_action_view_service_lifecycle_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-action-view-service-lifecycle-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-action-view-service-lifecycle-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_action_view_service_lifecycle_profile_ini(thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle action-view/service lifecycle capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_pc_worker_selector_lifetime_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-pc-worker-selector-lifetime-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-pc-worker-selector-lifetime-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_pc_worker_selector_lifetime_profile_ini(
            thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle PC worker selector/lifetime capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_action_view_pathing_loop_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err)
{
    if (output_path.empty()) {
        err << "write-first-battle-action-view-pathing-loop-profile requires --output PATH.\n";
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
    const auto text = build_first_battle_action_view_pathing_loop_profile_ini();
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle action-view pathing-loop capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_first_battle_thread_pathing_timing_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-first-battle-thread-pathing-timing-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-first-battle-thread-pathing-timing-profile --list-max must be 128 or 256.\n";
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
    const auto text =
        build_first_battle_thread_pathing_timing_profile_ini(
            thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote first-battle thread/pathing timing capture profile: "
        << output_path.string() << "\n";
    return 0;
}

int write_battle_thread_producer_profile(
    const std::filesystem::path& output_path,
    std::ostream& out,
    std::ostream& err,
    std::uint32_t thread_list_max_nodes)
{
    if (output_path.empty()) {
        err << "write-battle-thread-producer-profile requires --output PATH.\n";
        return 2;
    }
    if (thread_list_max_nodes != 128 && thread_list_max_nodes != 256) {
        err << "write-battle-thread-producer-profile --list-max must be 128 or 256.\n";
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
    const auto text = build_battle_thread_producer_profile_ini(
        thread_list_max_nodes);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!file.good()) {
        err << "Failed to write output profile: " << output_path.string() << "\n";
        return 1;
    }
    out << "Wrote battle thread-producer capture profile: "
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
