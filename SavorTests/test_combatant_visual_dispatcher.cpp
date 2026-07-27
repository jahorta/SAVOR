#include "../SavorPredict/ActionViewStdJsonLoader.h"
#include "../SavorPredict/BattleFrameSchedulerModel.h"
#include "../SavorPredict/BattleSourceModel.h"
#include "../SavorPredict/CombatantAuxiliaryPublicationModel.h"
#include "../SavorPredict/RngCore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace savor::predict;

const BattleSourceSnapshot& first_battle_source_snapshot() {
    static const auto source = load_battle_source_bundle(
        "first-battle-soldiers-us-final");
    if (!source.ok) {
        throw std::runtime_error(
            source.errors.empty()
                ? "first-battle source bundle failed to load"
                : source.errors.front());
    }
    return source.snapshot;
}

void write_u16_be(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void write_u32_be(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value >> 24U);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[offset + 3] = static_cast<std::uint8_t>(value);
}

std::string bytes_hex(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto byte : bytes) {
        out << std::setw(2) << static_cast<unsigned>(byte);
    }
    return out.str();
}

std::vector<std::uint8_t> set_command_payload(
    std::int16_t delay = 2,
    std::int16_t action_key = 4) {
    std::vector<std::uint8_t> payload(0x1c, 0);
    write_u16_be(payload, 0x00, static_cast<std::uint16_t>(action_key));
    write_u16_be(payload, 0x04, 0xffffU);
    write_u32_be(payload, 0x10, 0x00100020U);
    write_u16_be(payload, 0x14, static_cast<std::uint16_t>(delay));
    write_u16_be(payload, 0x16, 0x000dU);
    return payload;
}

std::vector<std::uint8_t> system_camera_payload(
    std::int16_t mode = 0x0e,
    std::int16_t action_key = 4) {
    std::vector<std::uint8_t> payload(0x24, 0);
    write_u16_be(payload, 0x00, static_cast<std::uint16_t>(action_key));
    write_u16_be(payload, 0x04, 0xffffU);
    write_u32_be(payload, 0x10, 0x12345678U);
    write_u32_be(payload, 0x14, 0x3f800000U);
    write_u32_be(payload, 0x18, 0);
    write_u16_be(payload, 0x1c, 0x0460U);
    write_u16_be(payload, 0x1e, 3);
    write_u16_be(payload, 0x20, 1);
    write_u16_be(payload, 0x22, static_cast<std::uint16_t>(mode));
    return payload;
}

std::vector<std::uint8_t> collision_box_payload(
    std::int16_t action_key = 4) {
    std::vector<std::uint8_t> payload(0x38, 0);
    write_u16_be(payload, 0x00, static_cast<std::uint16_t>(action_key));
    write_u16_be(payload, 0x04, 0xffffU);
    write_u32_be(payload, 0x10, 5U);
    write_u16_be(payload, 0x14, 24U);
    write_u16_be(payload, 0x16, 998U);
    write_u16_be(payload, 0x18, 83U);
    write_u32_be(payload, 0x1c, 0x00000000U);
    write_u32_be(payload, 0x20, 0x40F66658U);
    write_u32_be(payload, 0x24, 0x415851B6U);
    write_u32_be(payload, 0x28, 0x00000000U);
    write_u32_be(payload, 0x2c, 0x00000000U);
    write_u32_be(payload, 0x30, 0x410FFFFDU);
    write_u32_be(payload, 0x34, 0U);
    return payload;
}

std::string collision_visual_json(const std::vector<std::uint8_t>& payload) {
    std::ostringstream json;
    json << "{\"schema\": \"spice_std_ir_v1\","
         << "\"layoutKind\": \"entry_table\",\"parseOk\": true,"
         << "\"entryTable\": {\"records\": ["
         << "{\"index\":0,\"locationCode\":11,\"opcode\":3,"
         << "\"payloadSize\":" << payload.size()
         << ",\"payloadInBounds\":true,\"payloadBytesHex\":\""
         << bytes_hex(payload) << "\"},"
         << "{\"index\":1,\"locationCode\":-1,\"opcode\":0,"
         << "\"payloadSize\":0,\"payloadInBounds\":true,"
         << "\"payloadBytesHex\":\"\"}]}}";
    return json.str();
}

std::vector<std::uint8_t> action_motion_delay_descriptor_payload(
    std::int16_t delay = 13,
    std::int16_t action_key = 5) {
    std::vector<std::uint8_t> payload(0x12, 0);
    write_u16_be(payload, 0x00, static_cast<std::uint16_t>(action_key));
    write_u16_be(payload, 0x02, 1);
    write_u16_be(payload, 0x04, 0);
    write_u16_be(payload, 0x10, static_cast<std::uint16_t>(delay));
    return payload;
}

std::string visual_json(
    const std::vector<std::uint8_t>& set_payload,
    const std::vector<std::uint8_t>& camera_payload,
    const std::optional<std::vector<std::uint8_t>>& delay_descriptor = std::nullopt) {
    std::ostringstream json;
    json << "{\"schema\": \"spice_std_ir_v1\","
         << "\"layoutKind\": \"entry_table\",\"parseOk\": true,"
         << "\"entryTable\": {\"records\": ["
         << "{\"index\":0,\"locationCode\":4,\"opcode\":3,"
         << "\"payloadSize\":" << set_payload.size()
         << ",\"payloadInBounds\":true,\"payloadBytesHex\":\""
         << bytes_hex(set_payload) << "\"},"
         << "{\"index\":1,\"locationCode\":42,\"opcode\":3,"
         << "\"payloadSize\":" << camera_payload.size()
         << ",\"payloadInBounds\":true,\"payloadBytesHex\":\""
         << bytes_hex(camera_payload) << "\"}";
    if (delay_descriptor.has_value()) {
        json << ",{\"index\":2,\"locationCode\":50,\"opcode\":3,"
             << "\"payloadSize\":" << delay_descriptor->size()
             << ",\"payloadInBounds\":true,\"payloadBytesHex\":\""
             << bytes_hex(*delay_descriptor) << "\"}";
    }
    const int sentinel_index = delay_descriptor.has_value() ? 3 : 2;
    json << ",{\"index\":" << sentinel_index << ",\"locationCode\":-1,\"opcode\":0,"
         << "\"payloadSize\":0,\"payloadInBounds\":true,"
         << "\"payloadBytesHex\":\"\"}]}}";
    return json.str();
}

CombatantVisualResource decoded_resource(
    int slot,
    bool mode0_rewrite_gate = false,
    std::int16_t camera_mode = 0x0e,
    std::int16_t delay = 2,
    std::int16_t action_key = 5,
    bool include_action_motion_delay_descriptor = false) {
    const std::optional<std::vector<std::uint8_t>> descriptor =
        include_action_motion_delay_descriptor
        ? std::optional<std::vector<std::uint8_t>>{
            action_motion_delay_descriptor_payload(13, action_key)}
        : std::nullopt;
    auto loaded = load_spice_std_visual_resource_from_json_text(
        visual_json(
            set_command_payload(delay, action_key),
            system_camera_payload(camera_mode, action_key),
            descriptor));
    EXPECT_TRUE(loaded.ok);
    loaded.resource.binding = {
        .slot = slot,
        .resource_stem = "fixture",
        .mode0_rewrite_gate = mode0_rewrite_gate,
    };
    loaded.resource.action_rows = {
        {.index = 0, .action_id = 0, .row_type = 0,
         .callback_index = 0, .callback_ordinal = 0,
         .transition_gate_divisor_bits = 0x3f800000u,
         .motion_progress_step_bits = 0x41b00000u},
        {.index = 1, .action_id = 1, .row_type = 1,
         .callback_index = 9, .callback_ordinal = 1,
         .transition_gate_divisor_bits = 0x3e99999au},
        {.index = 2, .action_id = 2, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 0,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 3, .action_id = 4, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 3,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 4, .action_id = 5, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 6,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 5, .action_id = 6, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 2,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 6, .action_id = 8, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 3,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 7, .action_id = 11, .row_type = 1,
         .callback_index = 10, .callback_ordinal = 0,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 8, .action_id = 0x13, .row_type = 1,
         .callback_index = 8, .callback_ordinal = 14,
         .transition_gate_divisor_bits = 0x40a00000u},
        {.index = 9, .action_id = -1, .row_type = 3},
    };
    return std::move(loaded.resource);
}

std::vector<MovementSlotState> frame_slots() {
    std::vector<MovementSlotState> slots;
    for (const int slot : {0, 1, 4, 5}) {
        MovementSlotState state;
        state.slot = slot;
        state.present = true;
        state.is_player = slot < 4;
        state.alive = true;
        state.movement_flags = slot == 1 ? 0x0ff7 : 0x0fc7;
        state.motion_base_speed = state.is_player ? 2.55f : 2.25f;
        state.motion_alt_speed = state.is_player ? 0.45f : 0.30f;
        state.motion_speeds_known = true;
        state.motion_turn_speed = 22.0f;
        state.motion_turn_speed_known = true;
        const auto placement = battle_source_placement_for_slot(
            first_battle_source_snapshot(), slot);
        if (placement.has_value()) {
            state.start_position = BattleStartPosition{
                .slot = placement->slot,
                .present = true,
                .is_player = placement->is_player,
                .combatant_id = placement->combatant_id,
                .combatant_name = placement->combatant_name,
                .grid_x = placement->grid_x,
                .grid_z = placement->grid_z,
            };
        }
        slots.push_back(state);
    }
    return slots;
}

std::optional<BattleFrameRuntime> initialize_frame_runtime() {
    auto runtime = initialize_first_battle_frame_runtime(
        0,
        frame_slots(),
        first_battle_source_snapshot().terrain_source_9x9);
    if (!runtime.has_value()) {
        return std::nullopt;
    }
    for (auto& combatant : runtime->state.combatants) {
        const auto created = create_battle_frame_thread(
            runtime->thread_list,
            BattleFrameThreadCreateRequest{
                .kind = BattleFrameThreadNodeKind::CombatantInstruction,
                .owner_slot = combatant.slot,
                .callback = BattleFrameThreadCallbackIdentity::CombatantInstruction,
                .active = true,
                .semantic_source_id = "test.std_resource.publication",
                .provenance = "visual-runtime fixture",
            });
        if (created.status != BattleFrameThreadMutationStatus::Applied) {
            return std::nullopt;
        }
        combatant.selected_action_row_index = 4;
        combatant.selected_action_row_flags = 0x01000000u;
        combatant.selected_action_row_action_id = 5;
        combatant.selected_action_row_callback_index = 8;
        combatant.selected_action_row_callback_ordinal = 6;
        combatant.selected_action_row_known = true;
        combatant.selected_action_row_duration_bits = 0x40a00000u;
        combatant.selected_action_row_duration_known = true;
    }
    return runtime;
}

