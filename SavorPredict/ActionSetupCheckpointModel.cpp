#include "ActionSetupCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <utility>

namespace savor::predict {

namespace {

constexpr std::string_view kEnemySetupOwner = "enemy_attack_execution_setup";
constexpr std::string_view kAttackHitOwner = "attack_hit_dodge";
constexpr const char* kPcHandlerPc = "80086C68";
constexpr const char* kEnemyHandlerPc = "8008B9E0";
constexpr const char* kEnemyWorkerParam0Pc = "80087F6C";
constexpr const char* kEnemyWorkerFallbackPc = "80087844";

std::string normalize_pc_string(std::string value) {
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool owner_is(const CheckpointEvent& event, std::string_view owner) {
    return event.known_rng_owner == owner;
}

std::optional<int> parse_field_int(const CheckpointEvent& event, const char* key) {
    const auto found = event.fields.find(key);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    char* end = nullptr;
    const long parsed = std::strtol(found->second.c_str(), &end, 0);
    if (end == found->second.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

std::optional<int> parse_first_field_int(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> parse_first_field_pc(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found != event.fields.end() && !found->second.empty()) {
            return normalize_pc_string(found->second);
        }
    }
    return std::nullopt;
}

std::optional<bool> parse_field_bool(const CheckpointEvent& event, const char* key) {
    const auto found = event.fields.find(key);
    if (found == event.fields.end()) {
        return std::nullopt;
    }

    std::string lowered = found->second;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (lowered == "1" || lowered == "true" || lowered == "yes") {
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no") {
        return false;
    }
    return std::nullopt;
}

std::optional<bool> parse_first_field_bool(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_field_bool(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

bool event_named(const CheckpointEvent& event, std::initializer_list<std::string_view> names) {
    for (const auto name : names) {
        if (event.function == name || event.checkpoint == name) {
            return true;
        }
    }
    return false;
}

bool is_setup_action_checkpoint(const CheckpointEvent& event) {
    return event.pc == "800708C0"
        || event_named(event, {
            "Battle::Run::setupAction",
            "setupAction",
            "setup_action",
            "action_setup",
        });
}

bool is_pc_handler_checkpoint(const CheckpointEvent& event) {
    return event.pc == kPcHandlerPc
        || event_named(event, {
            "Battle::HandlePCInst",
            "HandlePCInst",
            "pc_inst",
            "pc_handler",
        });
}

bool is_enemy_handler_checkpoint(const CheckpointEvent& event) {
    return event.pc == kEnemyHandlerPc
        || event_named(event, {
            "Battle::HandleECInst",
            "HandleECInst",
            "ec_inst",
            "enemy_handler",
        });
}

std::optional<int> actor_slot_from_event(const CheckpointEvent& event) {
    if (event.active_slot.has_value()) {
        return event.active_slot;
    }
    return parse_first_field_int(event, {"actor_slot", "acting_slot", "slot", "active_slot"});
}

std::optional<int> target_slot_from_event(const CheckpointEvent& event) {
    if (event.target_slot.has_value()) {
        return event.target_slot;
    }
    return parse_first_field_int(event, {"target_slot", "target"});
}

std::string expected_enemy_worker_pc_for_final_param(int final_instr_param_0x6) {
    return final_instr_param_0x6 == 0 ? kEnemyWorkerParam0Pc : kEnemyWorkerFallbackPc;
}

bool has_required_enemy_helper_fields(const ActionSetupCheckpointEvent& event) {
    if (!event.final_instr_param_0x6.has_value()
        || !event.direct_close_candidate.has_value()
        || !event.selected_worker_pc.has_value()) {
        return false;
    }

    if (*event.direct_close_candidate == 0) {
        return event.helper_8008a280_reached.has_value()
            && event.target_adjacent.has_value();
    }

    if (!event.helper_8008a174_result.has_value()) {
        return false;
    }

    if (*event.helper_8008a174_result != 0) {
        if (!event.helper_80082340_result.has_value()) {
            return false;
        }
        if (*event.helper_80082340_result == 0 && !event.target_distance.has_value()) {
            return false;
        }
        if (*event.helper_80082340_result == 0
            && event.target_distance.has_value()
            && *event.target_distance < 5) {
            return true;
        }
    }

    return event.helper_8008a280_reached.has_value()
        && event.target_adjacent.has_value();
}

const ActionSetupCheckpointEvent* find_matching_setup_event(
    const std::vector<ActionSetupCheckpointEvent>& setup_events,
    const ActionSetupCheckpointEvent& handler_event) {
    if (!handler_event.actor_slot.has_value()) {
        return nullptr;
    }

    for (auto it = setup_events.rbegin(); it != setup_events.rend(); ++it) {
        if (!it->actor_slot.has_value() || *it->actor_slot != *handler_event.actor_slot) {
            continue;
        }
        if (it->draw_index.has_value()
            && handler_event.draw_index.has_value()
            && *it->draw_index > *handler_event.draw_index) {
            continue;
        }
        return &*it;
    }
    return nullptr;
}

void record_setup_handler_field_match(
    ActionSetupCheckpointSummary& summary,
    bool pc_handler,
    std::optional<bool> field_matches) {
    if (!field_matches.has_value()) {
        return;
    }

    ++summary.setup_handler_field_comparisons;
    if (pc_handler) {
        ++summary.pc_setup_handler_field_comparisons;
    } else {
        ++summary.enemy_setup_handler_field_comparisons;
    }

    if (*field_matches) {
        ++summary.setup_handler_field_matches;
        if (pc_handler) {
            ++summary.pc_setup_handler_field_matches;
        } else {
            ++summary.enemy_setup_handler_field_matches;
        }
    } else {
        ++summary.setup_handler_field_mismatches;
        if (pc_handler) {
            ++summary.pc_setup_handler_field_mismatches;
        } else {
            ++summary.enemy_setup_handler_field_mismatches;
        }
    }
}

std::optional<bool> compare_optional_ints(std::optional<int> lhs, std::optional<int> rhs) {
    if (!lhs.has_value() || !rhs.has_value()) {
        return std::nullopt;
    }
    return *lhs == *rhs;
}

void compare_setup_to_handler(
    ActionSetupCheckpointSummary& summary,
    const std::vector<ActionSetupCheckpointEvent>& setup_events,
    ActionSetupCheckpointEvent& handler_event,
    bool pc_handler) {
    const auto* setup_event = find_matching_setup_event(setup_events, handler_event);
    if (setup_event == nullptr) {
        return;
    }

    handler_event.matched_setup_draw_index = setup_event->draw_index;
    handler_event.instruction_matches_setup =
        compare_optional_ints(handler_event.instruction, setup_event->instruction);
    handler_event.target_slot_matches_setup =
        compare_optional_ints(handler_event.target_slot, setup_event->target_slot);
    handler_event.instr_param_matches_setup =
        compare_optional_ints(handler_event.instr_param_0x6, setup_event->instr_param_0x6);

    record_setup_handler_field_match(
        summary,
        pc_handler,
        handler_event.instruction_matches_setup);
    record_setup_handler_field_match(
        summary,
        pc_handler,
        handler_event.target_slot_matches_setup);
    record_setup_handler_field_match(
        summary,
        pc_handler,
        handler_event.instr_param_matches_setup);
}

ActionSetupCheckpointStatus classify_status(const ActionSetupCheckpointSummary& summary) {
    if (summary.handler_mismatches > 0) {
        return ActionSetupCheckpointStatus::HandlerMismatch;
    }
    if (summary.setup_handler_field_mismatches > 0) {
        return ActionSetupCheckpointStatus::SetupHandlerFieldMismatch;
    }
    if (summary.observed_setup_action_events > 0
        && (summary.setup_events_with_actor_slot < summary.observed_setup_action_events
            || summary.setup_events_with_handler_pc < summary.observed_setup_action_events
            || summary.setup_events_with_instruction < summary.observed_setup_action_events
            || summary.setup_events_with_target_slot < summary.observed_setup_action_events
            || summary.setup_events_with_instr_param < summary.observed_setup_action_events)) {
        return ActionSetupCheckpointStatus::MissingLiveSetupFields;
    }
    const int handler_entries = summary.observed_pc_handler_entries + summary.observed_enemy_handler_entries;
    if (handler_entries > 0
        && (summary.handler_entries_with_instruction < handler_entries
            || summary.handler_entries_with_target_slot < handler_entries
            || summary.handler_entries_with_instr_param < handler_entries)) {
        return ActionSetupCheckpointStatus::MissingLiveSetupFields;
    }
    if (summary.expected_enemy_setup_draws.has_value()) {
        if (summary.observed_enemy_setup_draws < *summary.expected_enemy_setup_draws) {
            return ActionSetupCheckpointStatus::MissingEnemySetupDraws;
        }
        if (summary.observed_enemy_setup_draws > *summary.expected_enemy_setup_draws) {
            return ActionSetupCheckpointStatus::ExtraEnemySetupDraws;
        }
        if (summary.worker_mismatches > 0) {
            return ActionSetupCheckpointStatus::WorkerMismatch;
        }
        if (summary.observed_enemy_setup_draws > 0
            && summary.enemy_setup_draws_with_required_helper_fields < summary.observed_enemy_setup_draws) {
            return ActionSetupCheckpointStatus::MissingEnemySetupHelperFields;
        }
        return ActionSetupCheckpointStatus::MatchesExpected;
    }
    if (summary.observed_setup_action_events == 0
        && handler_entries == 0
        && summary.observed_enemy_setup_draws == 0) {
        return ActionSetupCheckpointStatus::ObservedOnly;
    }
    return ActionSetupCheckpointStatus::ObservedOnly;
}

void maybe_set_first(std::optional<int>& target, const std::optional<int>& value) {
    if (!target.has_value() && value.has_value()) {
        target = *value;
    }
}

void update_handler_match_counts(ActionSetupCheckpointSummary& summary, ActionSetupCheckpointEvent& observed) {
    if (!observed.actor_slot.has_value()) {
        return;
    }
    observed.expected_handler_pc = expected_action_setup_handler_pc_for_slot(*observed.actor_slot);
    if (!observed.handler_pc.has_value()) {
        return;
    }
    if (*observed.handler_pc == *observed.expected_handler_pc) {
        ++summary.handler_matches;
    } else {
        ++summary.handler_mismatches;
    }
}

ActionSetupCheckpointEvent make_common_event(
    const CheckpointEvent& event,
    ActionSetupCheckpointKind kind) {
    ActionSetupCheckpointEvent observed;
    observed.kind = kind;
    observed.draw_index = event.rng_draw_index_before;
    observed.actor_slot = actor_slot_from_event(event);
    observed.target_slot = target_slot_from_event(event);
    observed.instruction =
        parse_first_field_int(event, {"instruction", "queued_instruction", "instruction_0x0"});
    observed.instr_param_0x6 =
        parse_first_field_int(event, {"instr_param_0x6", "instr_param", "param_0x6"});
    observed.movement_flags =
        parse_first_field_int(event, {"movement_flags", "actor_movement_flags"});
    observed.handler_pc =
        parse_first_field_pc(event, {"handler_pc", "selected_handler_pc", "next_fxn_0x10"});
    return observed;
}

} // namespace

std::string expected_action_setup_handler_pc_for_slot(int actor_slot) {
    return actor_slot < 4 ? kPcHandlerPc : kEnemyHandlerPc;
}

ActionSetupCheckpointSummary summarize_action_setup_checkpoints(
    const std::vector<CheckpointEvent>& events,
    std::optional<int> expected_enemy_setup_draws) {
    ActionSetupCheckpointSummary summary;
    summary.expected_enemy_setup_draws = expected_enemy_setup_draws;
    std::vector<ActionSetupCheckpointEvent> setup_events;

    for (const auto& event : events) {
        if (owner_is(event, kAttackHitOwner)) {
            maybe_set_first(summary.first_attack_hit_draw_index, event.rng_draw_index_before);
            continue;
        }

        if (is_setup_action_checkpoint(event)) {
            auto observed = make_common_event(event, ActionSetupCheckpointKind::SetupAction);
            ++summary.observed_setup_action_events;
            maybe_set_first(summary.first_setup_action_draw_index, observed.draw_index);
            if (observed.actor_slot.has_value()) {
                ++summary.setup_events_with_actor_slot;
            }
            if (observed.handler_pc.has_value()) {
                ++summary.setup_events_with_handler_pc;
            }
            if (observed.instruction.has_value()) {
                ++summary.setup_events_with_instruction;
            }
            if (observed.target_slot.has_value()) {
                ++summary.setup_events_with_target_slot;
            }
            if (observed.instr_param_0x6.has_value()) {
                ++summary.setup_events_with_instr_param;
            }
            update_handler_match_counts(summary, observed);
            setup_events.push_back(observed);
            summary.events.push_back(std::move(observed));
            continue;
        }

        if (owner_is(event, kEnemySetupOwner)) {
            auto observed = make_common_event(event, ActionSetupCheckpointKind::EnemySetupDraw);
            observed.setup_rand =
                parse_first_field_int(event, {"setup_rand", "rand_value", "enemy_setup_rand"});
            observed.setup_rand_mod10 =
                parse_first_field_int(event, {"setup_rand_mod10", "rand_mod10"});
            observed.direct_close_candidate =
                parse_first_field_int(event, {"direct_close_candidate", "direct_close_branch_candidate"});
            observed.final_instr_param_0x6 = parse_first_field_int(
                event,
                {"final_instr_param_0x6", "instr_param_after", "final_instr_param", "instr_param_final"});
            observed.helper_8008a174_result = parse_first_field_int(
                event,
                {"helper_8008a174_result", "fun_8008a174_result", "target_repair_result"});
            observed.helper_8008a280_reached = parse_first_field_bool(
                event,
                {"helper_8008a280_reached", "fun_8008a280_reached", "target_scope_helper_reached"});
            observed.helper_80082340_result = parse_first_field_int(
                event,
                {"helper_80082340_result", "fun_80082340_result", "distance_helper_result"});
            observed.target_adjacent = parse_first_field_int(
                event,
                {"target_adjacent", "check_target_adjacent", "checkTargetAdjacent_result"});
            observed.target_distance =
                parse_first_field_int(event, {"target_distance", "dist_to_target_0x14"});
            observed.selected_worker_pc =
                parse_first_field_pc(event, {"selected_worker_pc", "worker_pc", "next_function_pc"});

            ++summary.observed_enemy_setup_draws;
            maybe_set_first(summary.first_enemy_setup_draw_index, observed.draw_index);
            if (observed.instruction.has_value() && observed.movement_flags.has_value()) {
                ++summary.enemy_setup_draws_with_gate_inputs;
            }
            if (observed.setup_rand.has_value()) {
                ++summary.enemy_setup_draws_with_rand_value;
            }
            if (observed.setup_rand_mod10.has_value()) {
                ++summary.enemy_setup_draws_with_rand_mod10;
            }
            if (observed.direct_close_candidate.has_value()) {
                ++summary.enemy_setup_draws_with_direct_close_candidate;
            }
            if (observed.final_instr_param_0x6.has_value()) {
                ++summary.enemy_setup_draws_with_final_instr_param;
                observed.expected_worker_pc =
                    expected_enemy_worker_pc_for_final_param(*observed.final_instr_param_0x6);
            }
            if (observed.helper_8008a174_result.has_value()) {
                ++summary.enemy_setup_draws_with_helper_8008a174_result;
            }
            if (observed.helper_8008a280_reached.has_value()) {
                ++summary.enemy_setup_draws_with_helper_8008a280_marker;
            }
            if (observed.helper_80082340_result.has_value()) {
                ++summary.enemy_setup_draws_with_helper_80082340_result;
            }
            if (observed.target_adjacent.has_value()) {
                ++summary.enemy_setup_draws_with_target_adjacency;
            }
            if (observed.target_distance.has_value()) {
                ++summary.enemy_setup_draws_with_target_distance;
            }
            if (observed.selected_worker_pc.has_value()) {
                ++summary.enemy_setup_draws_with_selected_worker;
            }
            if (observed.expected_worker_pc.has_value() && observed.selected_worker_pc.has_value()) {
                if (*observed.expected_worker_pc == *observed.selected_worker_pc) {
                    ++summary.worker_matches;
                } else {
                    ++summary.worker_mismatches;
                }
            }
            if (has_required_enemy_helper_fields(observed)) {
                ++summary.enemy_setup_draws_with_required_helper_fields;
            }
            summary.events.push_back(std::move(observed));
            continue;
        }

        if (is_pc_handler_checkpoint(event) || is_enemy_handler_checkpoint(event)) {
            const bool pc_handler = is_pc_handler_checkpoint(event);
            auto observed = make_common_event(
                event,
                pc_handler ? ActionSetupCheckpointKind::PcHandlerEntry
                           : ActionSetupCheckpointKind::EnemyHandlerEntry);
            observed.handler_pc = pc_handler ? kPcHandlerPc : kEnemyHandlerPc;
            if (pc_handler) {
                ++summary.observed_pc_handler_entries;
                maybe_set_first(summary.first_pc_handler_draw_index, observed.draw_index);
            } else {
                ++summary.observed_enemy_handler_entries;
                maybe_set_first(summary.first_enemy_handler_draw_index, observed.draw_index);
            }
            if (observed.instruction.has_value()) {
                ++summary.handler_entries_with_instruction;
            }
            if (observed.target_slot.has_value()) {
                ++summary.handler_entries_with_target_slot;
            }
            if (observed.instr_param_0x6.has_value()) {
                ++summary.handler_entries_with_instr_param;
            }
            if (observed.movement_flags.has_value()) {
                ++summary.handler_entries_with_movement_flags;
            }
            update_handler_match_counts(summary, observed);
            compare_setup_to_handler(summary, setup_events, observed, pc_handler);
            summary.events.push_back(std::move(observed));
            continue;
        }
    }

    if (summary.first_attack_hit_draw_index.has_value()) {
        for (const auto& event : summary.events) {
            if (event.kind == ActionSetupCheckpointKind::EnemySetupDraw
                && event.draw_index.has_value()
                && *event.draw_index < *summary.first_attack_hit_draw_index) {
                ++summary.enemy_setup_draws_before_first_attack_hit;
            }
        }
    }

    summary.status = classify_status(summary);
    return summary;
}

const char* action_setup_checkpoint_status_name(ActionSetupCheckpointStatus status) {
    switch (status) {
    case ActionSetupCheckpointStatus::ObservedOnly: return "ObservedOnly";
    case ActionSetupCheckpointStatus::MatchesExpected: return "MatchesExpected";
    case ActionSetupCheckpointStatus::MissingLiveSetupFields: return "MissingLiveSetupFields";
    case ActionSetupCheckpointStatus::HandlerMismatch: return "HandlerMismatch";
    case ActionSetupCheckpointStatus::SetupHandlerFieldMismatch: return "SetupHandlerFieldMismatch";
    case ActionSetupCheckpointStatus::MissingEnemySetupDraws: return "MissingEnemySetupDraws";
    case ActionSetupCheckpointStatus::ExtraEnemySetupDraws: return "ExtraEnemySetupDraws";
    case ActionSetupCheckpointStatus::MissingEnemySetupHelperFields: return "MissingEnemySetupHelperFields";
    case ActionSetupCheckpointStatus::WorkerMismatch: return "WorkerMismatch";
    default: return "Unknown";
    }
}

const char* action_setup_checkpoint_kind_name(ActionSetupCheckpointKind kind) {
    switch (kind) {
    case ActionSetupCheckpointKind::SetupAction: return "SetupAction";
    case ActionSetupCheckpointKind::PcHandlerEntry: return "PcHandlerEntry";
    case ActionSetupCheckpointKind::EnemyHandlerEntry: return "EnemyHandlerEntry";
    case ActionSetupCheckpointKind::EnemySetupDraw: return "EnemySetupDraw";
    default: return "Unknown";
    }
}

const char* first_battle_action_setup_checkpoint_rule_detail() {
    return "first-battle setupAction should route slots 0-3 to HandlePCInst 80086c68 and enemy slots to HandleECInst 8008b9e0; handler entries should preserve setupAction instruction, target, and instrParam_0x6 fields; Soldier attacks that reach HandleECInst should expose queued instruction fields, the 8008bc68 enemy-only setup draw, helper results that determine final instrParam_0x6, and worker 80087f6c/80087844 selection before the shared attack-result draw";
}

} // namespace savor::predict
