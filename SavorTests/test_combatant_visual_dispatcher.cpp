#include "../SavorPredict/ActionViewStdJsonLoader.h"
#include "../SavorPredict/BattleFrameSchedulerModel.h"
#include "../SavorPredict/BattleSourceModel.h"
#include "../SavorPredict/RngCore.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
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

std::string visual_json(
    const std::vector<std::uint8_t>& set_payload,
    const std::vector<std::uint8_t>& camera_payload) {
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
         << bytes_hex(camera_payload) << "\"},"
         << "{\"index\":2,\"locationCode\":-1,\"opcode\":0,"
         << "\"payloadSize\":0,\"payloadInBounds\":true,"
         << "\"payloadBytesHex\":\"\"}]}}";
    return json.str();
}

CombatantVisualResource decoded_resource(
    int slot,
    bool mode0_rewrite_gate = false,
    std::int16_t camera_mode = 0x0e,
    std::int16_t delay = 2,
    std::int16_t action_key = 5) {
    auto loaded = load_spice_std_visual_resource_from_json_text(
        visual_json(
            set_command_payload(delay, action_key),
            system_camera_payload(camera_mode, action_key)));
    EXPECT_TRUE(loaded.ok);
    loaded.resource.binding = {
        .slot = slot,
        .resource_stem = "fixture",
        .mode0_rewrite_gate = mode0_rewrite_gate,
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
        combatant.selected_action_row_index = 0;
        combatant.selected_action_row_flags = 0x01000000u;
        combatant.selected_action_row_action_id = 5;
        combatant.selected_action_row_known = true;
    }
    return runtime;
}

bool publish_fixture_visual_instruction_state(
    BattleFrameRuntime& runtime,
    int slot = 0,
    int action_ordinal = 0,
    std::int16_t mode = 5) {
    auto* combatant = find_frame_combatant(runtime.state, slot);
    if (combatant == nullptr) {
        return false;
    }
    runtime.visual.timelines[static_cast<std::size_t>(slot)] = {};
    runtime.visual.timeline_action_ordinals[static_cast<std::size_t>(slot)] = -1;
    runtime.visual.controller = {};
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
    return publish_battle_frame_visual_instruction_state(
        runtime,
        action_ordinal,
        std::move(instruction));
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

int count_rng_label(
    const std::vector<BattleFrameStepEvent>& events,
    const std::string& label) {
    return static_cast<int>(std::count_if(
        events.begin(), events.end(),
        [&label](const BattleFrameStepEvent& event) { return event.rng_label == label; }));
}

std::vector<BattleFrameStepEvent> run_until_visual_publication(
    BattleFrameRuntime& runtime,
    std::uint32_t& rng,
    int max_frames = 16) {
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < max_frames; ++frame) {
        const auto step = run_first_turn_frame(runtime, rng);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_step(events, BattleFrameWorkerStepKind::VisualCommandPublish) > 0) {
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
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualCommandPublish), 2);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildState0), 2);
    const auto publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualCommandPublish;
        });
    const auto child = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildState0;
        });
    ASSERT_NE(publication, events.end());
    ASSERT_NE(child, events.end());
    EXPECT_LT(std::distance(events.begin(), publication),
              std::distance(events.begin(), child));
}

TEST(SavorPredictCombatantVisualRuntime, ControllerPublicationWaitsForState1StdRowProducer) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(schedule_action(*runtime).scheduled);

    std::uint32_t rng = 0x22446688U;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 512; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_step(events, BattleFrameWorkerStepKind::VisualInstructionInstall) > 0) {
            break;
        }
    }
    const auto state_publication = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                    == BattleFrameWorkerStepKind::VisualInstructionStatePublish
                && event.slot == 0;
        });
    const auto epoch_install = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualInstructionInstall
                && event.slot == 0
                && event.detail.find("FUN_80022850_state1") != std::string::npos;
        });
    ASSERT_NE(state_publication, events.end());
    ASSERT_NE(epoch_install, events.end());
    EXPECT_LT(std::distance(events.begin(), state_publication),
              std::distance(events.begin(), epoch_install));
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualInstructionDecision), 0);
    EXPECT_EQ(runtime->visual.timelines[0].instruction.runtime_instruction_mode, 6);
    EXPECT_EQ(rng, 0x22446688U);
}

TEST(SavorPredictCombatantVisualRuntime, ValidatedStateTransitionWaitsForNextState1Visit) {
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

    ASSERT_TRUE(publish_battle_frame_validated_instruction_transition(
        *runtime,
        0,
        0,
        4,
        8,
        "validated attack-result mode-8 state transition"));
    EXPECT_EQ(runtime->visual.timelines[0].epoch, epoch_before);
    EXPECT_EQ(runtime->visual.child_tasks.size(), task_count_before);

    const auto next_visit = run_first_turn_frame(*runtime, rng);
    EXPECT_EQ(runtime->visual.timelines[0].epoch, epoch_before + 1);
    EXPECT_EQ(runtime->visual.timelines[0].instruction.runtime_instruction_mode, 8);
    const auto state_event = std::find_if(
        next_visit.events.begin(), next_visit.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::VisualInstructionStatePublish;
        });
    const auto install_event = std::find_if(
        next_visit.events.begin(), next_visit.events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind
                == BattleFrameWorkerStepKind::VisualInstructionInstall;
        });
    ASSERT_NE(state_event, next_visit.events.end());
    ASSERT_NE(install_event, next_visit.events.end());
    EXPECT_LT(std::distance(next_visit.events.begin(), state_event),
              std::distance(next_visit.events.begin(), install_event));
}