bool publish_fixture_visual_instruction_state(
    BattleFrameRuntime& runtime,
    int slot = 0,
    int action_ordinal = 0,
    std::int16_t mode = 5,
    bool invoke_action_motion_playback = false) {
    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant == nullptr) {
        return false;
    }
    runtime.visual.timelines[static_cast<std::size_t>(slot)] = {};
    runtime.visual.timeline_action_ordinals[static_cast<std::size_t>(slot)] = -1;
    runtime.visual.controller = {};
    for (auto& worker : runtime.workers) {
        if (worker.slot == slot && worker.action_ordinal == action_ordinal) {
            worker.complete = true;
            worker.waiting_for_combatant_instruction = false;
        }
    }
    runtime.combatant_instructions[static_cast<std::size_t>(slot)] = {};
    runtime.movement_controller_states[static_cast<std::size_t>(slot)] =
        BattleMovementControllerState::Idle;
    combatant->selected_action_row_action_id = mode;
    CombatantVisualInstructionSnapshot instruction;
    instruction.slot = slot;
    instruction.runtime_instruction_mode = mode;
    instruction.selected_std_action_key = mode;
    instruction.target_slot = 4;
    instruction.subtype = -1;
    instruction.instruction_flags = combatant->instruction_flags_0xec;
    instruction.knowledge = CombatantVisualInstructionKnowledge::Known;
    instruction.provenance =
        "low-level visual runtime fixture publishes persistent IW state";
    auto& callback_runtime =
        runtime.visual.persistent_instruction_callbacks[
            static_cast<std::size_t>(slot)];
    callback_runtime.thread_state_0x19 = 1;
    runtime.visual.std_row_producers[static_cast<std::size_t>(slot)]
        .thread_state_0x19 = 1;
    const bool staged = stage_battle_frame_visual_instruction_state(
        runtime,
        action_ordinal,
        std::move(instruction));
    const bool published = staged
        && publish_battle_frame_persistent_instruction_callback(
            runtime,
            slot,
            BattleFrameInstructionCallbackPublicationSource::
                ExplicitModeledTransition,
            "low-level fixture executes the modeled param4=0 publisher");
    if (published && !invoke_action_motion_playback) {
        callback_runtime.current_motion_resource_present = true;
        callback_runtime.current_motion_id =
            combatant->selected_action_row_callback_ordinal;
        callback_runtime.callback_state = 9;
        callback_runtime.state8_delay_remaining = 1;
        callback_runtime.auxiliary_publication_pending = true;
        callback_runtime.provenance +=
            "; fixture starts one state-9 visit before the audited state-10 boundary so the source epoch is produced independently";
    }
    return published;
}

BattleFrameActionScheduleResult schedule_action(
    BattleFrameRuntime& runtime,
    bool automatic_basic_attack_transition = true) {
    return schedule_first_turn_actor_action(
        runtime,
        BattleFrameScheduleActionInput{
            .action_ordinal = 0,
            .actor_slot = 0,
            .target_slot = 4,
            .enemy_owned = false,
            .selected_worker = MovementSelectedWorker::PcDirectAttack_80086308,
            .action_kind = automatic_basic_attack_transition
                ? BattleMovementActionKind::BasicAttack
                : BattleMovementActionKind::Guard,
            .relation_scope = BattleMovementRelationScope::SingleTarget,
            .turn_type = BattleMovementTurnType::Normal,
        });
}

int count_step(
    const std::vector<BattleFrameStepEvent>& events,
    BattleFrameWorkerStepKind kind) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [kind](const BattleFrameStepEvent& event) { return event.step_kind == kind; }));
}

int count_step_for_command(
    const std::vector<BattleFrameStepEvent>& events,
    BattleFrameWorkerStepKind kind,
    CombatantVisualCommandKind command_kind) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [kind, command_kind](const BattleFrameStepEvent& event) {
            return event.step_kind == kind
                && event.visual_command_kind == command_kind;
        }));
}

int count_non_synthetic_publications(
    const std::vector<BattleFrameStepEvent>& events) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualCommandPublish
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        }));
}

int count_rng_label(
    const std::vector<BattleFrameStepEvent>& events,
    const std::string& label) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [&label](const BattleFrameStepEvent& event) { return event.rng_label == label; }));
}

int active_thread_visit_cursor(
    const BattleFrameRuntime& runtime,
    int node_id) {
    int cursor = 1;
    for (const auto& node : runtime.thread_list.nodes) {
        if (!node.active) {
            continue;
        }
        if (node.node_id == node_id) {
            return cursor;
        }
        ++cursor;
    }
    return -1;
}

int instruction_thread_node_id(
    const BattleFrameRuntime& runtime,
    int slot) {
    const auto found = std::find_if(
        runtime.thread_list.nodes.begin(),
        runtime.thread_list.nodes.end(),
        [slot](const BattleFrameThreadNode& node) {
            return node.active
                && node.kind == BattleFrameThreadNodeKind::CombatantInstruction
                && node.owner_slot == slot;
        });
    return found == runtime.thread_list.nodes.end() ? -1 : found->node_id;
}

void add_direct_transition_rows(CombatantVisualResource& resource) {
    const auto sentinel = std::find_if(
        resource.action_rows.begin(),
        resource.action_rows.end(),
        [](const CombatantStdActionRow& row) { return row.row_type == 3; });
    resource.action_rows.insert(sentinel, {
        .index = 10,
        .action_id = BattleFrameActionMode::PassiveBlock,
        .row_type = 1,
        .callback_index = 8,
        .callback_ordinal = 3,
        .transition_gate_divisor_bits = 0x40a00000u,
    });
    const auto updated_sentinel = std::find_if(
        resource.action_rows.begin(),
        resource.action_rows.end(),
        [](const CombatantStdActionRow& row) { return row.row_type == 3; });
    resource.action_rows.insert(updated_sentinel, {
        .index = 11,
        .action_id = BattleFrameActionMode::PassiveDodge,
        .row_type = 1,
        .callback_index = 8,
        .callback_ordinal = 3,
        .transition_gate_divisor_bits = 0x40a00000u,
    });
    for (const std::int16_t action_id : {12, 13}) {
        const auto row_sentinel = std::find_if(
            resource.action_rows.begin(),
            resource.action_rows.end(),
            [](const CombatantStdActionRow& row) { return row.row_type == 3; });
        resource.action_rows.insert(row_sentinel, {
            .index = action_id,
            .action_id = action_id,
            .row_type = 1,
            .callback_index = 8,
            .callback_ordinal = 3,
            .transition_gate_divisor_bits = 0x40a00000u,
        });
    }
}

CombatantVisualResource decoded_collision_resource(
    int slot,
    std::int16_t start_counter = 0,
    std::int16_t end_counter = 50) {
    auto payload = collision_box_payload(4);
    write_u16_be(payload, 0x14, static_cast<std::uint16_t>(start_counter));
    write_u16_be(payload, 0x16, static_cast<std::uint16_t>(end_counter));
    write_u32_be(payload, 0x1c, 0x00000000U);
    write_u32_be(payload, 0x20, 0x00000000U);
    write_u32_be(payload, 0x24, 0x41700000U);
    write_u32_be(payload, 0x28, 0x00000000U);
    write_u32_be(payload, 0x2c, 0x00000000U);
    write_u32_be(payload, 0x30, 0x00000000U);
    auto loaded = load_spice_std_visual_resource_from_json_text(
        collision_visual_json(payload));
    EXPECT_TRUE(loaded.ok);
    loaded.resource.binding = {
        .slot = slot,
        .resource_stem = "collision_fixture",
    };
    loaded.resource.action_rows = decoded_resource(slot).action_rows;
    return std::move(loaded.resource);
}

std::optional<BattleFrameRuntime> direct_reset_runtime(
    bool producer_before_target) {
    auto runtime = initialize_frame_runtime();
    if (!runtime.has_value()) {
        return std::nullopt;
    }
    auto target_resource = decoded_resource(4, false, 2);
    add_direct_transition_rows(target_resource);
    if (!configure_battle_frame_visual_resource(
            *runtime, std::move(target_resource))
        || !publish_fixture_visual_instruction_state(*runtime, 4, -1, 5)) {
        return std::nullopt;
    }
    runtime->visual.persistent_instruction_callbacks[4].callback_state = 15;
    runtime->visual.pending_events.clear();
    runtime->visual.history.clear();
    runtime->visual.instruction_control_reset_history.clear();

    const int target_node = instruction_thread_node_id(*runtime, 4);
    const int relative_node = producer_before_target
        ? instruction_thread_node_id(*runtime, 1)
        : -1;
    const auto producer = create_battle_frame_thread(
        runtime->thread_list,
        BattleFrameThreadCreateRequest{
            .kind = BattleFrameThreadNodeKind::ResourceWorker,
            .owner_slot = producer_before_target ? 100 : 101,
            .callback = BattleFrameThreadCallbackIdentity::ResourceQueue,
            .active = true,
            .insertion = producer_before_target
                ? BattleFrameThreadInsertionKind::AfterNode
                : BattleFrameThreadInsertionKind::Append,
            .relative_node_id = producer_before_target
                ? std::optional<int>{relative_node}
                : std::nullopt,
            .semantic_source_id = "test.direct_reset.producer",
            .provenance = "action-service producer ordering fixture",
        });
    if (target_node < 0
        || producer.status != BattleFrameThreadMutationStatus::Applied) {
        return std::nullopt;
    }

    auto* origin = find_frame_combatant(runtime->state, 0);
    if (origin == nullptr) {
        return std::nullopt;
    }
    origin->visual_instruction_mode_0x6 = 4;
    origin->visual_instruction_knowledge =
        CombatantVisualInstructionKnowledge::Known;
    origin->instruction_flags_0xec |= 0x00100000u;

    BattleFrameVisualChildTask task;
    task.sequence = runtime->visual.next_child_sequence++;
    task.action_ordinal = 7;
    task.origin_slot = 0;
    task.target_slot = 4;
    task.kind = BattleFrameVisualChildKind::ActionService;
    task.phase = BattleFrameVisualChildPhase::Delay;
    task.thread_state_0x19 = 1;
    task.first_eligible_frame = runtime->state.frame_index + 1;
    task.publication_visit_cursor = active_thread_visit_cursor(
        *runtime, producer.node_id);
    task.maximum_visits = 4;
    task.delay_remaining = 0;
    task.status = CombatantVisualModelStatus::Matched;
    task.provenance = "direct reset ordering fixture";
    runtime->visual.child_tasks.push_back(std::move(task));
    return runtime;
}

std::vector<BattleFrameStepEvent> run_until_visual_publication(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng,
    int max_frames = 16) {
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < max_frames; ++frame) {
        const auto step = run_first_turn_frame(runtime, rng);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_non_synthetic_publications(events) > 0) {
            break;
        }
    }
    return events;
}

TEST(SavorPredictCombatantVisualLoader, DecodesCompleteSetAndSystemCameraPayloads) {
    const auto loaded = load_spice_std_visual_resource_from_json_text(
        visual_json(set_command_payload(), system_camera_payload()));

    ASSERT_TRUE(loaded.ok);
    EXPECT_EQ(loaded.records_seen, 3);
    EXPECT_EQ(loaded.records_imported, 3);
    EXPECT_EQ(loaded.visual_records_decoded, 2);
    ASSERT_EQ(loaded.resource.records.size(), 3u);
    EXPECT_TRUE(loaded.resource.includes_sentinel);

    const auto& set = loaded.resource.records[0];
    ASSERT_EQ(set.kind, CombatantVisualCommandKind::SetCommand);
    ASSERT_TRUE(set.set_command.has_value());
    EXPECT_EQ(set.payload_bytes.size(), 0x1cu);
    EXPECT_EQ(set.set_command->service_flags, 0x00100020U);
    EXPECT_EQ(set.set_command->delay, 2);
    EXPECT_EQ(set.set_command->forced_mode, 0x0d);

    const auto& camera = loaded.resource.records[1];
    ASSERT_EQ(camera.kind, CombatantVisualCommandKind::SystemCamera);
    ASSERT_TRUE(camera.system_camera.has_value());
    EXPECT_EQ(camera.payload_bytes.size(), 0x24u);
    EXPECT_EQ(camera.system_camera->flags, 0x12345678U);
    EXPECT_EQ(camera.system_camera->scalar_bits, 0x3f800000U);
    EXPECT_EQ(camera.system_camera->end_frame, 0x0460U);
    EXPECT_EQ(camera.system_camera->mode, 0x0e);
}

TEST(SavorPredictCombatantAuxiliaryPublication, PreservesCurrentResourceChildOrder) {
    CombatantVisualResource resource;
    resource.binding.resource_stem = "MA001";
    resource.records = {
        {
            .index = 3,
            .location_code = 0x0d,
            .opcode = 3,
            .combined_type = 0x0003000du,
            .payload_in_bounds = true,
            .gate_fields_known = true,
            .gate_fields = {
                .primary_action_key = 5,
                .generic_secondary_key = 0,
                .direct_gate_secondary_key = -1,
            },
            .kind = CombatantVisualCommandKind::HitWeapon,
        },
        {
            .index = 4,
            .location_code = 0x0a,
            .opcode = 3,
            .combined_type = 0x0003000au,
            .payload_in_bounds = true,
            .gate_fields_known = true,
            .gate_fields = {
                .primary_action_key = 5,
                .generic_secondary_key = 0,
                .direct_gate_secondary_key = -1,
            },
            .kind = CombatantVisualCommandKind::MotionPause,
        },
        {
            .index = 8,
            .location_code = 0x2a,
            .opcode = 3,
            .combined_type = 0x0003002au,
            .payload_in_bounds = true,
            .gate_fields_known = true,
            .gate_fields = {
                .primary_action_key = 5,
                .generic_secondary_key = 0,
                .direct_gate_secondary_key = -1,
            },
            .kind = CombatantVisualCommandKind::SystemCamera,
        },
        {.index = 9, .location_code = -1},
    };
    const auto result = publish_combatant_auxiliary_commands({
        .action_ordinal = 2,
        .slot = 1,
        .target_slot = 4,
        .instruction_revision = 7,
        .publication_epoch = 3,
        .instruction_mode = 5,
        .instruction_subtype = -1,
        .instruction_flags_0xec = 0u,
        .instruction_flags_0xf0 = 0u,
        .gate_input = {
            .current_action_key = 5,
            .current_secondary_key = -1,
            .instruction_flags_0xec = 0u,
        },
        .current_resource = &resource,
        .readiness_uses_static_resource = false,
        .current_range_policy = CombatantAuxiliaryRangePolicy::FullTable,
        .selector_state = 0,
    });

    EXPECT_EQ(result.status, CombatantAuxiliaryPublicationStatus::Provisional);
    ASSERT_EQ(result.publications.size(), 3u);
    EXPECT_EQ(result.publications[0].kind, CombatantVisualCommandKind::HitWeapon);
    EXPECT_EQ(result.publications[1].kind, CombatantVisualCommandKind::MotionPause);
    EXPECT_EQ(result.publications[2].kind, CombatantVisualCommandKind::SystemCamera);
    EXPECT_TRUE(result.static_lane_missing);
}

TEST(SavorPredictCombatantAuxiliaryPublication, AppliesHardSkipsAndVisualSuppression) {
    CombatantVisualResource resource;
    resource.binding.resource_stem = "MA000";
    resource.records = {
        {
            .index = 0,
            .location_code = 4,
            .opcode = 3,
            .combined_type = 0x00030004u,
            .payload_in_bounds = true,
            .gate_fields_known = true,
            .gate_fields = {
                .primary_action_key = 5,
                .generic_secondary_key = 0,
                .direct_gate_secondary_key = -1,
            },
            .kind = CombatantVisualCommandKind::SetCommand,
        },
        {
            .index = 1,
            .location_code = 0x2a,
            .opcode = 3,
            .combined_type = 0x0003002au,
            .payload_in_bounds = true,
            .gate_fields_known = true,
            .gate_fields = {
                .primary_action_key = 5,
                .generic_secondary_key = 0,
                .direct_gate_secondary_key = -1,
            },
            .kind = CombatantVisualCommandKind::SystemCamera,
        },
        {.index = 2, .location_code = -1},
    };
    const auto mode2 = publish_combatant_auxiliary_commands({
        .slot = 0,
        .instruction_mode = 2,
        .instruction_subtype = -1,
        .instruction_flags_0x50 = 0x08000000u,
        .instruction_flags_0xec = 0u,
        .instruction_flags_0xf0 = 0u,
        .gate_input = {
            .current_action_key = 2,
            .current_secondary_key = -1,
            .instruction_flags_0xec = 0u,
        },
        .current_resource = &resource,
        .current_range_policy = CombatantAuxiliaryRangePolicy::FullTable,
    });
    EXPECT_EQ(mode2.status, CombatantAuxiliaryPublicationStatus::Skipped);
    EXPECT_TRUE(mode2.hard_skipped);
    EXPECT_TRUE(mode2.cleared_mode2_flag);
    EXPECT_TRUE(mode2.publications.empty());

    const auto suppressed = publish_combatant_auxiliary_commands({
        .slot = 0,
        .instruction_mode = 5,
        .instruction_subtype = -1,
        .instruction_flags_0xec = 0u,
        .instruction_flags_0xf0 = 0x00008000u,
        .gate_input = {
            .current_action_key = 5,
            .current_secondary_key = -1,
            .instruction_flags_0xec = 0u,
        },
        .current_resource = &resource,
        .readiness_uses_static_resource = false,
        .current_range_policy = CombatantAuxiliaryRangePolicy::FullTable,
        .selector_state = 0,
    });
    ASSERT_EQ(suppressed.publications.size(), 1u);
    EXPECT_EQ(
        suppressed.publications.front().kind,
        CombatantVisualCommandKind::SetCommand);
    EXPECT_TRUE(std::any_of(
        suppressed.decisions.begin(),
        suppressed.decisions.end(),
        [](const CombatantAuxiliaryCommandDecision& decision) {
            return decision.command_kind
                    == CombatantVisualCommandKind::SystemCamera
                && decision.decision
                    == CombatantAuxiliaryCommandDecisionKind::Suppressed;
        }));
}

TEST(SavorPredictCombatantVisualLoader, DecodesCollisionBoxPayload) {
    const auto loaded = load_spice_std_visual_resource_from_json_text(
        collision_visual_json(collision_box_payload()));

    ASSERT_TRUE(loaded.ok);
    ASSERT_EQ(loaded.resource.records.size(), 2u);
    const auto& collision = loaded.resource.records[0];
    EXPECT_EQ(collision.kind, CombatantVisualCommandKind::CollisionBox);
    ASSERT_TRUE(collision.collision_box.has_value());
    EXPECT_EQ(collision.collision_box->behavior_flags, 5U);
    EXPECT_EQ(collision.collision_box->start_counter, 24);
    EXPECT_EQ(collision.collision_box->end_counter, 998);
    EXPECT_EQ(collision.collision_box->object_id, 83);
    EXPECT_EQ(collision.collision_box->current_y_bits, 0x40F66658U);
    EXPECT_EQ(collision.collision_box->velocity_z_bits, 0x410FFFFDU);
    EXPECT_EQ(collision.collision_box->trailing_flags, 0U);
}

TEST(SavorPredictCombatantVisualModel, KeyPriorityAndSameModeEpochsAreExplicit) {
    CombatantVisualInstructionSnapshot instruction;
    instruction.runtime_instruction_mode = 5;
    instruction.validated_transition_mode = 8;
    instruction.selected_std_action_key = 4;
    instruction.knowledge = CombatantVisualInstructionKnowledge::Known;
    EXPECT_EQ(resolve_combatant_visual_action_key(instruction).action_key, 5);

    instruction.knowledge = CombatantVisualInstructionKnowledge::Provisional;
    const auto provisional_runtime = resolve_combatant_visual_action_key(instruction);
    EXPECT_EQ(provisional_runtime.action_key, 5);
    EXPECT_EQ(provisional_runtime.status, CombatantVisualModelStatus::Provisional);
    instruction.knowledge = CombatantVisualInstructionKnowledge::Known;

    instruction.runtime_instruction_mode.reset();
    EXPECT_EQ(resolve_combatant_visual_action_key(instruction).action_key, 8);
    instruction.validated_transition_mode.reset();
    EXPECT_EQ(resolve_combatant_visual_action_key(instruction).action_key, 4);
    instruction.selected_std_action_key.reset();
    const auto missing = resolve_combatant_visual_action_key(instruction);
    EXPECT_FALSE(missing.action_key.has_value());
    EXPECT_EQ(missing.status, CombatantVisualModelStatus::MissingInput);

    auto resource = decoded_resource(0, false, 0x0e, 2, 4);
    CombatantVisualTimelineState timeline;
    instruction.slot = 0;
    instruction.target_slot = 4;
    instruction.selected_std_action_key = 4;
    instruction.knowledge = CombatantVisualInstructionKnowledge::Provisional;
    install_combatant_visual_instruction(timeline, instruction);
    const auto first_epoch = timeline.epoch;
    const auto first = advance_combatant_visual_timeline(timeline, &resource);
    EXPECT_EQ(first.publications.size(), 2u);
    EXPECT_TRUE(advance_combatant_visual_timeline(timeline, &resource).publications.empty());

    install_combatant_visual_instruction(timeline, instruction);
    EXPECT_EQ(timeline.epoch, first_epoch + 1);
    EXPECT_EQ(advance_combatant_visual_timeline(timeline, &resource).publications.size(), 2u);
}

TEST(SavorPredictCombatantVisualModel, StdRowProducerDefersState0AndPublishesChangedState1Input) {
    CombatantInstructionStdRowProducerCursor cursor;
    const CombatantInstructionStdRowProducerRequest request{
        .action_ordinal = 3,
        .slot = 0,
        .instruction_revision = 7,
        .instruction_state_revision = 11,
        .selected_action_row_known = true,
        .selected_action_row_index = 42,
        .selected_action_key = 5,
        .runtime_instruction_mode = 5,
        .subtype = -1,
        .target_slot = 4,
        .instruction_flags = 0x00100000u,
        .knowledge = CombatantVisualInstructionKnowledge::Known,
        .provenance = "captured persistent instruction state",
    };

    const auto state0 = visit_combatant_instruction_std_row_producer(
        cursor, request);
    EXPECT_EQ(
        state0.status,
        CombatantInstructionStdRowProducerStatus::DeferredState0);
    EXPECT_FALSE(state0.install_epoch);
    EXPECT_EQ(state0.cursor_after.thread_state_0x19, 1);

    const auto published = visit_combatant_instruction_std_row_producer(
        state0.cursor_after, request);
    ASSERT_EQ(
        published.status,
        CombatantInstructionStdRowProducerStatus::Published);
    ASSERT_TRUE(published.install_epoch);
    ASSERT_TRUE(published.instruction.has_value());
    EXPECT_EQ(published.instruction->runtime_instruction_mode, 5);
    EXPECT_EQ(published.instruction->selected_std_action_key, 5);

    const auto unchanged = visit_combatant_instruction_std_row_producer(
        published.cursor_after, request);
    EXPECT_EQ(
        unchanged.status,
        CombatantInstructionStdRowProducerStatus::Unchanged);
    EXPECT_FALSE(unchanged.install_epoch);

    auto revised = request;
    revised.instruction_state_revision = 12;
    const auto same_mode_republication =
        visit_combatant_instruction_std_row_producer(
            unchanged.cursor_after, revised);
    EXPECT_EQ(
        same_mode_republication.status,
        CombatantInstructionStdRowProducerStatus::Published);
    EXPECT_TRUE(same_mode_republication.install_epoch);
}