TEST(SavorPredictCombatantVisualRuntime, Mode8PublicationWaitsForCapturedState6Playback) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(configure_battle_frame_visual_resource(
        *runtime, decoded_resource(0, false, 0, 2, 8)));
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    auto* combatant = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(combatant, nullptr);
    combatant->selected_action_row_index = 4;
    combatant->selected_action_row_action_id = 8;
    combatant->selected_action_row_duration_bits = 0x40a00000u;
    combatant->selected_action_row_duration_known = true;
    runtime->visual.std_row_producers[0].thread_state_0x19 = 1;

    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime, 0, 0, 8));
    EXPECT_TRUE(battle_frame_action_visual_publication_pending(*runtime, 0));
    std::uint32_t rng = 0x53746174u;
    std::vector<BattleFrameStepEvent> events;
    for (int frame = 0; frame < 12; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        ASSERT_TRUE(step.ok);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (count_step(events, BattleFrameWorkerStepKind::VisualCommandPublish) > 0) {
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
            return event.step_kind == BattleFrameWorkerStepKind::VisualCommandPublish;
        });
    const auto first_child = std::find_if(
        events.begin(), events.end(),
        [](const BattleFrameStepEvent& event) {
            return event.step_kind == BattleFrameWorkerStepKind::VisualChildState0;
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

TEST(SavorPredictCombatantVisualRuntime, MovementActivationDoesNotCreateVisualEpoch) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    ASSERT_TRUE(schedule_action(*runtime).scheduled);

    std::uint32_t rng = 0x13579bdfU;
    const auto first = run_first_turn_frame(*runtime, rng);
    EXPECT_GT(
        count_step(first.events, BattleFrameWorkerStepKind::MovementInvocationActivate),
        0);
    EXPECT_EQ(
        count_step(first.events, BattleFrameWorkerStepKind::VisualInstructionInstall),
        0);
    EXPECT_EQ(count_step(
        first.events,
        BattleFrameWorkerStepKind::VisualInstructionDecision), 0);
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

TEST(SavorPredictCombatantVisualRuntime, ServiceDelayHasNDecrementsAndZeroVisitCleanup) {
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
    for (int frame = 0; frame < 6; ++frame) {
        const auto step = run_first_turn_frame(*runtime, rng);
        events.insert(events.end(), step.events.begin(), step.events.end());
        if (frame == 0) {
            runtime->active_action.reset();
        }
    }

    EXPECT_EQ(rng, placement.next_state);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildState0), 1);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildDelay), 2);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildNested), 1);
    EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildCleanup), 1);
}

TEST(SavorPredictCombatantVisualRuntime, FlaggedActionServiceIsTheEb4cRngOwner) {
    auto runtime = initialize_frame_runtime();
    ASSERT_TRUE(runtime.has_value());
    auto resource = decoded_resource(0, false, 2, 0);
    resource.records.erase(resource.records.begin() + 1);
    resource.selector_table = {};
    resource.selector_table = combatant_visual_selector_table(resource);
    ASSERT_TRUE(configure_battle_frame_visual_resource(*runtime, std::move(resource)));
    auto* origin = find_frame_combatant(runtime->state, 0);
    ASSERT_NE(origin, nullptr);
    origin->instruction_flags_0xec |= 0x00100000U;
    ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

    std::uint32_t rng = 0x13572468U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto state0 = run_until_visual_publication(*runtime, rng);
    EXPECT_EQ(count_step(state0, BattleFrameWorkerStepKind::VisualChildState0), 1);
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

    std::uint32_t rng = 0x10203040U;
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto published = run_until_visual_publication(*runtime, rng);
    ASSERT_EQ(count_step(
        published,
        BattleFrameWorkerStepKind::VisualCommandPublish), 1);
    ASSERT_EQ(runtime->visual.child_tasks.size(), 1u);
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
    EXPECT_FALSE(runtime->visual.child_tasks[0].complete);

    runtime->visual.child_tasks[0].maximum_visits =
        runtime->visual.child_tasks[0].visits + 1;
    const auto released = run_first_turn_frame(*runtime, rng);
    EXPECT_TRUE(runtime->visual.child_tasks[0].complete);
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

    std::uint32_t rng = 1;
    while ((draw_rand15(draw_rand15(rng).next_state).value % 2U) != 0U) {
        ++rng;
    }
    const auto placement = draw_rand15(rng);
    const auto first = draw_rand15(placement.next_state);
    const auto second = draw_rand15(first.next_state);
    ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
    const auto frame = run_until_visual_publication(*runtime, rng);

    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0Rewrite), 1);
    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualMode0eCamera), 1);
    EXPECT_EQ(rng, second.next_state);
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
    EXPECT_EQ(count_step(frame, BattleFrameWorkerStepKind::VisualCommandPublish), 0);
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
        auto* origin = find_frame_combatant(runtime->state, 0);
        ASSERT_NE(origin, nullptr);
        origin->instruction_flags_0xec = captured.eb4c_gate ? 0x00180000U : 0x00080000U;
        ASSERT_TRUE(schedule_action(*runtime, false).scheduled);

        std::uint32_t rng = 0x31415926U;
        std::vector<BattleFrameStepEvent> events;
        ASSERT_TRUE(publish_fixture_visual_instruction_state(*runtime));
        for (int frame = 0; frame < captured.delay + 5; ++frame) {
            const auto step = run_first_turn_frame(*runtime, rng);
            events.insert(events.end(), step.events.begin(), step.events.end());
            if (frame == 0) {
                runtime->active_action.reset();
            }
        }

        EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildState0), 1);
        EXPECT_EQ(
            count_step(events, BattleFrameWorkerStepKind::VisualChildDelay),
            captured.delay);
        EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildNested), 1);
        EXPECT_EQ(count_step(events, BattleFrameWorkerStepKind::VisualChildCleanup), 1);
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