TEST(SavorPredictCombatantVisualModel, StdRowProducerDoesNotGuessMissingCurrentRow) {
    CombatantInstructionStdRowProducerCursor cursor;
    cursor.thread_state_0x19 = 1;
    const auto idle = visit_combatant_instruction_std_row_producer(
        cursor,
        CombatantInstructionStdRowProducerRequest{
            .slot = 0,
            .instruction_state_revision = 1,
            .selected_action_row_known = true,
            .selected_action_row_index = 42,
            .selected_action_key = 1,
        });
    EXPECT_EQ(idle.status, CombatantInstructionStdRowProducerStatus::Idle);
    EXPECT_FALSE(idle.install_epoch);

    const auto missing = visit_combatant_instruction_std_row_producer(
        idle.cursor_after,
        CombatantInstructionStdRowProducerRequest{
            .action_ordinal = 0,
            .slot = 0,
            .instruction_revision = 1,
            .instruction_state_revision = 1,
        });
    EXPECT_EQ(
        missing.status,
        CombatantInstructionStdRowProducerStatus::MissingInput);
    EXPECT_FALSE(missing.install_epoch);
    EXPECT_FALSE(missing.instruction.has_value());
}

TEST(SavorPredictCombatantVisualModel, SelectsStdActionRowsFromProducerInputs) {
    const std::vector<CombatantStdActionRow> rows{
        CombatantStdActionRow{
            .index = 0,
            .action_id = 6,
            .row_type = 0,
            .flags = 0x01000000u,
        },
        CombatantStdActionRow{
            .index = 1,
            .action_id = 0x18,
            .row_type = 0,
            .flags = 0x03000000u,
            .secondary_key = 7,
        },
        CombatantStdActionRow{
            .index = 2,
            .action_id = -1,
            .row_type = 3,
        },
        CombatantStdActionRow{
            .index = 3,
            .action_id = 5,
            .row_type = 0,
            .flags = 0xffffffffu,
        },
    };

    const auto exact = select_combatant_std_action_row(rows, {
        .action_id = 6,
    });
    ASSERT_TRUE(exact.row.has_value());
    EXPECT_EQ(exact.status, CombatantStdActionRowSelectionStatus::Matched);
    EXPECT_EQ(exact.row->index, 0);
    EXPECT_EQ(exact.row->flags, 0x01000000u);

    const auto missing_secondary = select_combatant_std_action_row(rows, {
        .action_id = 0x18,
    });
    EXPECT_FALSE(missing_secondary.row.has_value());
    EXPECT_EQ(
        missing_secondary.status,
        CombatantStdActionRowSelectionStatus::MissingInput);

    const auto keyed = select_combatant_std_action_row(rows, {
        .action_id = 0x18,
        .secondary_key = 7,
    });
    ASSERT_TRUE(keyed.row.has_value());
    EXPECT_EQ(keyed.row->index, 1);

    const auto transition = select_combatant_std_action_row(rows, {
        .action_id = 0x13,
    });
    ASSERT_TRUE(transition.row.has_value());
    EXPECT_EQ(
        transition.status,
        CombatantStdActionRowSelectionStatus::Provisional);
    EXPECT_EQ(transition.selected_action_id, 6);
    EXPECT_TRUE(transition.used_transition_fallback);

    const auto terminated = select_combatant_std_action_row(rows, {
        .action_id = 5,
        .allow_transition_fallback = false,
    });
    EXPECT_FALSE(terminated.row.has_value());
    EXPECT_EQ(
        terminated.status,
        CombatantStdActionRowSelectionStatus::Unsupported);
}

TEST(SavorPredictCombatantVisualModel, InitializesMotionStateFromStdProducerRows) {
    const std::vector<CombatantStdActionRow> rows{
        CombatantStdActionRow{
            .index = 0,
            .action_id = 6,
            .row_type = 1,
            .transition_gate_divisor_bits = 0x40a00000u,
            .motion_progress_step_bits = 0x3f800000u,
        },
        CombatantStdActionRow{
            .index = 68,
            .action_id = 0,
            .row_type = 0,
            .transition_gate_divisor_bits = 0x3fd9999bu,
            .motion_progress_step_bits = 0x41c8ccc9u,
        },
        CombatantStdActionRow{
            .index = 69,
            .action_id = 1,
            .row_type = 1,
            .transition_gate_divisor_bits = 0x3e99999au,
            .motion_progress_step_bits = 0x3f800000u,
        },
    };

    const auto initialized = initialize_combatant_std_motion_state(rows);
    EXPECT_EQ(
        initialized.status,
        CombatantStdMotionInitializationStatus::Matched);
    EXPECT_EQ(initialized.base_row_index, 68);
    EXPECT_EQ(initialized.alternate_row_index, 69);
    EXPECT_EQ(initialized.base_speed_bits, 0x40233334u);
    EXPECT_EQ(initialized.alternate_speed_bits, 0x3ee66667u);
    EXPECT_EQ(initialized.turn_speed_bits, 0x41c8ccc9u);
    EXPECT_FALSE(initialized.alternate_uses_base);
}

TEST(SavorPredictCombatantVisualModel, UsesBaseMotionWhenActionOneSourceIsZero) {
    const std::vector<CombatantStdActionRow> rows{
        CombatantStdActionRow{
            .index = 16,
            .action_id = 0,
            .row_type = 0,
            .transition_gate_divisor_bits = 0x3fccccbdu,
            .motion_progress_step_bits = 0x41a0ccbfu,
        },
        CombatantStdActionRow{
            .index = 17,
            .action_id = 1,
            .row_type = 1,
            .transition_gate_divisor_bits = 0,
            .motion_progress_step_bits = 0x3f800000u,
        },
    };

    const auto initialized = initialize_combatant_std_motion_state(rows);
    ASSERT_EQ(
        initialized.status,
        CombatantStdMotionInitializationStatus::Matched);
    EXPECT_TRUE(initialized.alternate_uses_base);
    EXPECT_EQ(initialized.alternate_speed_bits, initialized.base_speed_bits);
    EXPECT_EQ(initialized.turn_speed_bits, 0x41a0ccbfu);
}

TEST(SavorPredictCombatantVisualRuntime, State0PublishesInitialCallbackWithoutInvokingIt) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 1)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x12344321U;
    const auto first = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(first.ok);

    const auto& callback = runtime->visual.persistent_instruction_callbacks[0];
    EXPECT_TRUE(callback.installed);
    EXPECT_EQ(callback.thread_state_0x19, 1);
    EXPECT_EQ(callback.publication_revision, 1u);
    EXPECT_EQ(callback.callback_index, 9);
    EXPECT_EQ(
        callback.callback_family,
        ActionMotionPersistentCallbackFamily::ActionMotionSync_8001AB60);
    EXPECT_EQ(callback.callback_state, 0);
    EXPECT_EQ(runtime->state.combatants[0].visual_instruction_mode_0x6, 1);

    const auto publication = std::find_if(
        first.events.begin(), first.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 0
                && event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionStatePublish
                && event.detail.find("source=State0Initialization")
                    != std::string::npos;
        });
    ASSERT_NE(publication, first.events.end());
    EXPECT_TRUE(publication->persistent_callback_changed);
    EXPECT_FALSE(publication->persistent_callback_same_value);
    EXPECT_TRUE(std::none_of(
        first.events.begin(), first.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 0
                && event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionInvocationDecision;
        }));
}

TEST(SavorPredictCombatantVisualRuntime, CallbackPublicationIsSeparateFromInstructionStagingAndPlaybackState) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 1)));

    ASSERT_TRUE(publish_battle_frame_persistent_instruction_callback(
        *runtime,
        0,
        BattleFrameInstructionCallbackPublicationSource::State0Initialization,
        "unit state-0 publisher"));
    auto& callback = runtime->visual.persistent_instruction_callbacks[0];
    ASSERT_EQ(callback.publication_revision, 1u);
    callback.callback_state = 7;

    ASSERT_TRUE(publish_battle_frame_persistent_instruction_callback(
        *runtime,
        0,
        BattleFrameInstructionCallbackPublicationSource::State1CurrentInstruction,
        "unit same-value state-1 publisher"));
    EXPECT_EQ(callback.publication_revision, 2u);
    EXPECT_EQ(callback.same_value_publications, 1);
    EXPECT_EQ(callback.callback_changes, 0);
    EXPECT_EQ(callback.callback_state, 7);

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->selected_action_row_index = 4;
    combatant->selected_action_row_action_id = 5;
    combatant->selected_action_row_callback_index = 8;
    combatant->selected_action_row_callback_ordinal = 6;
    combatant->selected_action_row_known = true;
    CombatantVisualInstructionSnapshot staged;
    staged.slot = 0;
    staged.runtime_instruction_mode = 5;
    staged.selected_std_action_key = 5;
    staged.target_slot = 4;
    staged.knowledge = CombatantVisualInstructionKnowledge::Known;
    staged.provenance = "unit movement-like stage";
    ASSERT_TRUE(stage_battle_frame_visual_instruction_state(
        *runtime, 3, std::move(staged)));
    EXPECT_EQ(callback.publication_revision, 2u);
    EXPECT_EQ(callback.callback_index, 9);
    EXPECT_EQ(callback.callback_state, 7);

    ASSERT_TRUE(publish_battle_frame_persistent_instruction_callback(
        *runtime,
        0,
        BattleFrameInstructionCallbackPublicationSource::
            ExplicitModeledTransition,
        "unit exact param4=0 publisher"));
    EXPECT_EQ(callback.publication_revision, 3u);
    EXPECT_EQ(callback.callback_changes, 1);
    EXPECT_EQ(callback.callback_index, 8);
    EXPECT_EQ(
        callback.callback_family,
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0);
    EXPECT_EQ(callback.callback_state, 7);
    EXPECT_EQ(
        runtime->visual.action_motion_playbacks[0].phase,
        ActionMotionPlaybackPhase::Inactive);
}

TEST(SavorPredictCombatantVisualRuntime, QueuedTransitionPublishesAndResetsInCurrentInstructionVisit) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 2)));
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 0, -1, 5));
    auto& callback = runtime->visual.persistent_instruction_callbacks[0];
    callback.callback_state = 15;
    runtime->visual.pending_events.clear();
    runtime->visual.instruction_control_reset_history.clear();

    const int target_node = instruction_thread_node_id(*runtime, 0);
    ASSERT_GE(target_node, 0);
    begin_battle_frame_thread_traversal(runtime->thread_list, 1);
    ASSERT_EQ(set_battle_frame_thread_cursor(
        runtime->thread_list,
        target_node,
        "test.queued_transition.visit",
        "queued transition instruction visit",
        1).status, BattleFrameThreadMutationStatus::Applied);

    BattleFrameActionRuntime action;
    action.active = true;
    action.action_ordinal = 3;
    action.actor_slot = 0;
    action.target_slot = 4;
    action.action_kind = BattleMovementActionKind::BasicAttack;
    action.execution_route = BasicAttackExecutionRoute::DirectMelee;
    action.action_resolution_available = true;
    action.queued_state_transition = BasicAttackQueuedStateResult{
        .status = QueuedInstructionParamStatus::Validated,
        .queued_state = QueuedStdActionState::DirectNoncritical5,
        .instruction_mode = 4,
        .confidence = "validated",
        .provenance = "queued reset fixture",
    };
    action.queued_state_transition_pending = true;
    runtime->active_action = std::move(action);

    const auto publication_before = callback.publication_revision;
    ASSERT_TRUE(stage_battle_frame_queued_std_action_transition(*runtime, 3));
    EXPECT_EQ(callback.publication_revision, publication_before + 1);
    EXPECT_EQ(callback.callback_state, 0);
    EXPECT_FALSE(runtime->active_action->queued_state_transition_pending);
    EXPECT_TRUE(runtime->active_action->queued_state_transition_published);
    EXPECT_TRUE(runtime->visual.pending_instruction_control_resets.empty());
    ASSERT_EQ(runtime->visual.instruction_control_reset_history.size(), 1u);
    const auto& reset = runtime->visual.instruction_control_reset_history.back();
    EXPECT_EQ(reset.source,
              BattleFrameInstructionControlResetSource::QueuedTransition);
    EXPECT_EQ(reset.lifecycle,
              BattleFrameInstructionControlResetLifecycle::Applied);
    EXPECT_EQ(reset.timing,
              BattleFrameInstructionControlResetTiming::SameInstructionVisit);
    EXPECT_EQ(reset.target_node_id, target_node);
    EXPECT_EQ(reset.callback_state_before, 15);
    EXPECT_EQ(reset.callback_state_after, 0);
    end_battle_frame_thread_traversal(runtime->thread_list);
}

TEST(SavorPredictCombatantVisualRuntime, DirectResetReachesLaterTargetInSameFrameWithoutRepublishing) {
    auto runtime = direct_reset_runtime(true);
    ASSERT_TRUE(runtime.has_value());

    std::uint32_t rng = 0x11223344u;
    const auto frame = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(frame.ok);
    EXPECT_TRUE(runtime->visual.pending_instruction_control_resets.empty());

    const auto& history = runtime->visual.instruction_control_reset_history;
    ASSERT_EQ(history.size(), 2u);
    EXPECT_EQ(history[0].source,
              BattleFrameInstructionControlResetSource::DirectTransition);
    EXPECT_EQ(history[0].lifecycle,
              BattleFrameInstructionControlResetLifecycle::Applied);
    EXPECT_EQ(history[0].timing,
              BattleFrameInstructionControlResetTiming::SameFrameLaterVisit);
    EXPECT_EQ(history[1].lifecycle,
              BattleFrameInstructionControlResetLifecycle::Consumed);
    EXPECT_EQ(history[1].frame_index, history[0].frame_index);
    EXPECT_EQ(history[1].sequence, history[0].sequence);

    EXPECT_EQ(std::count_if(
        frame.events.begin(),
        frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 4
                && event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionStatePublish;
        }), 1);
    const auto invocation = std::find_if(
        frame.events.begin(),
        frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 4
                && event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionInvocationDecision;
        });
    ASSERT_NE(invocation, frame.events.end());
    EXPECT_EQ(invocation->action_motion_callback_state_before, 0);
}

TEST(SavorPredictCombatantVisualRuntime, DirectResetReachesEarlierTargetOnNextFrameWithoutRepublishing) {
    auto runtime = direct_reset_runtime(false);
    ASSERT_TRUE(runtime.has_value());

    std::uint32_t rng = 0x55667788u;
    const auto producer_frame = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(producer_frame.ok);
    ASSERT_EQ(runtime->visual.pending_instruction_control_resets.size(), 1u);
    ASSERT_EQ(runtime->visual.instruction_control_reset_history.size(), 1u);
    EXPECT_EQ(runtime->visual.instruction_control_reset_history[0].timing,
              BattleFrameInstructionControlResetTiming::NextFrameVisit);
    const int producer_frame_index =
        runtime->visual.instruction_control_reset_history[0].frame_index;

    const auto target_frame = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(target_frame.ok);
    EXPECT_TRUE(runtime->visual.pending_instruction_control_resets.empty());
    ASSERT_EQ(runtime->visual.instruction_control_reset_history.size(), 2u);
    const auto& consumed =
        runtime->visual.instruction_control_reset_history.back();
    EXPECT_EQ(consumed.lifecycle,
              BattleFrameInstructionControlResetLifecycle::Consumed);
    EXPECT_EQ(consumed.frame_index, producer_frame_index + 1);
    EXPECT_EQ(std::count_if(
        target_frame.events.begin(),
        target_frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 4
                && event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionStatePublish;
        }), 0);
    const auto invocation = std::find_if(
        target_frame.events.begin(),
        target_frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 4
                && event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionInvocationDecision;
        });
    ASSERT_NE(invocation, target_frame.events.end());
    EXPECT_EQ(invocation->action_motion_callback_state_before, 0);
}

TEST(SavorPredictCombatantVisualRuntime, DirectResetReportsRemovedAndReplacedTargetNodes) {
    for (const bool replace_target : {false, true}) {
        SCOPED_TRACE(replace_target ? "replacement" : "removal");
        auto runtime = direct_reset_runtime(false);
        ASSERT_TRUE(runtime.has_value());
        std::uint32_t rng = 0x778899aau;
        ASSERT_TRUE(run_first_turn_frame(*runtime, rng).ok);
        ASSERT_EQ(runtime->visual.pending_instruction_control_resets.size(), 1u);
        const int target_node = runtime->visual
            .pending_instruction_control_resets.front().target_node_id;
        ASSERT_EQ(remove_battle_frame_thread(
            runtime->thread_list,
            target_node,
            "test.direct_reset.target_remove",
            "remove pending target",
            runtime->state.frame_index).status,
            BattleFrameThreadMutationStatus::Applied);
        if (replace_target) {
            ASSERT_EQ(create_battle_frame_thread(
                runtime->thread_list,
                BattleFrameThreadCreateRequest{
                    .kind = BattleFrameThreadNodeKind::CombatantInstruction,
                    .owner_slot = 4,
                    .callback =
                        BattleFrameThreadCallbackIdentity::CombatantInstruction,
                    .active = true,
                    .semantic_source_id = "test.direct_reset.target_replace",
                    .provenance = "replacement instruction node",
                }).status, BattleFrameThreadMutationStatus::Applied);
        }

        const auto next_frame = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(next_frame.ok);
        EXPECT_TRUE(runtime->visual.pending_instruction_control_resets.empty());
        ASSERT_EQ(runtime->visual.instruction_control_reset_history.size(), 2u);
        EXPECT_EQ(
            runtime->visual.instruction_control_reset_history.back().lifecycle,
            replace_target
                ? BattleFrameInstructionControlResetLifecycle::TargetReplaced
                : BattleFrameInstructionControlResetLifecycle::TargetRemoved);
    }
}

TEST(SavorPredictCombatantVisualRuntime, PublishesAndRunsChildrenAfterOwnerInSameFrame) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, decoded_resource(0, false, 2)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x12345678U;
    const auto placement = draw_rand15(rng);
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto events = run_until_visual_publication(*runtime, rng);

    EXPECT_EQ(rng, placement.next_state);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualInstructionInstall), 1);
    EXPECT_EQ(count_step(
        events, BattleFrameWorkerStepKind::VisualAuxiliaryPublication), 1);
    EXPECT_EQ(count_non_synthetic_publications(events), 2);
    EXPECT_EQ(
        count_step_for_command(
            events,
            BattleFrameWorkerStepKind::VisualChildState0,
            CombatantVisualCommandKind::SetCommand)
            + count_step_for_command(
                events,
                BattleFrameWorkerStepKind::VisualChildState0,
                CombatantVisualCommandKind::SystemCamera),
        2);
    std::vector<const BattleFrameVisualChildTask*> published_tasks;
    for (const auto& task : runtime->visual.child_tasks) {
        if (task.command_kind
            != CombatantVisualCommandKind::SyntheticActionView) {
            published_tasks.push_back(&task);
        }
    }
    ASSERT_EQ(published_tasks.size(), 2u);
    EXPECT_GE(published_tasks[0]->thread_node_id, 0);
    EXPECT_GE(published_tasks[1]->thread_node_id, 0);
    EXPECT_LT(
        published_tasks[0]->thread_node_id,
        published_tasks[1]->thread_node_id);
    EXPECT_EQ(
        std::count_if(
            runtime->thread_list.history.begin(),
            runtime->thread_list.history.end(),
            [](const BattleFrameThreadMutationEvent& event) {
                return event.kind == BattleFrameThreadMutationKind::Create
                    && event.semantic_source_id
                        == "combatant.auxiliary_visual.child";
            }),
        2);
    const auto publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualCommandPublish
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        });
    const auto child = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualChildState0
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        });
    ASSERT_NE(publication, events.end());
    ASSERT_NE(child, events.end());
    EXPECT_LT(std::distance(events.begin(), publication),
              std::distance(events.begin(), child));
}

TEST(SavorPredictCombatantVisualRuntime, MovementStageWaitsForState1PublisherAndStdRowProducer) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 2)));
    ASSERT_TRUE(schedule_action(*runtime).scheduled);

    std::uint32_t rng = 0x22446688U;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 512; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        const bool installed = std::any_of(
            events.begin(), events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.step_kind
                        == BattleFrameWorkerStepKind::VisualInstructionInstall
                    && event.slot == 0
                    && event.action_ordinal == 0;
            });
        const bool setup_evaluated = std::any_of(
            events.begin(), events.end(),
            [](const BattleFrameStepEvent& event) {
                return event.step_kind
                        == BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc
                    && event.slot == 0
                    && event.action_ordinal == 0
                    && event.action_motion_setup_event;
            });
        if (installed && setup_evaluated) {
            break;
        }
    }
    const auto state_publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionDecision
                && event.slot == 0
                && event.action_ordinal == 0;
        });
    const auto epoch_install = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualInstructionInstall
                && event.slot == 0
                && event.action_ordinal == 0;
        });
    const auto control_reset = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::InstructionCallbackControlReset
                && event.slot == 0
                && event.action_ordinal == 0
                && event.instruction_control_reset_source
                    == BattleFrameInstructionControlResetSource::
                        QueuedTransition;
        });
    const auto setup = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc
                && event.slot == 0
                && event.action_ordinal == 0
                && event.action_motion_setup_event;
        });
    ASSERT_NE(state_publication, events.end());
    ASSERT_NE(control_reset, events.end());
    ASSERT_NE(epoch_install, events.end());
    ASSERT_NE(setup, events.end());
    EXPECT_LT(std::distance(events.begin(), state_publication),
              std::distance(events.begin(), control_reset));
    EXPECT_LT(std::distance(events.begin(), control_reset),
              std::distance(events.begin(), epoch_install));
    EXPECT_LT(std::distance(events.begin(), control_reset),
              std::distance(events.begin(), setup));
    EXPECT_NE(setup->status, BattleFrameEventStatus::MissingInput);
    EXPECT_NE(setup->status, BattleFrameEventStatus::Unsupported);
    EXPECT_EQ(control_reset->action_motion_callback_state_after, 0);
    EXPECT_GT(count_step(events, BattleFrameWorkerStepKind::VisualInstructionDecision), 0);
    EXPECT_EQ(runtime->visual.timelines[0].instruction.runtime_instruction_mode, 6);
    EXPECT_TRUE(std::none_of(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            const bool invocation_event = event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionInvocationDecision
                || event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionPlaybackInstall;
            return invocation_event
                && (event.rng_event || event.draws_consumed != 0);
        }));
}

TEST(SavorPredictCombatantVisualRuntime, SetupPendingWaitsForMatchingPersistentCallbackEvaluation) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 2)));

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->visual_instruction_action_ordinal = 0;
    combatant->visual_instruction_revision = 17;
    combatant->visual_instruction_mode_0x6 = 6;
    combatant->instruction_target_slot_0x4 = 4;
    combatant->selected_action_row_index = 5;
    combatant->selected_action_row_action_id = 6;
    combatant->selected_action_row_callback_index = 8;
    combatant->selected_action_row_callback_ordinal = 2;

    auto& callback = runtime->visual.persistent_instruction_callbacks[0];
    callback.installed = true;
    callback.thread_state_0x19 = 1;
    callback.action_ordinal = 0;
    callback.slot = 0;
    callback.instruction_state_revision = 17;
    callback.callback_index = 8;
    callback.callback_family =
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0;
    callback.callback_state = 7;
    callback.current_instruction_row = CombatantStdActionRow{
        .index = 5,
        .action_id = 6,
        .row_type = 1,
        .callback_index = 8,
        .callback_ordinal = 2,
        .transition_gate_divisor_bits = 0x40a00000u,
    };

    auto& instruction = runtime->combatant_instructions[0];
    instruction.active = true;
    instruction.revision = 1;
    instruction.action_ordinal = 0;
    instruction.slot = 0;
    instruction.target_slot = 4;
    instruction.action_mode = 6;
    instruction.phase = BattleFrameCombatantInstructionPhase::SetupPending;

    std::uint32_t rng = 0x22446688U;
    const auto waiting_step = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(waiting_step.ok);
    const auto setup_wait = std::find_if(
        waiting_step.events.begin(), waiting_step.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::CombatantInstructionWait
                && event.slot == 0
                && event.detail.find(
                    "setup waits for the matching persistent callback visit")
                    != std::string::npos;
        });
    ASSERT_NE(setup_wait, waiting_step.events.end());
    EXPECT_EQ(instruction.phase, BattleFrameCombatantInstructionPhase::SetupPending);
    EXPECT_FALSE(instruction.action_motion_setup_evaluated);

    callback.callback_state = 0;
    const auto setup_step = run_first_turn_frame(*runtime, rng);
    ASSERT_TRUE(setup_step.ok);
    const auto setup = std::find_if(
        setup_step.events.begin(), setup_step.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionSetup_8001fabc
                && event.slot == 0
                && event.action_motion_setup_event;
        });
    ASSERT_NE(setup, setup_step.events.end());
    EXPECT_NE(setup->status, BattleFrameEventStatus::MissingInput);
    EXPECT_NE(setup->status, BattleFrameEventStatus::Unsupported);
    EXPECT_TRUE(instruction.action_motion_setup_evaluated);
    EXPECT_EQ(instruction.action_motion_setup_instruction_state_revision, 17u);
}

TEST(SavorPredictCombatantVisualRuntime, ValidatedStateTransitionPublishesAtProducerAndInvokesOnTargetVisit) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 2)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x88776655U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto initial = run_until_visual_publication(*runtime, rng);
    ASSERT_GT(count_step(
        initial,
        BattleFrameWorkerStepKind::VisualInstructionInstall), 0);
    const auto epoch_before = runtime->visual.timelines[0].epoch;
    const auto task_count_before = runtime->visual.child_tasks.size();
    auto& callback = runtime->visual.persistent_instruction_callbacks[0];
    callback.callback_state = 15;
    const auto publication_before = callback.publication_revision;
    auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    ASSERT_EQ(actor->instruction_target_slot_0x4, 4);
    ASSERT_EQ(actor->instruction_secondary_target_slot_0x48, -1);

    ASSERT_TRUE(stage_battle_frame_validated_instruction_transition(
        *runtime,
        0,
        0,
        5,
        8,
        "validated attack-result mode-8 state transition"));
    EXPECT_EQ(actor->instruction_target_slot_0x4, 4);
    EXPECT_EQ(actor->instruction_secondary_target_slot_0x48, 5);
    EXPECT_EQ(callback.publication_revision, publication_before + 1);
    EXPECT_EQ(callback.callback_state, 0);
    ASSERT_EQ(runtime->visual.pending_instruction_control_resets.size(), 1u);
    EXPECT_EQ(runtime->visual.timelines[0].epoch, epoch_before);
    EXPECT_EQ(runtime->visual.child_tasks.size(), task_count_before);

    std::vector<BattleFrameStepEvent> transition_events;
    for (int frame = 0; frame < 32
        && runtime->visual.timelines[0].epoch == epoch_before; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        transition_events.insert(
            transition_events.end(), step.events.begin(), step.events.end());
    }
    EXPECT_EQ(runtime->visual.timelines[0].epoch, epoch_before + 1);
    EXPECT_EQ(runtime->visual.timelines[0].instruction.runtime_instruction_mode, 8);
    const auto state_event = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::VisualInstructionDecision;
        });
    const auto callback_publication = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::VisualInstructionStatePublish;
        });
    const auto install_event = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::VisualInstructionInstall;
        });
    const auto reset_event = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::InstructionCallbackControlReset;
        });
    const auto reset_consume_event = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume;
        });
    const auto invocation_event = std::find_if(
        transition_events.begin(), transition_events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::ActionMotionInvocationDecision;
        });
    ASSERT_NE(state_event, transition_events.end());
    ASSERT_NE(callback_publication, transition_events.end());
    ASSERT_NE(invocation_event, transition_events.end());
    ASSERT_NE(install_event, transition_events.end());
    ASSERT_NE(reset_event, transition_events.end());
    ASSERT_NE(reset_consume_event, transition_events.end());
    EXPECT_LT(std::distance(transition_events.begin(), state_event),
              std::distance(transition_events.begin(), callback_publication));
    EXPECT_LT(std::distance(transition_events.begin(), callback_publication),
              std::distance(transition_events.begin(), reset_event));
    EXPECT_LT(std::distance(transition_events.begin(), reset_event),
              std::distance(transition_events.begin(), reset_consume_event));
    EXPECT_LT(std::distance(transition_events.begin(), reset_consume_event),
              std::distance(transition_events.begin(), invocation_event));
    EXPECT_LT(std::distance(transition_events.begin(), invocation_event),
              std::distance(transition_events.begin(), install_event));
}

TEST(SavorPredictCombatantVisualRuntime, PersistentStateFourOwnsExactlyOneRotationStepPerInstructionVisit) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 2)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    ASSERT_TRUE(publish_fixture_visual_instruction_state(
        *runtime, 0, 0, 5, true));

    auto* actor = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(actor, nullptr);
    actor->turn_current_degrees_0x11c = 10.0f;
    actor->turn_target_degrees_0x120 = 50.0f;
    actor->turn_step_degrees_0x124 = 20.0f;
    actor->turn_state_known = true;

    auto& callback =
        runtime->visual.persistent_instruction_callbacks[0];
    callback.callback_state = 4;
    callback.last_rotation_step_valid = false;
    auto& instruction = runtime->combatant_instructions[0];
    instruction.active = true;
    instruction.action_ordinal = 0;
    instruction.phase = BattleFrameCombatantInstructionPhase::Rotating;

    std::uint32_t rng = 0x88776655U;
    const auto frame = run_first_turn_frame(*runtime, rng);
    const auto rotation_steps = std::count_if(
        frame.events.begin(),
        frame.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.slot == 0
                && event.step_kind
                    == BattleFrameWorkerStepKind::
                        ActionMotionRotateStep_8001b630_80061114;
        });

    EXPECT_EQ(rotation_steps, 1);
    EXPECT_FLOAT_EQ(actor->turn_current_degrees_0x11c, 30.0f);
    EXPECT_EQ(callback.callback_state, 4);
    EXPECT_EQ(
        instruction.phase,
        BattleFrameCombatantInstructionPhase::Rotating);
    EXPECT_TRUE(callback.last_rotation_step_valid);
    EXPECT_EQ(
        callback.last_rotation_step_traversal_generation,
        runtime->thread_list.traversal_generation);
    EXPECT_FALSE(callback.last_rotation_reached_target);
}

TEST(SavorPredictCombatantVisualRuntime, CollisionBoxUsesGeometryAndSharedSelectorAtItsThreadCursor) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_collision_resource(0)));
    auto target_resource = decoded_resource(4, false, 2, 0, 4);
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, std::move(target_resource)));

    auto* origin = find_frame_combatant(runtime->state, 0);
    auto* target = find_frame_combatant(runtime->state, 4);
    ASSERT_NE(origin, nullptr);
    ASSERT_NE(target, nullptr);
    origin->combatant_cur_pos_0x1c = {};
    origin->pos_holder = {};
    origin->combatant_facing_angle_0x2c = 0;
    target->combatant_cur_pos_0x1c = {.x = 0.0f, .y = 0.0f, .z = 15.0f};
    target->pos_holder = target->combatant_cur_pos_0x1c;
    runtime->collision_occupancy = make_battle_collision_occupancy_runtime();
    ASSERT_EQ(refresh_battle_collision_occupancy(
        runtime->collision_occupancy,
        {.slot = 0, .present = true, .alive = true,
         .position = {.x = 0.0f, .y = 0.0f, .z = 0.0f}}).status,
        BattleCollisionModelStatus::Matched);
    ASSERT_EQ(refresh_battle_collision_occupancy(
        runtime->collision_occupancy,
        {.slot = 4, .present = true, .alive = true,
         .position = {.x = 0.0f, .y = 0.0f, .z = 15.0f}}).status,
        BattleCollisionModelStatus::Matched);

    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 4, -1, 5));
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 0, 0, 4));
    const auto reaction = model_battle_target_reaction({
        .action_kind = BattleTargetReactionActionKind::BasicAttack,
        .origin_slot = 0,
        .target_slot = 4,
        .hit_check = 1,
        .pending_damage = 10,
        .target_hp_before_flush = 100,
        .target_dead = false,
        .counter_accepted = false,
    });
    ASSERT_TRUE(publish_first_turn_target_reaction(
        *runtime,
        {.action_ordinal = 0, .reaction = reaction}));

    std::uint32_t rng = 0x10293847U;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 16; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        const bool collision_complete = std::any_of(
            events.begin(), events.end(), [](const BattleFrameStepEvent& event) {
                return event.step_kind
                        == BattleFrameWorkerStepKind::VisualChildCleanup
                    && event.visual_command_kind
                        == CombatantVisualCommandKind::CollisionBox;
            });
        if (collision_complete) {
            break;
        }
    }

    const auto publication = std::find_if(
        events.begin(), events.end(), [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualCommandPublish
                && event.visual_command_kind == CombatantVisualCommandKind::CollisionBox;
        });
    const auto state0 = std::find_if(
        events.begin(), events.end(), [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildState0
                && event.visual_command_kind == CombatantVisualCommandKind::CollisionBox;
        });
    const auto selector = std::find_if(
        events.begin(), events.end(), [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::DirectTransitionSelect;
        });
    const auto cleanup = std::find_if(
        events.begin(), events.end(), [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildCleanup
                && event.visual_command_kind == CombatantVisualCommandKind::CollisionBox;
        });
    std::ostringstream collision_trace;
    for (const auto& event : events) {
        if (event.visual_command_kind == CombatantVisualCommandKind::CollisionBox
            || event.step_kind == BattleFrameWorkerStepKind::DirectTransitionSelect) {
            collision_trace
                << "\nframe=" << event.frame_index
                << " step=" << battle_frame_worker_step_kind_name(event.step_kind)
                << " child=" << event.visual_child_kind
                << " slot=" << event.slot
                << " target=" << event.target_slot
                << " detail=" << event.detail;
        }
    }
    ASSERT_NE(publication, events.end());
    ASSERT_NE(state0, events.end());
    ASSERT_NE(selector, events.end());
    ASSERT_NE(cleanup, events.end()) << collision_trace.str();
    EXPECT_EQ(state0->frame_index, publication->frame_index);
    EXPECT_EQ(selector->frame_index, publication->frame_index + 2);
    EXPECT_EQ(cleanup->frame_index, selector->frame_index + 1);
    EXPECT_EQ(selector->visual_effective_mode, 11);
    EXPECT_EQ(selector->draws_consumed, 0);
    EXPECT_GT(count_step(events, BattleFrameWorkerStepKind::CollisionProbe), 0);
    EXPECT_GT(count_step(
        events, BattleFrameWorkerStepKind::InstructionCallbackControlResetConsume), 0);
}

TEST(SavorPredictCombatantVisualRuntime, RoleCallbackPublishesAndClearsLowF0BitFromChildThread) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0, false, 0, 2, 5);
    for (auto& row : resource.action_rows) {
        if (row.action_id == 5 || row.action_id == 0x0b) {
            row.callback_index = 12;
            row.callback_ordinal = 0;
        }
    }
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, std::move(resource)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    runtime->visual.action_view_role.valid = false;

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->selected_action_row_index = 4;
    combatant->selected_action_row_action_id = 5;
    combatant->selected_action_row_callback_index = 12;
    combatant->selected_action_row_callback_ordinal = 0;
    combatant->selected_action_row_duration_bits = 0x40a00000u;
    combatant->selected_action_row_duration_known = true;
    runtime->visual.std_row_producers[0].thread_state_0x19 = 1;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(
        *runtime, 0, 0, 5, true));

    std::uint32_t rng = 0x12345678u;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 10
        && (combatant->instruction_flags_0xf0 & 0x00000004u) == 0;
         ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(
            events.end(), step.events.begin(), step.events.end());
    }
    EXPECT_NE(combatant->instruction_flags_0xf0 & 0x00000004u, 0u);
    EXPECT_EQ(
        count_step(
            events,
            BattleFrameWorkerStepKind::ActionViewRoleFlagSpawn),
        1);
    EXPECT_GT(
        count_step(
            events,
            BattleFrameWorkerStepKind::ActionViewRoleFlagVisit),
        0);
    ASSERT_TRUE(runtime->active_action.has_value());
    runtime->active_action->completion_turn_phase = 5;
    const auto release = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(combatant->instruction_flags_0xf0 & 0x00000004u, 0u);
    EXPECT_GT(
        count_step(
            release.events,
            BattleFrameWorkerStepKind::ActionViewRoleFlagVisit),
        0);
    EXPECT_TRUE(std::all_of(
        runtime->visual.role_flag_children.begin(),
        runtime->visual.role_flag_children.end(),
        [](const BattleFrameActionViewRoleFlagChildRuntime& child) {
            return child.complete;
        }));
}

TEST(SavorPredictCombatantVisualRuntime, Mode8PublicationWaitsForCapturedState6Playback) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 0, 2, 8)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    runtime->visual.action_view_role.valid = false;

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->selected_action_row_index = 6;
    combatant->selected_action_row_action_id = 8;
    combatant->selected_action_row_callback_index = 8;
    combatant->selected_action_row_callback_ordinal = 3;
    combatant->selected_action_row_duration_bits = 0x40a00000u;
    combatant->selected_action_row_duration_known = true;
    runtime->visual.std_row_producers[0].thread_state_0x19 = 1;

    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 0, 0, 8, true));
    EXPECT_TRUE(battle_frame_action_visual_publication_pending(*runtime, 0));
    std::uint32_t rng = 0x53746174u;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 12; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_non_synthetic_publications(events) > 0) {
            break;
        }
    }

    const auto true_gate = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::ActionMotionState6Poll
                && event.action_motion_gate_result.value_or(false);
        });
    const auto release = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::ActionMotionPublicationRelease;
        });
    const auto publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualCommandPublish
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        });
    const auto first_child = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualChildState0
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        });
    ASSERT_NE(true_gate, events.end());
    ASSERT_NE(release, events.end());
    ASSERT_NE(publication, events.end());
    ASSERT_NE(first_child, events.end());
    EXPECT_EQ(true_gate->frame_index + 1, release->frame_index);
    EXPECT_EQ(release->frame_index, publication->frame_index);
    EXPECT_EQ(publication->frame_index, first_child->frame_index);
    EXPECT_EQ(true_gate->action_motion_progress_after_bits, 0x3f800000u);
    EXPECT_EQ(true_gate->action_motion_flags_after & 0x80000000u, 0u);
    EXPECT_EQ(release->action_motion_control_before, 6);
    EXPECT_EQ(release->action_motion_control_after, 11);
    EXPECT_EQ(runtime->visual.timelines[0].instruction.runtime_instruction_mode, 8);
    EXPECT_TRUE(runtime->visual.persistent_instruction_callbacks[0].installed);
    EXPECT_EQ(
        runtime->visual.persistent_instruction_callbacks[0].callback_family,
        ActionMotionPersistentCallbackFamily::ActionMotionBasic_8001B1B0);
    EXPECT_GE(
        runtime->visual.persistent_instruction_callbacks[0]
            .publication_revision,
        2u);
    EXPECT_FALSE(battle_frame_action_visual_publication_pending(*runtime, 0));
    EXPECT_TRUE(std::none_of(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            const bool playback_event =
                event.step_kind == BattleFrameWorkerStepKind::ActionMotionPlaybackInstall
                || event.step_kind == BattleFrameWorkerStepKind::ActionMotionRendererAdvance
                || event.step_kind == BattleFrameWorkerStepKind::ActionMotionState6Poll
                || event.step_kind == BattleFrameWorkerStepKind::ActionMotionPublicationRelease;
            return playback_event && (event.rng_event || event.draws_consumed != 0);
        }));
}

TEST(SavorPredictCombatantVisualRuntime, DescriptorBackedState9DelayKeepsPublicationOnOwnerVisits) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 0, 2, 5, true)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    runtime->visual.action_view_role.valid = false;

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->selected_action_row_index = 4;
    combatant->selected_action_row_action_id = 5;
    combatant->selected_action_row_duration_bits = 0x3dccc954u;
    combatant->selected_action_row_duration_known = true;
    runtime->visual.std_row_producers[0].thread_state_0x19 = 1;

    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 0, 0, 5, true));
    std::uint32_t rng = 0x53746174u;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 32; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_non_synthetic_publications(events) > 0) {
            break;
        }
    }

    const auto true_gate = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::ActionMotionState6Poll
                && event.action_motion_gate_result.value_or(false);
        });
    const auto first_delay = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::ActionMotionPostState6Delay
                && event.action_motion_delay_lookup_performed;
        });
    const auto release = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::ActionMotionPublicationRelease;
        });
    const auto publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualCommandPublish
                && event.visual_command_kind
                    != CombatantVisualCommandKind::SyntheticActionView;
        });
    ASSERT_NE(true_gate, events.end());
    ASSERT_NE(first_delay, events.end());
    ASSERT_NE(release, events.end());
    ASSERT_NE(publication, events.end());
    EXPECT_EQ(count_step(
        events, BattleFrameWorkerStepKind::ActionMotionPostState6Delay), 13);
    EXPECT_EQ(first_delay->action_motion_delay_status, ActionMotionDelayStatus::Matched);
    EXPECT_EQ(first_delay->action_motion_delay_descriptor_record_index, 2);
    EXPECT_EQ(first_delay->action_motion_delay_before, 13);
    EXPECT_EQ(first_delay->action_motion_delay_after, 12);
    EXPECT_EQ(true_gate->frame_index + 14, release->frame_index);
    EXPECT_EQ(release->frame_index, publication->frame_index);
    EXPECT_EQ(release->action_motion_control_before, 9);
    EXPECT_EQ(release->action_motion_control_after, 11);
}

TEST(SavorPredictCombatantVisualRuntime, MovementActivationDoesNotCreateVisualEpoch) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(schedule_action(*runtime).scheduled);

    std::uint32_t rng = 0x13579bdfU;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0;
         frame < 8
            && count_step(
                events,
                BattleFrameWorkerStepKind::MovementInvocationActivate) == 0;
         ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
    }
    EXPECT_GT(
        count_step(events, BattleFrameWorkerStepKind::MovementInvocationActivate),
        0);
    EXPECT_EQ(
        count_step(events, BattleFrameWorkerStepKind::VisualInstructionInstall),
        0);
    EXPECT_TRUE(std::none_of(
        events.begin(),
        events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionDecision
                && event.action_ordinal == 0;
        }));
    EXPECT_EQ(rng, 0x13579bdfU);
}

TEST(SavorPredictCombatantVisualRuntime, Mode1RunsOnRecordVisitBeforeActionResolution) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 1)));
    configure_battle_frame_visual_pathing_profile(
        *runtime, "first-battle-soldiers");
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x31415926U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto find_mode1 = [&]() -> BattleFrameVisualChildTask* {
        const auto found = std::find_if(
            runtime->visual.child_tasks.begin(),
            runtime->visual.child_tasks.end(),
            [](const BattleFrameVisualChildTask& task) {
                return task.kind == BattleFrameVisualChildKind::ActionViewRecord
                    && task.effective_mode == 1;
        });
        return found == runtime->visual.child_tasks.end() ? nullptr : &*found;
    };
    int pathing_events = 0;
    for (int frame = 0; frame < 16; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        pathing_events += count_step(
            step.events,
            BattleFrameWorkerStepKind::VisualMode1Pathing);
        if (find_mode1() != nullptr && find_mode1()->mode1_pathing_consumed) {
            break;
        }
    }
    ASSERT_NE(find_mode1(), nullptr);
    EXPECT_TRUE(find_mode1()->mode1_pathing_consumed);
    EXPECT_GT(find_mode1()->visits, 0);
    ASSERT_TRUE(runtime->active_action.has_value());
    EXPECT_FALSE(runtime->active_action->action_resolution_available);
    EXPECT_GT(pathing_events, 0);
}

TEST(SavorPredictCombatantVisualRuntime, ServiceDelayRetainsState3UntilNextVisitCleanup) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0);
    resource.records.erase(resource.records.begin() + 1);
    resource.selector_table = {};
    resource.selector_table = combatant_visual_selector_table(resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x24681357U;
    const auto placement = draw_rand15(rng);
    std::vector<BattleFrameStepEvent> events;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    bool action_released = false;
    for (int frame = 0; frame < 6; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (!action_released
            && count_non_synthetic_publications(events) > 0) {
            runtime->active_action.reset();
            action_released = true;
        }
    }

    EXPECT_EQ(rng, placement.next_state);
    EXPECT_EQ(count_step_for_command(
        events,
        BattleFrameWorkerStepKind::VisualChildState0,
        CombatantVisualCommandKind::SetCommand), 1);
    EXPECT_EQ(count_step_for_command(
        events,
        BattleFrameWorkerStepKind::VisualChildDelay,
        CombatantVisualCommandKind::SetCommand), 2);
    EXPECT_EQ(count_step_for_command(
        events,
        BattleFrameWorkerStepKind::VisualChildNested,
        CombatantVisualCommandKind::SetCommand), 1);
    EXPECT_EQ(count_step_for_command(
        events,
        BattleFrameWorkerStepKind::VisualChildCleanup,
        CombatantVisualCommandKind::SetCommand), 1);
    const auto nested = std::find_if(
        events.begin(),
        events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildNested
                && event.visual_command_kind
                    == CombatantVisualCommandKind::SetCommand;
        });
    const auto cleanup = std::find_if(
        events.begin(),
        events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildCleanup
                && event.visual_command_kind
                    == CombatantVisualCommandKind::SetCommand;
        });
    ASSERT_NE(nested, events.end());
    ASSERT_NE(cleanup, events.end());
    EXPECT_GT(cleanup->frame_index, nested->frame_index);
}

TEST(SavorPredictCombatantVisualRuntime, FlaggedActionServiceIsTheEb4cRngOwner) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0, false, 2, 0);
    resource.records.erase(resource.records.begin() + 1);
    resource.selector_table = {};
    resource.selector_table = combatant_visual_selector_table(resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
    auto target_resource = decoded_resource(4, false, 2, 0);
    add_direct_transition_rows(target_resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, std::move(target_resource)));
    auto* origin = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(origin, nullptr);
    origin->instruction_flags_0xec |= 0x00100000U;
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x13572468U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto state0 = run_until_visual_publication(*runtime, rng);
    EXPECT_EQ(count_step_for_command(
        state0,
        BattleFrameWorkerStepKind::VisualChildState0,
        CombatantVisualCommandKind::SetCommand), 1);
    runtime->active_action.reset();
    const auto expected = draw_rand15(rng);
    const auto nested = run_first_turn_frame(*runtime, rng);

    EXPECT_EQ(count_step(nested.events, BattleFrameWorkerStepKind::VisualChildNested), 1);
    const auto eb4c = std::find_if(
        nested.events.begin(), nested.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.rng_label == "fun_8002eb4c_action_service";
        });
    ASSERT_NE(eb4c, nested.events.end());
    EXPECT_EQ(eb4c->draws_consumed, 1);
    EXPECT_EQ(eb4c->rand_value, expected.value);
    EXPECT_EQ(rng, expected.next_state);
    EXPECT_EQ(origin->instruction_flags_0xec & 0x00100000U, 0U);
}

TEST(SavorPredictCombatantVisualRuntime, SerializedChildHoldsActionBarrierUntilCleanup) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0, false, 2);
    resource.records.erase(resource.records.begin());
    resource.selector_table = {};
    resource.selector_table = combatant_visual_selector_table(resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    runtime->visual.action_view_role.valid = false;

    std::uint32_t rng = 0x10203040U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto published = run_until_visual_publication(*runtime, rng);
    ASSERT_EQ(count_step_for_command(
        published,
        BattleFrameWorkerStepKind::VisualCommandPublish,
        CombatantVisualCommandKind::SystemCamera), 1);
    const auto camera_task = std::find_if(
        runtime->visual.child_tasks.begin(),
        runtime->visual.child_tasks.end(),
        [](const BattleFrameVisualChildTask& task) {
            return task.command_kind
                == CombatantVisualCommandKind::SystemCamera;
        });
    ASSERT_NE(camera_task, runtime->visual.child_tasks.end());
    const int camera_sequence = camera_task->sequence;
    for (auto& worker : runtime->workers) {
        worker.complete = true;
    }
    runtime->passive_participants = {};
    ASSERT_TRUE(runtime->active_action.has_value());
    runtime->active_action->completion_gate_open = true;
    runtime->active_action->passive_completion_mask = 0;
    runtime->active_action->phase = BattleFrameActionPhase::Draining;

    const auto blocked = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(runtime->active_action->phase, BattleFrameActionPhase::Draining);
    EXPECT_EQ(count_step(blocked.events, BattleFrameWorkerStepKind::ActionComplete), 0);
    const auto find_camera = [&]() -> BattleFrameVisualChildTask* {
        const auto found = std::find_if(
            runtime->visual.child_tasks.begin(),
            runtime->visual.child_tasks.end(),
            [camera_sequence](const BattleFrameVisualChildTask& task) {
                return task.sequence == camera_sequence;
            });
        return found == runtime->visual.child_tasks.end() ? nullptr : &*found;
    };
    ASSERT_NE(find_camera(), nullptr);
    EXPECT_FALSE(find_camera()->complete);

    find_camera()->maximum_visits = find_camera()->visits + 1;
    const auto released = run_first_turn_frame(*runtime, rng);
    ASSERT_NE(find_camera(), nullptr);
    EXPECT_TRUE(find_camera()->complete);
    EXPECT_EQ(runtime->active_action->phase, BattleFrameActionPhase::Complete);
    EXPECT_EQ(count_step(released.events, BattleFrameWorkerStepKind::ActionComplete), 1);
}

TEST(SavorPredictCombatantVisualRuntime, Mode0RewriteAndMode0eDrawHaveSeparateOwners) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0, true, 0);
    resource.records.erase(resource.records.begin());
    resource.selector_table = {};
    resource.selector_table = combatant_visual_selector_table(resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);
    runtime->visual.action_view_role.valid = false;

    std::uint32_t rng = 1;
    while ((draw_rand15(draw_rand15(rng).next_state).value % 2U) != 0U) {
        ++rng;
    }
    const auto placement = draw_rand15(rng);
    const auto first = draw_rand15(placement.next_state);
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto frame = run_until_visual_publication(*runtime, rng);

    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0Rewrite), 1);
    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0eCamera), 1);
    EXPECT_EQ(rng, first.next_state);
}

TEST(SavorPredictCombatantVisualRuntime, MissingResourceDoesNotInventRng) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    CombatantVisualResource unavailable;
    unavailable.binding = {
        .slot = 0,
        .resource_stem = "missing",
    };
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(unavailable)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0xabcdef01U;
    const auto placement = draw_rand15(rng);
    std::vector<BattleFrameStepEvent> frame;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    for (int index = 0; index < 3; ++index) {
        const auto step = run_first_turn_frame(*runtime, rng);
        frame.insert(frame.end(), step.events.begin(), step.events.end());
    }
    EXPECT_EQ(rng, placement.next_state);
    EXPECT_GE(count_step(frame, BattleFrameWorkerStepKind::ViewPlacementResolve), 1);
    EXPECT_EQ(
        count_step_for_command(
            frame,
            BattleFrameWorkerStepKind::VisualCommandPublish,
            CombatantVisualCommandKind::SyntheticActionView),
        1);
    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0Rewrite), 0);
    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0eCamera), 0);
}

TEST(SavorPredictCombatantVisualRuntime, ReplaysAcceptedFourteenServiceVisitContracts) {
    struct ServiceVector {
        int delay;
        bool eb4c_gate;
    };
    const std::vector<ServiceVector> vectors{
        {24, false}, {19, true}, {14, false}, {19, false},
        {14, true}, {14, false}, {19, true}, {19, false},
        {24, false}, {19, true}, {19, false}, {19, false},
        {14, false}, {35, false},
    };

    for (const auto& captured : vectors) {
        auto runtime = initialize_frame_runtime();
        ASSERT_TRUE(runtime.has_value());
        auto resource = decoded_resource(
            0, false, 2, static_cast<std::int16_t>(captured.delay));
        resource.records.erase(resource.records.begin() + 1);
        resource.selector_table = combatant_visual_selector_table(resource);
        ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
        auto target_resource = decoded_resource(4, false, 2, 0);
        add_direct_transition_rows(target_resource);
        ASSERT_TRUE(configure_battle_frame_visual_resource(
            *runtime, std::move(target_resource)));
        auto* origin = find_frame_combatant(runtime->state, 0);
        ASSERT_NE(origin, nullptr);
        origin->instruction_flags_0xec = captured.eb4c_gate ? 0x00180000U : 0x00080000U;
        ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

        std::uint32_t rng = 0x31415926U;
        std::vector<BattleFrameStepEvent> events;
        ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
        bool action_released = false;
        for (int frame = 0; frame < captured.delay + 5; ++frame) {
            const auto step = run_first_turn_frame(*runtime, rng);
            events.insert(events.end(), step.events.begin(), step.events.end());
            if (!action_released
                && count_non_synthetic_publications(events) > 0) {
                runtime->active_action.reset();
                action_released = true;
            }
        }

        EXPECT_EQ(count_step_for_command(
            events,
            BattleFrameWorkerStepKind::VisualChildState0,
            CombatantVisualCommandKind::SetCommand), 1);
        EXPECT_EQ(
            count_step_for_command(
                events,
                BattleFrameWorkerStepKind::VisualChildDelay,
                CombatantVisualCommandKind::SetCommand),
            captured.delay);
        EXPECT_EQ(count_step_for_command(
            events,
            BattleFrameWorkerStepKind::VisualChildNested,
            CombatantVisualCommandKind::SetCommand), 1);
        EXPECT_EQ(count_step_for_command(
            events,
            BattleFrameWorkerStepKind::VisualChildCleanup,
            CombatantVisualCommandKind::SetCommand), 1);
        EXPECT_EQ(
            count_rng_label(events, "fun_8002eb4c_action_service"),
            captured.eb4c_gate ? 1 : 0);
    }
}

TEST(SavorPredictCombatantVisualModel, ReplaysAcceptedTwentySevenRecordRngContracts) {
    struct RecordVector {
        std::int16_t payload_mode;
        std::int16_t effective_mode;
        bool mode0_draw;
        bool mode0e_draw;
        bool mode1_callback;
    };
    const std::vector<RecordVector> vectors{
        {0x11, 0x11, false, false, false}, {1, 1, false, false, true},
        {0x11, 0x11, false, false, false}, {2, 2, false, false, false},
        {0, 0, true, false, false}, {0x11, 0x11, false, false, false},
        {2, 2, false, false, false}, {0x0e, 0x0e, false, true, false},
        {0x11, 0x11, false, false, false}, {1, 1, false, false, true},
        {0x11, 0x11, false, false, false}, {2, 2, false, false, false},
        {0, 0, true, false, false}, {0x11, 0x11, false, false, false},
        {2, 2, false, false, false}, {0x0e, 0x0e, false, true, false},
        {0x11, 0x11, false, false, false}, {1, 1, false, false, true},
        {0x11, 0x11, false, false, false}, {0x0e, 0x0e, false, true, false},
        {0x11, 0x11, false, false, false}, {2, 2, false, false, false},
        {0x0e, 0x0e, false, true, false}, {0, 0, true, false, false},
        {0x11, 0x11, false, false, false}, {2, 2, false, false, false},
        {0, 0x0e, true, true, false},
    };

    ASSERT_EQ(vectors.size(), 27u);
    for (const auto& captured : vectors) {
        const auto plan = combatant_visual_action_view_rng_plan(
            captured.payload_mode,
            captured.effective_mode);
        EXPECT_EQ(plan.mode0_rewrite_draw, captured.mode0_draw);
        EXPECT_EQ(plan.mode0e_camera_draw, captured.mode0e_draw);
        EXPECT_EQ(plan.mode1_pathing_callback, captured.mode1_callback);
    }
}

} // namespace
