#include "CheckpointTrace.h"

#include "ActionViewCameraModel.h"
#include "ActionSetupCheckpointModel.h"
#include "ActionSourceCheckpointModel.h"
#include "ActionViewGateCheckpointModel.h"
#include "AttackDamageValueCheckpointModel.h"
#include "AttackResolutionCheckpointModel.h"
#include "CritGateCheckpointModel.h"
#include "CounterCheckpointModel.h"
#include "DropCheckpointModel.h"
#include "DeathDropCheckpointModel.h"
#include "OutcomeCheckpointModel.h"
#include "PreAiCheckpointModel.h"
#include "TurnOrderCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace savor::predict {

namespace {

std::string trim(std::string value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) {
        return {};
    }
    return std::string(first, last);
}

std::string to_lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::string normalize_pc(std::string value) {
    value = trim(std::move(value));
    if (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) {
        value = value.substr(2);
    }
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return value;
}

bool parse_int_value(const std::string& value, int& out) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_u32_value(const std::string& value, unsigned int& out) {
    char* end = nullptr;
    const int base = (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0) ? 16 : 10;
    const unsigned long parsed = std::strtoul(value.c_str(), &end, base);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<unsigned int>(parsed);
    return true;
}

bool parse_bool_value(const std::string& value, bool& out) {
    const auto lowered = to_lower(value);
    if (lowered == "1" || lowered == "true" || lowered == "yes") {
        out = true;
        return true;
    }
    if (lowered == "0" || lowered == "false" || lowered == "no") {
        out = false;
        return true;
    }
    return false;
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const char c : value) {
        switch (c) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            out << c;
            break;
        }
    }
    return out.str();
}

std::string seed_hex(unsigned int seed) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << seed;
    return out.str();
}

void write_optional_int(std::ostream& out, const std::optional<int>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "unknown";
    }
}

void write_optional_bool(std::ostream& out, const std::optional<bool>& value) {
    if (value.has_value()) {
        out << (*value ? "true" : "false");
    } else {
        out << "unknown";
    }
}

void write_optional_string(std::ostream& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "unknown";
    }
}

void write_json_optional_int(std::ostream& out, const std::optional<int>& value) {
    if (value.has_value()) {
        out << *value;
    } else {
        out << "null";
    }
}

void write_json_optional_bool(std::ostream& out, const std::optional<bool>& value) {
    if (value.has_value()) {
        out << (*value ? "true" : "false");
    } else {
        out << "null";
    }
}

void write_json_optional_string(std::ostream& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        out << "\"" << json_escape(*value) << "\"";
    } else {
        out << "null";
    }
}

bool token_is_comment(const std::string& token) {
    return token.rfind("#", 0) == 0;
}

void parse_event_field(CheckpointEvent& event, const std::string& token, CheckpointParseResult& result) {
    const auto equals = token.find('=');
    if (equals == std::string::npos || equals == 0) {
        result.errors.push_back("line " + std::to_string(event.line_number) + ": expected key=value token, got '" + token + "'");
        return;
    }

    const auto key = token.substr(0, equals);
    const auto value = token.substr(equals + 1);
    event.fields[key] = value;
}

void finalize_event(CheckpointEvent& event, CheckpointParseResult& result) {
    auto require_string = [&](const char* key, std::string& target) {
        const auto found = event.fields.find(key);
        if (found == event.fields.end() || found->second.empty()) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": missing required field '" + key + "'");
            return;
        }
        target = found->second;
    };

    require_string("pc", event.pc);
    event.pc = normalize_pc(event.pc);
    require_string("function", event.function);
    require_string("checkpoint", event.checkpoint);

    const auto draw = event.fields.find("rng_draw_index_before");
    if (draw == event.fields.end()) {
        result.errors.push_back("line " + std::to_string(event.line_number) + ": missing required field 'rng_draw_index_before'");
    } else {
        int parsed = 0;
        if (!parse_int_value(draw->second, parsed) || parsed < 0) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": rng_draw_index_before must be a non-negative integer");
        } else {
            event.rng_draw_index_before = parsed;
        }
    }

    const auto seed_before = event.fields.find("rng_seed_before");
    if (seed_before != event.fields.end()) {
        unsigned int parsed = 0;
        if (!parse_u32_value(seed_before->second, parsed)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": rng_seed_before must be decimal or 0x hex");
        } else {
            event.rng_seed_before = parsed;
        }
    }

    const auto seed_after = event.fields.find("rng_seed_after");
    if (seed_after != event.fields.end()) {
        unsigned int parsed = 0;
        if (!parse_u32_value(seed_after->second, parsed)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": rng_seed_after must be decimal or 0x hex");
        } else {
            event.rng_seed_after = parsed;
        }
    }

    const auto active = event.fields.find("active_slot");
    if (active != event.fields.end()) {
        int parsed = 0;
        if (!parse_int_value(active->second, parsed)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": active_slot must be an integer");
        } else {
            event.active_slot = parsed;
        }
    }

    const auto target = event.fields.find("target_slot");
    if (target != event.fields.end()) {
        int parsed = 0;
        if (!parse_int_value(target->second, parsed)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": target_slot must be an integer");
        } else {
            event.target_slot = parsed;
        }
    }

    const auto known_owner = known_rng_callsite_owners().find(event.pc);
    if (known_owner != known_rng_callsite_owners().end()) {
        event.known_rng_owner = known_owner->second;
        event.owns_rng_draw = true;
    }

    const auto owns_draw = event.fields.find("owns_rng_draw");
    if (owns_draw != event.fields.end()) {
        bool parsed = false;
        if (!parse_bool_value(owns_draw->second, parsed)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": owns_rng_draw must be true/false");
        } else {
            event.owns_rng_draw = parsed;
        }
    }

    if (event.owns_rng_draw && !event.rng_seed_before.has_value()) {
        result.warnings.push_back("line " + std::to_string(event.line_number) + ": RNG draw checkpoint has no rng_seed_before");
    }
    if (event.owns_rng_draw && !event.rng_seed_after.has_value()) {
        result.warnings.push_back("line " + std::to_string(event.line_number) + ": RNG draw checkpoint has no rng_seed_after");
    }
}

void add_ordering_warnings(CheckpointParseResult& result) {
    std::optional<int> previous_draw;
    int previous_line = 0;
    for (const auto& event : result.events) {
        if (!event.rng_draw_index_before.has_value()) {
            continue;
        }
        if (previous_draw.has_value() && *event.rng_draw_index_before < *previous_draw) {
            result.errors.push_back(
                "line " + std::to_string(event.line_number)
                + ": rng_draw_index_before regressed from line "
                + std::to_string(previous_line));
        }
        previous_draw = event.rng_draw_index_before;
        previous_line = event.line_number;
    }
}

std::map<std::string, int> count_known_rng_owners(const std::vector<CheckpointEvent>& events) {
    std::map<std::string, int> counts;
    for (const auto& event : events) {
        if (!event.known_rng_owner.empty()) {
            ++counts[event.known_rng_owner];
        }
    }
    return counts;
}

void write_text_report(
    const TraceCheckpointsOptions& options,
    const CheckpointParseResult& result,
    std::ostream& out) {
    out << "SavorPredict trace-checkpoints\n";
    out << "  checkpoint_file: " << options.checkpoint_file.string() << "\n";
    if (options.turn_job_id.has_value()) {
        out << "  turn_job_id: " << *options.turn_job_id << "\n";
    }
    if (options.exec_job_id.has_value()) {
        out << "  exec_job_id: " << *options.exec_job_id << "\n";
    }
    if (options.expected_fake_attacks.has_value()) {
        out << "  expected_fake_attacks: " << *options.expected_fake_attacks << "\n";
    }
    if (options.expected_enemy_setup_draws.has_value()) {
        out << "  expected_enemy_setup_draws: " << *options.expected_enemy_setup_draws << "\n";
    }
    if (options.expected_mode0e_camera_draws.has_value()) {
        out << "  expected_mode0e_camera_draws: " << *options.expected_mode0e_camera_draws << "\n";
    }
    if (options.expected_turn_order_draws.has_value()) {
        out << "  expected_turn_order_draws: " << *options.expected_turn_order_draws << "\n";
    }
    if (options.expected_attack_events.has_value()) {
        out << "  expected_attack_events: " << *options.expected_attack_events << "\n";
    }
    if (options.expected_crit_draws.has_value()) {
        out << "  expected_crit_draws: " << *options.expected_crit_draws << "\n";
    }
    if (options.expected_counter_roll_ceiling.has_value()) {
        out << "  expected_counter_roll_ceiling: " << *options.expected_counter_roll_ceiling << "\n";
    }
    if (options.expected_drop_rolls.has_value()) {
        out << "  expected_drop_rolls: " << *options.expected_drop_rolls << "\n";
    }
    if (options.expected_end_turn_status_draws.has_value()) {
        out << "  expected_end_turn_status_draws: "
            << *options.expected_end_turn_status_draws << "\n";
    }
    if (options.expected_level_up_stat_rolls.has_value()) {
        out << "  expected_level_up_stat_rolls: "
            << *options.expected_level_up_stat_rolls << "\n";
    }
    out << "  db_root: " << options.db_root.string() << "\n";
    out << "  events: " << result.events.size() << "\n";

    int rng_draw_events = 0;
    for (const auto& event : result.events) {
        if (event.owns_rng_draw) {
            ++rng_draw_events;
        }
    }
    out << "  rng_draw_events: " << rng_draw_events << "\n";
    out << "  errors: " << result.errors.size() << "\n";
    out << "  warnings: " << result.warnings.size() << "\n";

    out << "\nKnown RNG owners observed\n";
    const auto counts = count_known_rng_owners(result.events);
    if (counts.empty()) {
        out << "  none\n";
    } else {
        for (const auto& [owner, count] : counts) {
            out << "  " << owner << ": " << count << "\n";
        }
    }

    if (!result.errors.empty()) {
        out << "\nErrors\n";
        for (const auto& error : result.errors) {
            out << "  " << error << "\n";
        }
    }

    if (!result.warnings.empty()) {
        out << "\nWarnings\n";
        for (const auto& warning : result.warnings) {
            out << "  " << warning << "\n";
        }
    }

    const auto pre_ai = summarize_pre_ai_checkpoints(result.events, options.expected_fake_attacks);
    out << "\nPre-AI fake/camera checkpoints\n";
    out << "  status: " << pre_ai_checkpoint_status_name(pre_ai.status) << "\n";
    out << "  rule: " << first_battle_pre_ai_checkpoint_rule_detail() << "\n";
    out << "  expected_fake_attack_attempts: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_fake_attack_attempts << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_fake_attack_draws: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_fake_attack_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_battle_start_camera_draws: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_battle_start_camera_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_targeting_camera_draws: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_targeting_camera_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_total_pre_ai_draws: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_total_pre_ai_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_first_soldier_ai_draw_index_before: ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_first_soldier_ai_draw_index_before << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_battle_start_camera_draws: "
        << pre_ai.observed_battle_start_camera_draws << "\n";
    out << "  observed_targeting_camera_draws: "
        << pre_ai.observed_targeting_camera_draws << "\n";
    out << "  observed_fake_attack_attempts: "
        << pre_ai.observed_fake_attack_attempts << "\n";
    out << "  observed_fake_attack_draws: "
        << pre_ai.observed_fake_attack_draws << "\n";
    out << "  observed_skipped_fake_attack_draws: "
        << pre_ai.observed_skipped_fake_attack_draws << "\n";
    out << "  fake_attack_draws_with_frame_gap: "
        << pre_ai.fake_attack_draws_with_frame_gap << "\n";
    out << "  skipped_fake_attempts_with_frame_gap: "
        << pre_ai.skipped_fake_attempts_with_frame_gap << "\n";
    out << "  min_fake_attack_draw_camera_frame_gap: ";
    write_optional_int(out, pre_ai.min_fake_attack_draw_camera_frame_gap);
    out << "\n";
    out << "  max_fake_attack_draw_camera_frame_gap: ";
    write_optional_int(out, pre_ai.max_fake_attack_draw_camera_frame_gap);
    out << "\n";
    out << "  min_skipped_fake_attempt_camera_frame_gap: ";
    write_optional_int(out, pre_ai.min_skipped_fake_attempt_camera_frame_gap);
    out << "\n";
    out << "  max_skipped_fake_attempt_camera_frame_gap: ";
    write_optional_int(out, pre_ai.max_skipped_fake_attempt_camera_frame_gap);
    out << "\n";
    out << "  observed_pre_ai_draws: " << pre_ai.observed_pre_ai_draws << "\n";
    out << "  observed_first_soldier_ai_draws: "
        << pre_ai.observed_first_soldier_ai_draws << "\n";
    out << "  targeting_draws_with_target_slot: "
        << pre_ai.targeting_draws_with_target_slot << "\n";
    out << "  fake_draws_with_fake_attack_index: "
        << pre_ai.fake_draws_with_fake_attack_index << "\n";
    out << "  first_battle_start_camera_draw_index: ";
    if (pre_ai.first_battle_start_camera_draw_index.has_value()) {
        out << *pre_ai.first_battle_start_camera_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_targeting_camera_draw_index: ";
    if (pre_ai.first_targeting_camera_draw_index.has_value()) {
        out << *pre_ai.first_targeting_camera_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  last_targeting_camera_draw_index: ";
    if (pre_ai.last_targeting_camera_draw_index.has_value()) {
        out << *pre_ai.last_targeting_camera_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_fake_attack_draw_index: ";
    if (pre_ai.first_fake_attack_draw_index.has_value()) {
        out << *pre_ai.first_fake_attack_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  last_fake_attack_draw_index: ";
    if (pre_ai.last_fake_attack_draw_index.has_value()) {
        out << *pre_ai.last_fake_attack_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_soldier_ai_draw_index: ";
    if (pre_ai.first_soldier_ai_draw_index.has_value()) {
        out << *pre_ai.first_soldier_ai_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  pre_ai_draws_before_first_soldier_ai: "
        << pre_ai.pre_ai_draws_before_first_soldier_ai << "\n";
    if (!pre_ai.draws.empty()) {
        out << "  draws:\n";
        for (const auto& draw : pre_ai.draws) {
            out << "    owner=" << draw.owner;
            out << " draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " active_slot=";
            if (draw.active_slot.has_value()) {
                out << *draw.active_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (draw.target_slot.has_value()) {
                out << *draw.target_slot;
            } else {
                out << "unknown";
            }
            out << " fake_attack_index=";
            if (draw.fake_attack_index.has_value()) {
                out << *draw.fake_attack_index;
            } else {
                out << "unknown";
            }
            out << " camera_frame_gap=";
            if (draw.camera_frame_gap.has_value()) {
                out << *draw.camera_frame_gap;
            } else {
                out << "unknown";
            }
            out << " skipped_rng_draw=" << (draw.skipped_rng_draw ? "true" : "false") << "\n";
        }
    }

    const auto action_setup =
        summarize_action_setup_checkpoints(result.events, options.expected_enemy_setup_draws);
    out << "\nAction setup checkpoints\n";
    out << "  status: " << action_setup_checkpoint_status_name(action_setup.status) << "\n";
    out << "  rule: " << first_battle_action_setup_checkpoint_rule_detail() << "\n";
    out << "  expected_enemy_setup_draws: ";
    if (action_setup.expected_enemy_setup_draws.has_value()) {
        out << *action_setup.expected_enemy_setup_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_setup_action_events: "
        << action_setup.observed_setup_action_events << "\n";
    out << "  observed_pc_handler_entries: "
        << action_setup.observed_pc_handler_entries << "\n";
    out << "  observed_enemy_handler_entries: "
        << action_setup.observed_enemy_handler_entries << "\n";
    out << "  observed_enemy_setup_draws: "
        << action_setup.observed_enemy_setup_draws << "\n";
    out << "  setup_events_with_actor_slot: "
        << action_setup.setup_events_with_actor_slot << "\n";
    out << "  setup_events_with_handler_pc: "
        << action_setup.setup_events_with_handler_pc << "\n";
    out << "  setup_events_with_instruction: "
        << action_setup.setup_events_with_instruction << "\n";
    out << "  setup_events_with_target_slot: "
        << action_setup.setup_events_with_target_slot << "\n";
    out << "  setup_events_with_instr_param: "
        << action_setup.setup_events_with_instr_param << "\n";
    out << "  handler_entries_with_instruction: "
        << action_setup.handler_entries_with_instruction << "\n";
    out << "  handler_entries_with_target_slot: "
        << action_setup.handler_entries_with_target_slot << "\n";
    out << "  handler_entries_with_instr_param: "
        << action_setup.handler_entries_with_instr_param << "\n";
    out << "  handler_entries_with_movement_flags: "
        << action_setup.handler_entries_with_movement_flags << "\n";
    out << "  handler_matches: " << action_setup.handler_matches << "\n";
    out << "  handler_mismatches: " << action_setup.handler_mismatches << "\n";
    out << "  setup_handler_field_comparisons: "
        << action_setup.setup_handler_field_comparisons << "\n";
    out << "  setup_handler_field_matches: "
        << action_setup.setup_handler_field_matches << "\n";
    out << "  setup_handler_field_mismatches: "
        << action_setup.setup_handler_field_mismatches << "\n";
    out << "  pc_setup_handler_field_comparisons: "
        << action_setup.pc_setup_handler_field_comparisons << "\n";
    out << "  pc_setup_handler_field_matches: "
        << action_setup.pc_setup_handler_field_matches << "\n";
    out << "  pc_setup_handler_field_mismatches: "
        << action_setup.pc_setup_handler_field_mismatches << "\n";
    out << "  enemy_setup_handler_field_comparisons: "
        << action_setup.enemy_setup_handler_field_comparisons << "\n";
    out << "  enemy_setup_handler_field_matches: "
        << action_setup.enemy_setup_handler_field_matches << "\n";
    out << "  enemy_setup_handler_field_mismatches: "
        << action_setup.enemy_setup_handler_field_mismatches << "\n";
    out << "  enemy_setup_draws_with_gate_inputs: "
        << action_setup.enemy_setup_draws_with_gate_inputs << "\n";
    out << "  enemy_setup_draws_with_rand_value: "
        << action_setup.enemy_setup_draws_with_rand_value << "\n";
    out << "  enemy_setup_draws_with_rand_mod10: "
        << action_setup.enemy_setup_draws_with_rand_mod10 << "\n";
    out << "  enemy_setup_draws_with_direct_close_candidate: "
        << action_setup.enemy_setup_draws_with_direct_close_candidate << "\n";
    out << "  enemy_setup_draws_with_final_instr_param: "
        << action_setup.enemy_setup_draws_with_final_instr_param << "\n";
    out << "  enemy_setup_draws_with_helper_8008a174_result: "
        << action_setup.enemy_setup_draws_with_helper_8008a174_result << "\n";
    out << "  enemy_setup_draws_with_helper_8008a280_marker: "
        << action_setup.enemy_setup_draws_with_helper_8008a280_marker << "\n";
    out << "  enemy_setup_draws_with_helper_80082340_result: "
        << action_setup.enemy_setup_draws_with_helper_80082340_result << "\n";
    out << "  enemy_setup_draws_with_target_adjacency: "
        << action_setup.enemy_setup_draws_with_target_adjacency << "\n";
    out << "  enemy_setup_draws_with_target_distance: "
        << action_setup.enemy_setup_draws_with_target_distance << "\n";
    out << "  enemy_setup_draws_with_selected_worker: "
        << action_setup.enemy_setup_draws_with_selected_worker << "\n";
    out << "  enemy_setup_draws_with_required_helper_fields: "
        << action_setup.enemy_setup_draws_with_required_helper_fields << "\n";
    out << "  worker_matches: " << action_setup.worker_matches << "\n";
    out << "  worker_mismatches: " << action_setup.worker_mismatches << "\n";
    out << "  first_setup_action_draw_index: ";
    if (action_setup.first_setup_action_draw_index.has_value()) {
        out << *action_setup.first_setup_action_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_pc_handler_draw_index: ";
    if (action_setup.first_pc_handler_draw_index.has_value()) {
        out << *action_setup.first_pc_handler_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_enemy_handler_draw_index: ";
    if (action_setup.first_enemy_handler_draw_index.has_value()) {
        out << *action_setup.first_enemy_handler_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_enemy_setup_draw_index: ";
    if (action_setup.first_enemy_setup_draw_index.has_value()) {
        out << *action_setup.first_enemy_setup_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_attack_hit_draw_index: ";
    if (action_setup.first_attack_hit_draw_index.has_value()) {
        out << *action_setup.first_attack_hit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  enemy_setup_draws_before_first_attack_hit: "
        << action_setup.enemy_setup_draws_before_first_attack_hit << "\n";
    if (!action_setup.events.empty()) {
        out << "  events:\n";
        for (const auto& event : action_setup.events) {
            out << "    kind=" << action_setup_checkpoint_kind_name(event.kind);
            out << " draw_index=";
            if (event.draw_index.has_value()) {
                out << *event.draw_index;
            } else {
                out << "unknown";
            }
            out << " actor_slot=";
            if (event.actor_slot.has_value()) {
                out << *event.actor_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (event.target_slot.has_value()) {
                out << *event.target_slot;
            } else {
                out << "unknown";
            }
            out << " instruction=";
            if (event.instruction.has_value()) {
                out << *event.instruction;
            } else {
                out << "unknown";
            }
            out << " instr_param_0x6=";
            if (event.instr_param_0x6.has_value()) {
                out << *event.instr_param_0x6;
            } else {
                out << "unknown";
            }
            out << " movement_flags=";
            if (event.movement_flags.has_value()) {
                out << *event.movement_flags;
            } else {
                out << "unknown";
            }
            out << " expected_handler_pc=";
            if (event.expected_handler_pc.has_value()) {
                out << *event.expected_handler_pc;
            } else {
                out << "unknown";
            }
            out << " handler_pc=";
            if (event.handler_pc.has_value()) {
                out << *event.handler_pc;
            } else {
                out << "unknown";
            }
            out << " matched_setup_draw_index=";
            write_optional_int(out, event.matched_setup_draw_index);
            out << " instruction_matches_setup=";
            write_optional_bool(out, event.instruction_matches_setup);
            out << " target_slot_matches_setup=";
            write_optional_bool(out, event.target_slot_matches_setup);
            out << " instr_param_matches_setup=";
            write_optional_bool(out, event.instr_param_matches_setup);
            out << " setup_rand=";
            if (event.setup_rand.has_value()) {
                out << *event.setup_rand;
            } else {
                out << "unknown";
            }
            out << " setup_rand_mod10=";
            if (event.setup_rand_mod10.has_value()) {
                out << *event.setup_rand_mod10;
            } else {
                out << "unknown";
            }
            out << " direct_close_candidate=";
            if (event.direct_close_candidate.has_value()) {
                out << *event.direct_close_candidate;
            } else {
                out << "unknown";
            }
            out << " final_instr_param_0x6=";
            write_optional_int(out, event.final_instr_param_0x6);
            out << " helper_8008a174_result=";
            write_optional_int(out, event.helper_8008a174_result);
            out << " helper_8008a280_reached=";
            write_optional_bool(out, event.helper_8008a280_reached);
            out << " helper_80082340_result=";
            write_optional_int(out, event.helper_80082340_result);
            out << " target_adjacent=";
            write_optional_int(out, event.target_adjacent);
            out << " target_distance=";
            write_optional_int(out, event.target_distance);
            out << " expected_worker_pc=";
            write_optional_string(out, event.expected_worker_pc);
            out << " selected_worker_pc=";
            write_optional_string(out, event.selected_worker_pc);
            out << "\n";
        }
    }

    const auto action_source_expectation = first_battle_action_source_checkpoint_expectation();
    const auto action_source = summarize_action_source_checkpoints(
        result.events,
        action_source_expectation.expected_handler_pc);
    out << "\nAction-source field6 checkpoints\n";
    out << "  status: " << action_source_checkpoint_status_name(action_source.status) << "\n";
    out << "  rule: " << first_battle_action_source_checkpoint_rule_detail() << "\n";
    out << "  expected_handler_pc: ";
    if (action_source.expected_handler_pc.has_value()) {
        out << *action_source.expected_handler_pc << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_callback_pc: ";
    if (action_source.expected_callback_pc.has_value()) {
        out << *action_source.expected_callback_pc << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_source_selection_events: "
        << action_source.observed_source_selection_events << "\n";
    out << "  observed_action_source_events: "
        << action_source.observed_action_source_events << "\n";
    out << "  source_selection_events_with_source_slot: "
        << action_source.source_selection_events_with_source_slot << "\n";
    out << "  source_selection_events_with_actor_slot: "
        << action_source.source_selection_events_with_actor_slot << "\n";
    out << "  source_selection_events_with_target_slot: "
        << action_source.source_selection_events_with_target_slot << "\n";
    out << "  events_with_actor_slot: " << action_source.events_with_actor_slot << "\n";
    out << "  events_with_source_slot: " << action_source.events_with_source_slot << "\n";
    out << "  events_with_action_id: " << action_source.events_with_action_id << "\n";
    out << "  events_with_handler_pc: " << action_source.events_with_handler_pc << "\n";
    out << "  events_with_callback_pc: " << action_source.events_with_callback_pc << "\n";
    out << "  events_with_source_field6: " << action_source.events_with_source_field6 << "\n";
    out << "  events_with_actor_field6: " << action_source.events_with_actor_field6 << "\n";
    out << "  source_selection_bridge_pairs: "
        << action_source.source_selection_bridge_pairs << "\n";
    out << "  source_selection_bridge_matches: "
        << action_source.source_selection_bridge_matches << "\n";
    out << "  source_selection_bridge_mismatches: "
        << action_source.source_selection_bridge_mismatches << "\n";
    out << "  field6_matches: " << action_source.field6_matches << "\n";
    out << "  field6_mismatches: " << action_source.field6_mismatches << "\n";
    out << "  handler_matches: " << action_source.handler_matches << "\n";
    out << "  handler_mismatches: " << action_source.handler_mismatches << "\n";
    out << "  callback_matches: " << action_source.callback_matches << "\n";
    out << "  callback_mismatches: " << action_source.callback_mismatches << "\n";
    out << "  first_source_selection_draw_index: ";
    if (action_source.first_source_selection_draw_index.has_value()) {
        out << *action_source.first_source_selection_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_action_source_draw_index: ";
    if (action_source.first_action_source_draw_index.has_value()) {
        out << *action_source.first_action_source_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    if (!action_source.events.empty()) {
        out << "  events:\n";
        for (const auto& event : action_source.events) {
            out << "    kind=" << action_source_checkpoint_kind_name(event.kind);
            out << " draw_index=";
            if (event.draw_index.has_value()) {
                out << *event.draw_index;
            } else {
                out << "unknown";
            }
            out << " actor_slot=";
            if (event.actor_slot.has_value()) {
                out << *event.actor_slot;
            } else {
                out << "unknown";
            }
            out << " source_slot=";
            if (event.source_slot.has_value()) {
                out << *event.source_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (event.target_slot.has_value()) {
                out << *event.target_slot;
            } else {
                out << "unknown";
            }
            out << " action_id=";
            if (event.action_id.has_value()) {
                out << *event.action_id;
            } else {
                out << "unknown";
            }
            out << " source_field6_0x6=";
            if (event.source_field6_0x6.has_value()) {
                out << *event.source_field6_0x6;
            } else {
                out << "unknown";
            }
            out << " actor_field6_0x6=";
            if (event.actor_field6_0x6.has_value()) {
                out << *event.actor_field6_0x6;
            } else {
                out << "unknown";
            }
            out << " handler_pc=";
            if (event.handler_pc.has_value()) {
                out << *event.handler_pc;
            } else {
                out << "unknown";
            }
            out << " callback_pc=";
            if (event.callback_pc.has_value()) {
                out << *event.callback_pc;
            } else {
                out << "unknown";
            }
            out << "\n";
        }
    }

    const auto action_view_gate = summarize_action_view_gate_checkpoints(result.events);
    out << "\nAction-view gate checkpoints\n";
    out << "  status: " << action_view_gate_checkpoint_status_name(action_view_gate.status) << "\n";
    out << "  rule: " << first_battle_action_view_gate_checkpoint_rule_detail() << "\n";
    out << "  observed_gate_events: " << action_view_gate.observed_gate_events << "\n";
    out << "  events_with_aux_list_root: " << action_view_gate.events_with_aux_list_root << "\n";
    out << "  events_with_query_args: " << action_view_gate.events_with_query_args << "\n";
    out << "  events_with_query_result: " << action_view_gate.events_with_query_result << "\n";
    out << "  events_with_selected_record_mode: "
        << action_view_gate.events_with_selected_record_mode << "\n";
    out << "  query_args_match: " << action_view_gate.query_args_match << "\n";
    out << "  query_args_mismatch: " << action_view_gate.query_args_mismatch << "\n";
    out << "  selected_mode_matches: " << action_view_gate.selected_mode_matches << "\n";
    out << "  selected_mode_mismatches: " << action_view_gate.selected_mode_mismatches << "\n";
    out << "  events_with_action_child_thread: "
        << action_view_gate.events_with_action_child_thread << "\n";
    out << "  events_with_child_payload: " << action_view_gate.events_with_child_payload << "\n";
    out << "  events_with_nested_payload: " << action_view_gate.events_with_nested_payload << "\n";
    out << "  events_with_child_thread_state: "
        << action_view_gate.events_with_child_thread_state << "\n";
    out << "  events_with_scheduler_chain: "
        << action_view_gate.events_with_scheduler_chain << "\n";
    out << "  events_with_mode0_fallback_flag: "
        << action_view_gate.events_with_mode0_fallback_flag << "\n";
    out << "  mode0_fallback_reached_events: "
        << action_view_gate.mode0_fallback_reached_events << "\n";
    out << "  observed_mode0e_camera_draws: "
        << action_view_gate.observed_mode0e_camera_draws << "\n";
    out << "  observed_mode0_fallback_draws: "
        << action_view_gate.observed_mode0_fallback_draws << "\n";
    out << "  observed_attack_hit_draws: "
        << action_view_gate.observed_attack_hit_draws << "\n";
    out << "  first_gate_draw_index: ";
    if (action_view_gate.first_gate_draw_index.has_value()) {
        out << *action_view_gate.first_gate_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_mode0e_draw_index: ";
    if (action_view_gate.first_mode0e_draw_index.has_value()) {
        out << *action_view_gate.first_mode0e_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_mode0_fallback_draw_index: ";
    if (action_view_gate.first_mode0_fallback_draw_index.has_value()) {
        out << *action_view_gate.first_mode0_fallback_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_attack_hit_draw_index: ";
    if (action_view_gate.first_attack_hit_draw_index.has_value()) {
        out << *action_view_gate.first_attack_hit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  gate_events_before_first_mode0e: "
        << action_view_gate.gate_events_before_first_mode0e << "\n";
    out << "  gate_events_before_first_attack_hit: "
        << action_view_gate.gate_events_before_first_attack_hit << "\n";
    out << "  mode0e_draws_before_first_attack_hit: "
        << action_view_gate.mode0e_draws_before_first_attack_hit << "\n";
    out << "  mode0_fallback_draws_before_first_mode0e: "
        << action_view_gate.mode0_fallback_draws_before_first_mode0e << "\n";
    out << "  mode0_fallback_draws_before_first_attack_hit: "
        << action_view_gate.mode0_fallback_draws_before_first_attack_hit << "\n";
    if (!action_view_gate.events.empty()) {
        out << "  events:\n";
        for (const auto& event : action_view_gate.events) {
            out << "    draw_index=";
            if (event.draw_index.has_value()) {
                out << *event.draw_index;
            } else {
                out << "unknown";
            }
            out << " active_slot=";
            if (event.active_slot.has_value()) {
                out << *event.active_slot;
            } else {
                out << "unknown";
            }
            out << " source_slot=";
            if (event.source_slot.has_value()) {
                out << *event.source_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (event.target_slot.has_value()) {
                out << *event.target_slot;
            } else {
                out << "unknown";
            }
            out << " source_field6_0x6=";
            if (event.source_field6_0x6.has_value()) {
                out << *event.source_field6_0x6;
            } else {
                out << "unknown";
            }
            out << " actor_field6_0x6=";
            if (event.actor_field6_0x6.has_value()) {
                out << *event.actor_field6_0x6;
            } else {
                out << "unknown";
            }
            out << " aux_list_root=";
            if (event.aux_list_root.has_value()) {
                out << *event.aux_list_root;
            } else {
                out << "unknown";
            }
            out << " query=(";
            if (event.query_arg0.has_value()) {
                out << *event.query_arg0;
            } else {
                out << "unknown";
            }
            out << ",";
            if (event.query_arg1.has_value()) {
                out << *event.query_arg1;
            } else {
                out << "unknown";
            }
            out << ",";
            if (event.query_arg2.has_value()) {
                out << *event.query_arg2;
            } else {
                out << "unknown";
            }
            out << ",";
            if (event.query_arg3.has_value()) {
                out << *event.query_arg3;
            } else {
                out << "unknown";
            }
            out << ") query_result=";
            if (event.query_result.has_value()) {
                out << *event.query_result;
            } else {
                out << "unknown";
            }
            out << " selected_record_mode=";
            if (event.selected_record_mode.has_value()) {
                out << *event.selected_record_mode;
            } else {
                out << "unknown";
            }
            out << " action_child_thread=";
            write_optional_string(out, event.action_child_thread);
            out << " child_payload=";
            write_optional_string(out, event.child_payload);
            out << " nested_payload=";
            write_optional_string(out, event.nested_payload);
            out << " child_thread_state_byte=";
            write_optional_int(out, event.child_thread_state_byte);
            out << " mode0_fallback_reached=";
            write_optional_bool(out, event.mode0_fallback_reached);
            out << "\n";
        }
    }

    const auto action_view = summarize_action_view_camera_checkpoints(
        result.events,
        options.expected_mode0e_camera_draws);
    out << "\nAction-view camera checkpoints\n";
    out << "  status: " << action_view_camera_checkpoint_status_name(action_view.status) << "\n";
    out << "  expected_mode0e_camera_draws: ";
    if (action_view.expected_mode0e_camera_draws.has_value()) {
        out << *action_view.expected_mode0e_camera_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_mode0e_camera_draws: " << action_view.observed_mode0e_camera_draws << "\n";
    out << "  observed_mode0_fallback_draws: " << action_view.observed_mode0_fallback_draws << "\n";
    out << "  observed_attack_hit_draws: " << action_view.observed_attack_hit_draws << "\n";
    out << "  first_mode0e_draw_index: ";
    if (action_view.first_mode0e_draw_index.has_value()) {
        out << *action_view.first_mode0e_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_attack_hit_draw_index: ";
    if (action_view.first_attack_hit_draw_index.has_value()) {
        out << *action_view.first_attack_hit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  mode0e_draws_before_first_attack_hit: "
        << action_view.mode0e_draws_before_first_attack_hit << "\n";
    out << "  mode0e_draws_after_first_attack_hit: "
        << action_view.mode0e_draws_after_first_attack_hit << "\n";

    const auto turn_order = summarize_turn_order_checkpoints(
        result.events,
        options.expected_turn_order_draws);
    out << "\nTurn-order checkpoints\n";
    out << "  status: " << turn_order_checkpoint_status_name(turn_order.status) << "\n";
    out << "  expected_turn_order_draws: ";
    if (turn_order.expected_priority_jitter_draws.has_value()) {
        out << *turn_order.expected_priority_jitter_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_priority_jitter_draws: "
        << turn_order.observed_priority_jitter_draws << "\n";
    out << "  first_priority_jitter_draw_index: ";
    if (turn_order.first_priority_jitter_draw_index.has_value()) {
        out << *turn_order.first_priority_jitter_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  last_priority_jitter_draw_index: ";
    if (turn_order.last_priority_jitter_draw_index.has_value()) {
        out << *turn_order.last_priority_jitter_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  draws_with_slot: " << turn_order.draws_with_slot << "\n";
    out << "  draws_with_quick: " << turn_order.draws_with_quick << "\n";
    out << "  draws_with_assigned_priority: " << turn_order.draws_with_assigned_priority << "\n";
    out << "  draws_with_rand_value: " << turn_order.draws_with_rand_value << "\n";
    out << "  events_with_expected_first_battle_quick: "
        << turn_order.events_with_expected_first_battle_quick << "\n";
    out << "  quick_matches: " << turn_order.quick_matches << "\n";
    out << "  quick_mismatches: " << turn_order.quick_mismatches << "\n";
    out << "  events_with_fixed_priority_result: "
        << turn_order.events_with_fixed_priority_result << "\n";
    out << "  fixed_priority_zero_results: "
        << turn_order.fixed_priority_zero_results << "\n";
    out << "  fixed_priority_nonzero_results: "
        << turn_order.fixed_priority_nonzero_results << "\n";
    out << "  priority_sources_with_fixed_priority_result: "
        << turn_order.priority_sources_with_fixed_priority_result << "\n";
    out << "  priority_sources_missing_fixed_priority_result: "
        << turn_order.priority_sources_missing_fixed_priority_result << "\n";
    out << "  events_with_queue_metadata: " << turn_order.events_with_queue_metadata << "\n";
    out << "  queue_metadata_matches: " << turn_order.queue_metadata_matches << "\n";
    out << "  queue_metadata_mismatches: " << turn_order.queue_metadata_mismatches << "\n";
    out << "  observed_queue_entries: " << turn_order.observed_queue_entries << "\n";
    out << "  observed_execution_order_entries: "
        << turn_order.observed_execution_order_entries << "\n";
    out << "  queue_entries_with_slot: " << turn_order.queue_entries_with_slot << "\n";
    out << "  queue_entries_with_quick: " << turn_order.queue_entries_with_quick << "\n";
    out << "  queue_entries_with_assigned_priority: "
        << turn_order.queue_entries_with_assigned_priority << "\n";
    out << "  priority_draws_with_expected_priority: "
        << turn_order.priority_draws_with_expected_priority << "\n";
    out << "  priority_matches: " << turn_order.priority_matches << "\n";
    out << "  priority_mismatches: " << turn_order.priority_mismatches << "\n";
    out << "  incomplete_queue_entries: " << turn_order.incomplete_queue_entries << "\n";
    out << "  incomplete_execution_order_entries: "
        << turn_order.incomplete_execution_order_entries << "\n";
    out << "  execution_order_compared: "
        << (turn_order.execution_order_compared ? "true" : "false") << "\n";
    out << "  execution_order_exact: "
        << (turn_order.execution_order_exact ? "true" : "false") << "\n";
    out << "  priority_ties_observed: "
        << (turn_order.priority_ties_observed ? "true" : "false") << "\n";
    out << "  execution_order_matches: "
        << turn_order.execution_order_matches << "\n";
    out << "  execution_order_mismatches: "
        << turn_order.execution_order_mismatches << "\n";
    if (!turn_order.expected_execution_slots.empty()) {
        out << "  expected_execution_slots:";
        for (const auto slot : turn_order.expected_execution_slots) {
            out << " " << slot;
        }
        out << "\n";
    }
    if (!turn_order.observed_execution_slots.empty()) {
        out << "  observed_execution_slots:";
        for (const auto slot : turn_order.observed_execution_slots) {
            out << " " << slot;
        }
        out << "\n";
    }
    if (!turn_order.draws.empty()) {
        out << "  events:\n";
        for (const auto& draw : turn_order.draws) {
            out << "    draw_index=";
            write_optional_int(out, draw.draw_index);
            out << " kind=" << turn_order_checkpoint_kind_name(draw.kind);
            out << " slot=";
            write_optional_int(out, draw.slot);
            out << " quick=";
            write_optional_int(out, draw.quick);
            out << " queued_instruction=";
            write_optional_int(out, draw.queued_instruction);
            out << " target_slot=";
            write_optional_int(out, draw.target_slot);
            out << " initial_priority=";
            write_optional_int(out, draw.initial_priority);
            out << " fixed_priority_result=";
            write_optional_int(out, draw.fixed_priority_result);
            out << " fixed_priority_value=";
            write_optional_int(out, draw.fixed_priority_value);
            out << " queue_index=";
            write_optional_int(out, draw.queue_index);
            out << " execution_index=";
            write_optional_int(out, draw.execution_index);
            out << " jitter_modulus=";
            write_optional_int(out, draw.jitter_modulus);
            out << " sum_quick=";
            write_optional_int(out, draw.sum_quick);
            out << " queued_count=";
            write_optional_int(out, draw.queued_count);
            out << " assigned_priority=";
            write_optional_int(out, draw.assigned_priority);
            out << " expected_assigned_priority=";
            write_optional_int(out, draw.expected_assigned_priority);
            out << " expected_first_battle_quick=";
            write_optional_int(out, draw.expected_first_battle_quick);
            out << " expected_jitter_modulus=";
            write_optional_int(out, draw.expected_jitter_modulus);
            out << " rand_value=";
            write_optional_int(out, draw.rand_value);
            out << " priority_matches="
                << (draw.priority_matches ? "true" : "false");
            out << " quick_matches="
                << (draw.quick_matches ? "true" : "false");
            out << " queue_metadata_matches="
                << (draw.queue_metadata_matches ? "true" : "false");
            out << "\n";
        }
    }

    const auto attack_resolution = summarize_attack_resolution_checkpoints(
        result.events,
        options.expected_attack_events,
        options.expected_crit_draws);
    out << "\nAttack-result and damage checkpoints\n";
    out << "  status: " << attack_resolution_checkpoint_status_name(attack_resolution.status) << "\n";
    out << "  expected_attack_events: ";
    if (attack_resolution.expected_attack_events.has_value()) {
        out << *attack_resolution.expected_attack_events << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_crit_draws: ";
    if (attack_resolution.expected_crit_draws.has_value()) {
        out << *attack_resolution.expected_crit_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_hit_draws: " << attack_resolution.observed_hit_draws << "\n";
    out << "  observed_crit_draws: " << attack_resolution.observed_crit_draws << "\n";
    out << "  observed_damage_spread_draws: "
        << attack_resolution.observed_damage_spread_draws << "\n";
    out << "  observed_damage_bonus_draws: "
        << attack_resolution.observed_damage_bonus_draws << "\n";
    out << "  first_hit_draw_index: ";
    if (attack_resolution.first_hit_draw_index.has_value()) {
        out << *attack_resolution.first_hit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_damage_spread_draw_index: ";
    if (attack_resolution.first_damage_spread_draw_index.has_value()) {
        out << *attack_resolution.first_damage_spread_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  damage_pairs_in_order: " << attack_resolution.damage_pairs_in_order << "\n";
    out << "  damage_pairs_out_of_order: "
        << attack_resolution.damage_pairs_out_of_order << "\n";

    const auto attack_damage_values = summarize_attack_damage_value_checkpoints(result.events);
    out << "\nAttack damage value checkpoints\n";
    out << "  status: "
        << attack_damage_value_checkpoint_status_name(attack_damage_values.status) << "\n";
    out << "  rule: " << first_battle_attack_damage_value_checkpoint_rule_detail() << "\n";
    out << "  observed_attack_bursts: " << attack_damage_values.observed_attack_bursts << "\n";
    out << "  orphan_damage_draw_events: "
        << attack_damage_values.orphan_damage_draw_events << "\n";
    out << "  bursts_with_live_inputs: "
        << attack_damage_values.bursts_with_live_inputs << "\n";
    out << "  bursts_with_required_draws: "
        << attack_damage_values.bursts_with_required_draws << "\n";
    out << "  bursts_with_observed_attack_result: "
        << attack_damage_values.bursts_with_observed_attack_result << "\n";
    out << "  bursts_with_observed_damage: "
        << attack_damage_values.bursts_with_observed_damage << "\n";
    out << "  bursts_with_damage_apply: "
        << attack_damage_values.bursts_with_damage_apply << "\n";
    out << "  bursts_with_damage_apply_fields: "
        << attack_damage_values.bursts_with_damage_apply_fields << "\n";
    out << "  damage_apply_events_after_damage_draws: "
        << attack_damage_values.damage_apply_events_after_damage_draws << "\n";
    out << "  damage_apply_order_mismatches: "
        << attack_damage_values.damage_apply_order_mismatches << "\n";
    out << "  simulated_bursts: " << attack_damage_values.simulated_bursts << "\n";
    out << "  attack_result_matches: "
        << attack_damage_values.attack_result_matches << "\n";
    out << "  attack_result_mismatches: "
        << attack_damage_values.attack_result_mismatches << "\n";
    out << "  damage_matches: " << attack_damage_values.damage_matches << "\n";
    out << "  damage_mismatches: " << attack_damage_values.damage_mismatches << "\n";
    out << "  damage_apply_matches: "
        << attack_damage_values.damage_apply_matches << "\n";
    out << "  damage_apply_mismatches: "
        << attack_damage_values.damage_apply_mismatches << "\n";
    out << "  hp_after_matches: " << attack_damage_values.hp_after_matches << "\n";
    out << "  hp_after_mismatches: "
        << attack_damage_values.hp_after_mismatches << "\n";
    out << "  lethal_matches: " << attack_damage_values.lethal_matches << "\n";
    out << "  lethal_mismatches: "
        << attack_damage_values.lethal_mismatches << "\n";
    out << "  missing_live_field_bursts: "
        << attack_damage_values.missing_live_field_bursts << "\n";
    out << "  incomplete_draw_bursts: "
        << attack_damage_values.incomplete_draw_bursts << "\n";
    out << "  missing_damage_apply_field_bursts: "
        << attack_damage_values.missing_damage_apply_field_bursts << "\n";
    out << "  seed_transition_mismatches: "
        << attack_damage_values.seed_transition_mismatches << "\n";
    out << "  first_hit_draw_index: ";
    write_optional_int(out, attack_damage_values.first_hit_draw_index);
    out << "\n";
    out << "  first_damage_value_draw_index: ";
    write_optional_int(out, attack_damage_values.first_damage_value_draw_index);
    out << "\n";
    if (!attack_damage_values.attacks.empty()) {
        out << "  attacks:\n";
        for (const auto& attack : attack_damage_values.attacks) {
            out << "    attack_index=" << attack.attack_index;
            out << " active_slot=";
            write_optional_int(out, attack.active_slot);
            out << " target_slot=";
            write_optional_int(out, attack.target_slot);
            out << " hit_rand=";
            write_optional_int(out, attack.hit_rand);
            out << " crit_rand=";
            write_optional_int(out, attack.crit_rand);
            out << " spread_rand=";
            write_optional_int(out, attack.damage_spread_rand);
            out << " bonus_rand=";
            write_optional_int(out, attack.damage_bonus_rand);
            out << " damage_apply_draw_index=";
            write_optional_int(out, attack.damage_apply_draw_index);
            out << " expected_attack_result=";
            write_optional_int(out, attack.expected_attack_result);
            out << " observed_attack_result=";
            write_optional_int(out, attack.observed_attack_result);
            out << " expected_damage=";
            write_optional_int(out, attack.expected_damage);
            out << " observed_damage=";
            write_optional_int(out, attack.observed_damage);
            out << " damage_apply_damage=";
            write_optional_int(out, attack.damage_apply_damage);
            out << " hp_before=";
            write_optional_int(out, attack.hp_before);
            out << " hp_after=";
            write_optional_int(out, attack.hp_after);
            out << " expected_hp_after=";
            write_optional_int(out, attack.expected_hp_after);
            out << " lethal=";
            write_optional_int(out, attack.lethal);
            out << " expected_lethal=";
            write_optional_int(out, attack.expected_lethal);
            out << " missing_live_inputs=" << attack.missing_live_input_fields;
            out << " damage_apply_observed="
                << (attack.damage_apply_observed ? "true" : "false");
            out << " damage_apply_after_damage_draws="
                << (attack.damage_apply_after_damage_draws ? "true" : "false");
            out << " simulated=" << (attack.simulated ? "true" : "false") << "\n";
        }
    }

    const auto crit_gate = summarize_crit_gate_checkpoints(result.events);
    out << "\nCrit-gate checkpoints\n";
    out << "  status: " << crit_gate_checkpoint_status_name(crit_gate.status) << "\n";
    out << "  rule: " << crit_gate_checkpoint_rule_detail() << "\n";
    out << "  observed_hit_draws: " << crit_gate.observed_hit_draws << "\n";
    out << "  observed_crit_draws: " << crit_gate.observed_crit_draws << "\n";
    out << "  hit_draws_with_instr_param: "
        << crit_gate.hit_draws_with_instr_param << "\n";
    out << "  hit_draws_with_hit_success: "
        << crit_gate.hit_draws_with_hit_success << "\n";
    out << "  expected_crit_draws_from_live_gate: ";
    if (crit_gate.expected_crit_draws_from_live_gate.has_value()) {
        out << *crit_gate.expected_crit_draws_from_live_gate << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_hit_draw_index: ";
    if (crit_gate.first_hit_draw_index.has_value()) {
        out << *crit_gate.first_hit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_crit_draw_index: ";
    if (crit_gate.first_crit_draw_index.has_value()) {
        out << *crit_gate.first_crit_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    if (!crit_gate.hit_draws.empty()) {
        out << "  hit_draws:\n";
        for (const auto& draw : crit_gate.hit_draws) {
            out << "    draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " active_slot=";
            if (draw.active_slot.has_value()) {
                out << *draw.active_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (draw.target_slot.has_value()) {
                out << *draw.target_slot;
            } else {
                out << "unknown";
            }
            out << " instr_param_0x6=";
            if (draw.instr_param_0x6.has_value()) {
                out << *draw.instr_param_0x6;
            } else {
                out << "unknown";
            }
            out << " hit_success=";
            if (draw.hit_success.has_value()) {
                out << *draw.hit_success;
            } else {
                out << "unknown";
            }
            out << " attack_result=";
            if (draw.attack_result.has_value()) {
                out << *draw.attack_result;
            } else {
                out << "unknown";
            }
            out << " rand_value=";
            if (draw.rand_value.has_value()) {
                out << *draw.rand_value;
            } else {
                out << "unknown";
            }
            out << "\n";
        }
    }

    const auto counter = summarize_counter_checkpoints(
        result.events,
        options.expected_counter_roll_ceiling);
    out << "\nCounter checkpoints\n";
    out << "  status: " << counter_checkpoint_status_name(counter.status) << "\n";
    out << "  expected_counter_roll_ceiling: ";
    if (counter.expected_counter_roll_ceiling.has_value()) {
        out << *counter.expected_counter_roll_ceiling << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_counter_rolls: " << counter.observed_counter_rolls << "\n";
    out << "  first_counter_roll_draw_index: ";
    if (counter.first_counter_roll_draw_index.has_value()) {
        out << *counter.first_counter_roll_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  last_counter_roll_draw_index: ";
    if (counter.last_counter_roll_draw_index.has_value()) {
        out << *counter.last_counter_roll_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  draws_with_actor_slots: " << counter.draws_with_actor_slots << "\n";
    out << "  draws_with_gate_inputs: " << counter.draws_with_gate_inputs << "\n";
    out << "  draws_with_rand_value: " << counter.draws_with_rand_value << "\n";
    out << "  draws_with_counter_result: " << counter.draws_with_counter_result << "\n";
    out << "  draws_with_queue_result: " << counter.draws_with_queue_result << "\n";
    out << "  draws_with_counter_chance_update: "
        << counter.draws_with_counter_chance_update << "\n";
    out << "  live_gate_simulated_draws: " << counter.live_gate_simulated_draws << "\n";
    out << "  observed_counter_gate_attempts: "
        << counter.observed_counter_gate_attempts << "\n";
    out << "  gate_attempts_with_live_inputs: "
        << counter.gate_attempts_with_live_inputs << "\n";
    out << "  expected_counter_rolls_from_gate_inputs: "
        << counter.expected_counter_rolls_from_gate_inputs << "\n";
    out << "  expected_no_draw_gate_attempts: "
        << counter.expected_no_draw_gate_attempts << "\n";
    out << "  no_draw_gate_attempts_simulated: "
        << counter.no_draw_gate_attempts_simulated << "\n";
    out << "  observed_counter_follow_up_events: "
        << counter.observed_counter_follow_up_events << "\n";
    out << "  expected_counter_follow_up_events: "
        << counter.expected_counter_follow_up_events << "\n";
    out << "  counter_result_matches: " << counter.counter_result_matches << "\n";
    out << "  counter_result_mismatches: " << counter.counter_result_mismatches << "\n";
    out << "  queued_field_matches: " << counter.queued_field_matches << "\n";
    out << "  queued_field_mismatches: " << counter.queued_field_mismatches << "\n";
    out << "  counter_chance_update_matches: "
        << counter.counter_chance_update_matches << "\n";
    out << "  counter_chance_update_mismatches: "
        << counter.counter_chance_update_mismatches << "\n";
    out << "  seed_transition_mismatches: " << counter.seed_transition_mismatches << "\n";
    if (!counter.draws.empty()) {
        out << "  events:\n";
        for (const auto& draw : counter.draws) {
            out << "    draw_index=";
            write_optional_int(out, draw.draw_index);
            out << " kind=" << counter_checkpoint_kind_name(draw.kind);
            out << " attacker_slot=";
            write_optional_int(out, draw.attacker_slot);
            out << " target_slot=";
            write_optional_int(out, draw.target_slot);
            out << " cur_counter=";
            write_optional_int(out, draw.target_current_counter_chance);
            out << " crit=";
            write_optional_int(out, draw.attack_was_critical);
            out << " counter_rand=";
            write_optional_int(out, draw.counter_rand);
            out << " expected_counter_rolls=";
            write_optional_int(out, draw.expected_counter_rolls);
            out << " counter_result=";
            write_optional_int(out, draw.counter_result);
            out << " expected_counter_result=";
            write_optional_int(out, draw.expected_counter_result);
            out << " expected_counter_follow_up=";
            write_optional_int(out, draw.expected_counter_follow_up);
            out << " observed_counter_follow_up=";
            write_optional_int(out, draw.observed_counter_follow_up);
            out << " expected_reason=";
            if (draw.expected_reason.has_value()) {
                out << counter_result_reason_name(*draw.expected_reason);
            } else {
                out << "unknown";
            }
            out << "\n";
        }
    }

    const auto death_drop = summarize_death_drop_checkpoints(result.events);
    out << "\nDeath/drop flow checkpoints\n";
    out << "  status: " << death_drop_checkpoint_status_name(death_drop.status) << "\n";
    out << "  rule: " << first_battle_death_drop_checkpoint_rule_detail() << "\n";
    out << "  observed_damage_apply_events: "
        << death_drop.observed_damage_apply_events << "\n";
    out << "  observed_death_handler_events: "
        << death_drop.observed_death_handler_events << "\n";
    out << "  observed_drop_entry_events: "
        << death_drop.observed_drop_entry_events << "\n";
    out << "  observed_drop_rolls: " << death_drop.observed_drop_rolls << "\n";
    out << "  damage_events_with_live_death_fields: "
        << death_drop.damage_events_with_live_death_fields << "\n";
    out << "  lethal_damage_events: " << death_drop.lethal_damage_events << "\n";
    out << "  nonlethal_damage_events: " << death_drop.nonlethal_damage_events << "\n";
    out << "  damage_events_with_death_handler: "
        << death_drop.damage_events_with_death_handler << "\n";
    out << "  lethal_events_with_drop_entry: "
        << death_drop.lethal_events_with_drop_entry << "\n";
    out << "  lethal_events_with_drop_roll: "
        << death_drop.lethal_events_with_drop_roll << "\n";
    out << "  nonlethal_events_with_unexpected_drop: "
        << death_drop.nonlethal_events_with_unexpected_drop << "\n";
    out << "  drop_rolls_after_drop_entry: "
        << death_drop.drop_rolls_after_drop_entry << "\n";
    out << "  drop_rolls_before_drop_entry: "
        << death_drop.drop_rolls_before_drop_entry << "\n";
    out << "  missing_live_death_field_events: "
        << death_drop.missing_live_death_field_events << "\n";
    out << "  missing_death_handler_events: "
        << death_drop.missing_death_handler_events << "\n";
    out << "  missing_drop_entry_events: "
        << death_drop.missing_drop_entry_events << "\n";
    out << "  missing_drop_roll_events: "
        << death_drop.missing_drop_roll_events << "\n";
    out << "  first_damage_apply_draw_index: ";
    write_optional_int(out, death_drop.first_damage_apply_draw_index);
    out << "\n";
    out << "  first_death_handler_draw_index: ";
    write_optional_int(out, death_drop.first_death_handler_draw_index);
    out << "\n";
    out << "  first_drop_entry_draw_index: ";
    write_optional_int(out, death_drop.first_drop_entry_draw_index);
    out << "\n";
    out << "  first_drop_roll_draw_index: ";
    write_optional_int(out, death_drop.first_drop_roll_draw_index);
    out << "\n";
    if (!death_drop.damage_flows.empty()) {
        out << "  damage_flows:\n";
        for (const auto& flow : death_drop.damage_flows) {
            out << "    damage_event_index=" << flow.damage_event_index;
            out << " target_slot=";
            write_optional_int(out, flow.target_slot);
            out << " enemy_entry_id=";
            write_optional_int(out, flow.enemy_entry_id);
            out << " damage=";
            write_optional_int(out, flow.damage);
            out << " hp_before=";
            write_optional_int(out, flow.hp_before);
            out << " hp_after=";
            write_optional_int(out, flow.hp_after);
            out << " lethal=";
            write_optional_int(out, flow.lethal);
            out << " death_handler=" << (flow.observed_death_handler ? "true" : "false");
            out << " drop_entry=" << (flow.observed_drop_entry ? "true" : "false");
            out << " drop_roll=" << (flow.observed_drop_roll ? "true" : "false");
            out << " unexpected_nonlethal_drop="
                << (flow.unexpected_drop_for_nonlethal ? "true" : "false");
            out << "\n";
        }
    }

    const auto drop = summarize_drop_checkpoints(
        result.events,
        options.expected_drop_rolls);
    out << "\nDrop checkpoints\n";
    out << "  status: " << drop_checkpoint_status_name(drop.status) << "\n";
    out << "  expected_drop_rolls: ";
    if (drop.expected_drop_rolls.has_value()) {
        out << *drop.expected_drop_rolls << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_drop_rolls: " << drop.observed_drop_rolls << "\n";
    out << "  first_drop_roll_draw_index: ";
    if (drop.first_drop_roll_draw_index.has_value()) {
        out << *drop.first_drop_roll_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  last_drop_roll_draw_index: ";
    if (drop.last_drop_roll_draw_index.has_value()) {
        out << *drop.last_drop_roll_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  draws_with_target_slot: " << drop.draws_with_target_slot << "\n";
    out << "  draws_with_drop_row: " << drop.draws_with_drop_row << "\n";
    out << "  draws_with_rand_value: " << drop.draws_with_rand_value << "\n";
    out << "  observed_damage_bonus_draws: " << drop.observed_damage_bonus_draws << "\n";
    out << "  damage_bonus_draws_before_first_drop: "
        << drop.damage_bonus_draws_before_first_drop << "\n";
    out << "  damage_bonus_draws_after_first_drop: "
        << drop.damage_bonus_draws_after_first_drop << "\n";
    out << "  counter_rolls_before_first_drop: "
        << drop.counter_rolls_before_first_drop << " of " << drop.observed_counter_rolls << "\n";
    out << "  action_view_camera_draws_before_first_drop: "
        << drop.action_view_camera_draws_before_first_drop
        << " of " << drop.observed_action_view_camera_draws << "\n";
    out << "  status_attempt_draws_before_first_drop: "
        << drop.status_attempt_draws_before_first_drop
        << " of " << drop.observed_status_attempt_draws << "\n";
    if (!drop.draws.empty()) {
        out << "  draws:\n";
        for (const auto& draw : drop.draws) {
            out << "    draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (draw.target_slot.has_value()) {
                out << *draw.target_slot;
            } else {
                out << "unknown";
            }
            out << " row=";
            if (draw.drop_row_index.has_value()) {
                out << *draw.drop_row_index;
            } else {
                out << "unknown";
            }
            out << " rand_value=";
            if (draw.rand_value.has_value()) {
                out << *draw.rand_value;
            } else {
                out << "unknown";
            }
            out << " rand_mod100=";
            if (draw.rand_mod100.has_value()) {
                out << *draw.rand_mod100;
            } else {
                out << "unknown";
            }
            out << " drop_success=";
            if (draw.drop_success.has_value()) {
                out << *draw.drop_success;
            } else {
                out << "unknown";
            }
            out << "\n";
        }
    }

    const auto outcome = summarize_outcome_checkpoints(
        result.events,
        options.expected_end_turn_status_draws,
        options.expected_level_up_stat_rolls);
    out << "\nOutcome checkpoints\n";
    out << "  status: " << outcome_checkpoint_status_name(outcome.status) << "\n";
    out << "  rule: " << first_battle_outcome_checkpoint_rule_detail() << "\n";
    out << "  expected_end_turn_status_draws: ";
    if (outcome.expected_end_turn_status_draws.has_value()) {
        out << *outcome.expected_end_turn_status_draws << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  expected_level_up_stat_rolls: ";
    if (outcome.expected_level_up_stat_rolls.has_value()) {
        out << *outcome.expected_level_up_stat_rolls << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  observed_end_turn_status_draws: "
        << outcome.observed_end_turn_status_draws << "\n";
    out << "  observed_level_up_stat_rolls: "
        << outcome.observed_level_up_stat_rolls << "\n";
    out << "  observed_level_up_roll_1_draws: "
        << outcome.observed_level_up_roll_1_draws << "\n";
    out << "  observed_level_up_roll_2_draws: "
        << outcome.observed_level_up_roll_2_draws << "\n";
    out << "  observed_level_up_roll_3_draws: "
        << outcome.observed_level_up_roll_3_draws << "\n";
    out << "  observed_run_case9_events: "
        << outcome.observed_run_case9_events << "\n";
    out << "  observed_battle_success_events: "
        << outcome.observed_battle_success_events << "\n";
    out << "  observed_level_up_entries: "
        << outcome.observed_level_up_entries << "\n";
    out << "  observed_victory_branch_events: "
        << outcome.observed_victory_branch_events << "\n";
    out << "  battle_success_events_with_reward_context: "
        << outcome.battle_success_events_with_reward_context << "\n";
    out << "  level_up_entries_with_exp_context: "
        << outcome.level_up_entries_with_exp_context << "\n";
    out << "  level_up_entries_with_expected_rolls: "
        << outcome.level_up_entries_with_expected_rolls << "\n";
    out << "  expected_level_up_stat_rolls_from_entries: "
        << outcome.expected_level_up_stat_rolls_from_entries << "\n";
    out << "  stat_rolls_after_level_up_entry: "
        << outcome.stat_rolls_after_level_up_entry << "\n";
    out << "  stat_rolls_before_level_up_entry: "
        << outcome.stat_rolls_before_level_up_entry << "\n";
    out << "  first_end_turn_status_draw_index: ";
    write_optional_int(out, outcome.first_end_turn_status_draw_index);
    out << "\n";
    out << "  first_run_case9_draw_index: ";
    write_optional_int(out, outcome.first_run_case9_draw_index);
    out << "\n";
    out << "  first_battle_success_draw_index: ";
    write_optional_int(out, outcome.first_battle_success_draw_index);
    out << "\n";
    out << "  first_level_up_entry_draw_index: ";
    write_optional_int(out, outcome.first_level_up_entry_draw_index);
    out << "\n";
    out << "  first_level_up_stat_roll_index: ";
    write_optional_int(out, outcome.first_level_up_stat_roll_index);
    out << "\n";
    out << "  draws_with_actor_slot: " << outcome.draws_with_actor_slot << "\n";
    out << "  draws_with_rand_value: " << outcome.draws_with_rand_value << "\n";
    if (!outcome.draws.empty()) {
        out << "  events:\n";
        for (const auto& draw : outcome.draws) {
            out << "    draw_index=";
            write_optional_int(out, draw.draw_index);
            out << " kind=" << outcome_checkpoint_kind_name(draw.kind);
            out << " owner=" << draw.owner;
            out << " actor_slot=";
            write_optional_int(out, draw.actor_slot);
            out << " status_effect_id=";
            write_optional_int(out, draw.status_effect_id);
            out << " stat_index=";
            write_optional_int(out, draw.stat_index);
            out << " level=";
            write_optional_int(out, draw.level);
            out << " level_before=";
            write_optional_int(out, draw.level_before);
            out << " level_after=";
            write_optional_int(out, draw.level_after);
            out << " exp_before=";
            write_optional_int(out, draw.exp_before);
            out << " exp_after=";
            write_optional_int(out, draw.exp_after);
            out << " next_level_exp=";
            write_optional_int(out, draw.next_level_exp);
            out << " exp_awarded=";
            write_optional_int(out, draw.exp_awarded);
            out << " expected_stat_rolls=";
            write_optional_int(out, draw.expected_stat_rolls);
            out << " battle_outcome=";
            write_optional_int(out, draw.battle_outcome);
            out << " victory=";
            write_optional_int(out, draw.victory);
            out << " rand_value=";
            write_optional_int(out, draw.rand_value);
            out << "\n";
        }
    }

    out << "\nFirst events\n";
    const auto limit = std::min<std::size_t>(result.events.size(), 10);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& event = result.events[i];
        out << "  line " << event.line_number
            << " draw_before=";
        if (event.rng_draw_index_before.has_value()) {
            out << *event.rng_draw_index_before;
        } else {
            out << "unknown";
        }
        out << " pc=" << event.pc
            << " function=" << event.function
            << " checkpoint=" << event.checkpoint;
        if (!event.known_rng_owner.empty()) {
            out << " owner=" << event.known_rng_owner;
        }
        if (event.rng_seed_before.has_value()) {
            out << " seed_before=" << seed_hex(*event.rng_seed_before);
        }
        if (event.rng_seed_after.has_value()) {
            out << " seed_after=" << seed_hex(*event.rng_seed_after);
        }
        out << "\n";
    }
}

void write_json_report(
    const TraceCheckpointsOptions& options,
    const CheckpointParseResult& result,
    std::ostream& out) {
    int rng_draw_events = 0;
    for (const auto& event : result.events) {
        if (event.owns_rng_draw) {
            ++rng_draw_events;
        }
    }

    out << "{\n";
    out << "  \"checkpoint_file\": \"" << json_escape(options.checkpoint_file.string()) << "\",\n";
    out << "  \"turn_job_id\": ";
    if (options.turn_job_id.has_value()) {
        out << *options.turn_job_id;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"exec_job_id\": ";
    if (options.exec_job_id.has_value()) {
        out << *options.exec_job_id;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_fake_attacks\": ";
    if (options.expected_fake_attacks.has_value()) {
        out << *options.expected_fake_attacks;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_enemy_setup_draws\": ";
    if (options.expected_enemy_setup_draws.has_value()) {
        out << *options.expected_enemy_setup_draws;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_mode0e_camera_draws\": ";
    if (options.expected_mode0e_camera_draws.has_value()) {
        out << *options.expected_mode0e_camera_draws;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_turn_order_draws\": ";
    if (options.expected_turn_order_draws.has_value()) {
        out << *options.expected_turn_order_draws;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_attack_events\": ";
    if (options.expected_attack_events.has_value()) {
        out << *options.expected_attack_events;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_crit_draws\": ";
    if (options.expected_crit_draws.has_value()) {
        out << *options.expected_crit_draws;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_counter_roll_ceiling\": ";
    if (options.expected_counter_roll_ceiling.has_value()) {
        out << *options.expected_counter_roll_ceiling;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_drop_rolls\": ";
    if (options.expected_drop_rolls.has_value()) {
        out << *options.expected_drop_rolls;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_end_turn_status_draws\": ";
    if (options.expected_end_turn_status_draws.has_value()) {
        out << *options.expected_end_turn_status_draws;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"expected_level_up_stat_rolls\": ";
    if (options.expected_level_up_stat_rolls.has_value()) {
        out << *options.expected_level_up_stat_rolls;
    } else {
        out << "null";
    }
    out << ",\n";
    out << "  \"db_root\": \"" << json_escape(options.db_root.string()) << "\",\n";
    out << "  \"events\": " << result.events.size() << ",\n";
    out << "  \"rng_draw_events\": " << rng_draw_events << ",\n";
    out << "  \"errors\": [";
    for (std::size_t i = 0; i < result.errors.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.errors[i]) << "\"";
    }
    out << "],\n";
    out << "  \"warnings\": [";
    for (std::size_t i = 0; i < result.warnings.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(result.warnings[i]) << "\"";
    }
    out << "],\n";
    out << "  \"known_rng_owners_observed\": {";
    const auto counts = count_known_rng_owners(result.events);
    bool first = true;
    for (const auto& [owner, count] : counts) {
        if (!first) {
            out << ", ";
        }
        first = false;
        out << "\"" << json_escape(owner) << "\": " << count;
    }
    out << "},\n";

    const auto pre_ai = summarize_pre_ai_checkpoints(result.events, options.expected_fake_attacks);
    out << "  \"pre_ai_checkpoints\": {";
    out << "\"status\": \"" << pre_ai_checkpoint_status_name(pre_ai.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_pre_ai_checkpoint_rule_detail()) << "\"";
    out << ", \"expected_fake_attack_attempts\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_fake_attack_attempts;
    } else {
        out << "null";
    }
    out << ", \"expected_fake_attack_draws\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_fake_attack_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_battle_start_camera_draws\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_battle_start_camera_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_targeting_camera_draws\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_targeting_camera_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_total_pre_ai_draws\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_total_pre_ai_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_first_soldier_ai_draw_index_before\": ";
    if (pre_ai.expectation.has_value()) {
        out << pre_ai.expectation->expected_first_soldier_ai_draw_index_before;
    } else {
        out << "null";
    }
    out << ", \"observed_battle_start_camera_draws\": "
        << pre_ai.observed_battle_start_camera_draws;
    out << ", \"observed_targeting_camera_draws\": "
        << pre_ai.observed_targeting_camera_draws;
    out << ", \"observed_fake_attack_attempts\": "
        << pre_ai.observed_fake_attack_attempts;
    out << ", \"observed_fake_attack_draws\": "
        << pre_ai.observed_fake_attack_draws;
    out << ", \"observed_skipped_fake_attack_draws\": "
        << pre_ai.observed_skipped_fake_attack_draws;
    out << ", \"fake_attack_draws_with_frame_gap\": "
        << pre_ai.fake_attack_draws_with_frame_gap;
    out << ", \"skipped_fake_attempts_with_frame_gap\": "
        << pre_ai.skipped_fake_attempts_with_frame_gap;
    out << ", \"min_fake_attack_draw_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.min_fake_attack_draw_camera_frame_gap);
    out << ", \"max_fake_attack_draw_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.max_fake_attack_draw_camera_frame_gap);
    out << ", \"min_skipped_fake_attempt_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.min_skipped_fake_attempt_camera_frame_gap);
    out << ", \"max_skipped_fake_attempt_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.max_skipped_fake_attempt_camera_frame_gap);
    out << ", \"observed_pre_ai_draws\": " << pre_ai.observed_pre_ai_draws;
    out << ", \"observed_first_soldier_ai_draws\": "
        << pre_ai.observed_first_soldier_ai_draws;
    out << ", \"targeting_draws_with_target_slot\": "
        << pre_ai.targeting_draws_with_target_slot;
    out << ", \"fake_draws_with_fake_attack_index\": "
        << pre_ai.fake_draws_with_fake_attack_index;
    out << ", \"first_battle_start_camera_draw_index\": ";
    if (pre_ai.first_battle_start_camera_draw_index.has_value()) {
        out << *pre_ai.first_battle_start_camera_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_targeting_camera_draw_index\": ";
    if (pre_ai.first_targeting_camera_draw_index.has_value()) {
        out << *pre_ai.first_targeting_camera_draw_index;
    } else {
        out << "null";
    }
    out << ", \"last_targeting_camera_draw_index\": ";
    if (pre_ai.last_targeting_camera_draw_index.has_value()) {
        out << *pre_ai.last_targeting_camera_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_fake_attack_draw_index\": ";
    if (pre_ai.first_fake_attack_draw_index.has_value()) {
        out << *pre_ai.first_fake_attack_draw_index;
    } else {
        out << "null";
    }
    out << ", \"last_fake_attack_draw_index\": ";
    if (pre_ai.last_fake_attack_draw_index.has_value()) {
        out << *pre_ai.last_fake_attack_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_soldier_ai_draw_index\": ";
    if (pre_ai.first_soldier_ai_draw_index.has_value()) {
        out << *pre_ai.first_soldier_ai_draw_index;
    } else {
        out << "null";
    }
    out << ", \"pre_ai_draws_before_first_soldier_ai\": "
        << pre_ai.pre_ai_draws_before_first_soldier_ai;
    out << ", \"draws\": [";
    for (std::size_t i = 0; i < pre_ai.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = pre_ai.draws[i];
        out << "{\"owner\": \"" << json_escape(draw.owner) << "\"";
        out << ", \"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"active_slot\": ";
        if (draw.active_slot.has_value()) {
            out << *draw.active_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (draw.target_slot.has_value()) {
            out << *draw.target_slot;
        } else {
            out << "null";
        }
        out << ", \"fake_attack_index\": ";
        if (draw.fake_attack_index.has_value()) {
            out << *draw.fake_attack_index;
        } else {
            out << "null";
        }
        out << ", \"camera_frame_gap\": ";
        if (draw.camera_frame_gap.has_value()) {
            out << *draw.camera_frame_gap;
        } else {
            out << "null";
        }
        out << ", \"skipped_rng_draw\": "
            << (draw.skipped_rng_draw ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto action_setup =
        summarize_action_setup_checkpoints(result.events, options.expected_enemy_setup_draws);
    out << "  \"action_setup_checkpoints\": {";
    out << "\"status\": \"" << action_setup_checkpoint_status_name(action_setup.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_action_setup_checkpoint_rule_detail()) << "\"";
    out << ", \"expected_enemy_setup_draws\": ";
    if (action_setup.expected_enemy_setup_draws.has_value()) {
        out << *action_setup.expected_enemy_setup_draws;
    } else {
        out << "null";
    }
    out << ", \"observed_setup_action_events\": "
        << action_setup.observed_setup_action_events;
    out << ", \"observed_pc_handler_entries\": "
        << action_setup.observed_pc_handler_entries;
    out << ", \"observed_enemy_handler_entries\": "
        << action_setup.observed_enemy_handler_entries;
    out << ", \"observed_enemy_setup_draws\": "
        << action_setup.observed_enemy_setup_draws;
    out << ", \"setup_events_with_actor_slot\": "
        << action_setup.setup_events_with_actor_slot;
    out << ", \"setup_events_with_handler_pc\": "
        << action_setup.setup_events_with_handler_pc;
    out << ", \"setup_events_with_instruction\": "
        << action_setup.setup_events_with_instruction;
    out << ", \"setup_events_with_target_slot\": "
        << action_setup.setup_events_with_target_slot;
    out << ", \"setup_events_with_instr_param\": "
        << action_setup.setup_events_with_instr_param;
    out << ", \"handler_entries_with_instruction\": "
        << action_setup.handler_entries_with_instruction;
    out << ", \"handler_entries_with_target_slot\": "
        << action_setup.handler_entries_with_target_slot;
    out << ", \"handler_entries_with_instr_param\": "
        << action_setup.handler_entries_with_instr_param;
    out << ", \"handler_entries_with_movement_flags\": "
        << action_setup.handler_entries_with_movement_flags;
    out << ", \"handler_matches\": " << action_setup.handler_matches;
    out << ", \"handler_mismatches\": " << action_setup.handler_mismatches;
    out << ", \"setup_handler_field_comparisons\": "
        << action_setup.setup_handler_field_comparisons;
    out << ", \"setup_handler_field_matches\": "
        << action_setup.setup_handler_field_matches;
    out << ", \"setup_handler_field_mismatches\": "
        << action_setup.setup_handler_field_mismatches;
    out << ", \"pc_setup_handler_field_comparisons\": "
        << action_setup.pc_setup_handler_field_comparisons;
    out << ", \"pc_setup_handler_field_matches\": "
        << action_setup.pc_setup_handler_field_matches;
    out << ", \"pc_setup_handler_field_mismatches\": "
        << action_setup.pc_setup_handler_field_mismatches;
    out << ", \"enemy_setup_handler_field_comparisons\": "
        << action_setup.enemy_setup_handler_field_comparisons;
    out << ", \"enemy_setup_handler_field_matches\": "
        << action_setup.enemy_setup_handler_field_matches;
    out << ", \"enemy_setup_handler_field_mismatches\": "
        << action_setup.enemy_setup_handler_field_mismatches;
    out << ", \"enemy_setup_draws_with_gate_inputs\": "
        << action_setup.enemy_setup_draws_with_gate_inputs;
    out << ", \"enemy_setup_draws_with_rand_value\": "
        << action_setup.enemy_setup_draws_with_rand_value;
    out << ", \"enemy_setup_draws_with_rand_mod10\": "
        << action_setup.enemy_setup_draws_with_rand_mod10;
    out << ", \"enemy_setup_draws_with_direct_close_candidate\": "
        << action_setup.enemy_setup_draws_with_direct_close_candidate;
    out << ", \"enemy_setup_draws_with_final_instr_param\": "
        << action_setup.enemy_setup_draws_with_final_instr_param;
    out << ", \"enemy_setup_draws_with_helper_8008a174_result\": "
        << action_setup.enemy_setup_draws_with_helper_8008a174_result;
    out << ", \"enemy_setup_draws_with_helper_8008a280_marker\": "
        << action_setup.enemy_setup_draws_with_helper_8008a280_marker;
    out << ", \"enemy_setup_draws_with_helper_80082340_result\": "
        << action_setup.enemy_setup_draws_with_helper_80082340_result;
    out << ", \"enemy_setup_draws_with_target_adjacency\": "
        << action_setup.enemy_setup_draws_with_target_adjacency;
    out << ", \"enemy_setup_draws_with_target_distance\": "
        << action_setup.enemy_setup_draws_with_target_distance;
    out << ", \"enemy_setup_draws_with_selected_worker\": "
        << action_setup.enemy_setup_draws_with_selected_worker;
    out << ", \"enemy_setup_draws_with_required_helper_fields\": "
        << action_setup.enemy_setup_draws_with_required_helper_fields;
    out << ", \"worker_matches\": " << action_setup.worker_matches;
    out << ", \"worker_mismatches\": " << action_setup.worker_mismatches;
    out << ", \"first_setup_action_draw_index\": ";
    if (action_setup.first_setup_action_draw_index.has_value()) {
        out << *action_setup.first_setup_action_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_pc_handler_draw_index\": ";
    if (action_setup.first_pc_handler_draw_index.has_value()) {
        out << *action_setup.first_pc_handler_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_enemy_handler_draw_index\": ";
    if (action_setup.first_enemy_handler_draw_index.has_value()) {
        out << *action_setup.first_enemy_handler_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_enemy_setup_draw_index\": ";
    if (action_setup.first_enemy_setup_draw_index.has_value()) {
        out << *action_setup.first_enemy_setup_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_attack_hit_draw_index\": ";
    if (action_setup.first_attack_hit_draw_index.has_value()) {
        out << *action_setup.first_attack_hit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"enemy_setup_draws_before_first_attack_hit\": "
        << action_setup.enemy_setup_draws_before_first_attack_hit;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < action_setup.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = action_setup.events[i];
        out << "{\"kind\": \"" << action_setup_checkpoint_kind_name(event.kind) << "\"";
        out << ", \"draw_index\": ";
        if (event.draw_index.has_value()) {
            out << *event.draw_index;
        } else {
            out << "null";
        }
        out << ", \"actor_slot\": ";
        if (event.actor_slot.has_value()) {
            out << *event.actor_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (event.target_slot.has_value()) {
            out << *event.target_slot;
        } else {
            out << "null";
        }
        out << ", \"instruction\": ";
        if (event.instruction.has_value()) {
            out << *event.instruction;
        } else {
            out << "null";
        }
        out << ", \"instr_param_0x6\": ";
        if (event.instr_param_0x6.has_value()) {
            out << *event.instr_param_0x6;
        } else {
            out << "null";
        }
        out << ", \"movement_flags\": ";
        if (event.movement_flags.has_value()) {
            out << *event.movement_flags;
        } else {
            out << "null";
        }
        out << ", \"expected_handler_pc\": ";
        if (event.expected_handler_pc.has_value()) {
            out << "\"" << json_escape(*event.expected_handler_pc) << "\"";
        } else {
            out << "null";
        }
        out << ", \"handler_pc\": ";
        if (event.handler_pc.has_value()) {
            out << "\"" << json_escape(*event.handler_pc) << "\"";
        } else {
            out << "null";
        }
        out << ", \"matched_setup_draw_index\": ";
        write_json_optional_int(out, event.matched_setup_draw_index);
        out << ", \"instruction_matches_setup\": ";
        write_json_optional_bool(out, event.instruction_matches_setup);
        out << ", \"target_slot_matches_setup\": ";
        write_json_optional_bool(out, event.target_slot_matches_setup);
        out << ", \"instr_param_matches_setup\": ";
        write_json_optional_bool(out, event.instr_param_matches_setup);
        out << ", \"setup_rand\": ";
        if (event.setup_rand.has_value()) {
            out << *event.setup_rand;
        } else {
            out << "null";
        }
        out << ", \"setup_rand_mod10\": ";
        if (event.setup_rand_mod10.has_value()) {
            out << *event.setup_rand_mod10;
        } else {
            out << "null";
        }
        out << ", \"direct_close_candidate\": ";
        if (event.direct_close_candidate.has_value()) {
            out << *event.direct_close_candidate;
        } else {
            out << "null";
        }
        out << ", \"final_instr_param_0x6\": ";
        write_json_optional_int(out, event.final_instr_param_0x6);
        out << ", \"helper_8008a174_result\": ";
        write_json_optional_int(out, event.helper_8008a174_result);
        out << ", \"helper_8008a280_reached\": ";
        write_json_optional_bool(out, event.helper_8008a280_reached);
        out << ", \"helper_80082340_result\": ";
        write_json_optional_int(out, event.helper_80082340_result);
        out << ", \"target_adjacent\": ";
        write_json_optional_int(out, event.target_adjacent);
        out << ", \"target_distance\": ";
        write_json_optional_int(out, event.target_distance);
        out << ", \"expected_worker_pc\": ";
        write_json_optional_string(out, event.expected_worker_pc);
        out << ", \"selected_worker_pc\": ";
        write_json_optional_string(out, event.selected_worker_pc);
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto action_source_expectation = first_battle_action_source_checkpoint_expectation();
    const auto action_source = summarize_action_source_checkpoints(
        result.events,
        action_source_expectation.expected_handler_pc);
    out << "  \"action_source_checkpoints\": {";
    out << "\"status\": \"" << action_source_checkpoint_status_name(action_source.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_action_source_checkpoint_rule_detail()) << "\"";
    out << ", \"expected_handler_pc\": ";
    if (action_source.expected_handler_pc.has_value()) {
        out << "\"" << json_escape(*action_source.expected_handler_pc) << "\"";
    } else {
        out << "null";
    }
    out << ", \"expected_callback_pc\": ";
    if (action_source.expected_callback_pc.has_value()) {
        out << "\"" << json_escape(*action_source.expected_callback_pc) << "\"";
    } else {
        out << "null";
    }
    out << ", \"observed_source_selection_events\": "
        << action_source.observed_source_selection_events;
    out << ", \"observed_action_source_events\": "
        << action_source.observed_action_source_events;
    out << ", \"source_selection_events_with_source_slot\": "
        << action_source.source_selection_events_with_source_slot;
    out << ", \"source_selection_events_with_actor_slot\": "
        << action_source.source_selection_events_with_actor_slot;
    out << ", \"source_selection_events_with_target_slot\": "
        << action_source.source_selection_events_with_target_slot;
    out << ", \"events_with_actor_slot\": " << action_source.events_with_actor_slot;
    out << ", \"events_with_source_slot\": " << action_source.events_with_source_slot;
    out << ", \"events_with_action_id\": " << action_source.events_with_action_id;
    out << ", \"events_with_handler_pc\": " << action_source.events_with_handler_pc;
    out << ", \"events_with_callback_pc\": " << action_source.events_with_callback_pc;
    out << ", \"events_with_source_field6\": " << action_source.events_with_source_field6;
    out << ", \"events_with_actor_field6\": " << action_source.events_with_actor_field6;
    out << ", \"source_selection_bridge_pairs\": "
        << action_source.source_selection_bridge_pairs;
    out << ", \"source_selection_bridge_matches\": "
        << action_source.source_selection_bridge_matches;
    out << ", \"source_selection_bridge_mismatches\": "
        << action_source.source_selection_bridge_mismatches;
    out << ", \"field6_matches\": " << action_source.field6_matches;
    out << ", \"field6_mismatches\": " << action_source.field6_mismatches;
    out << ", \"handler_matches\": " << action_source.handler_matches;
    out << ", \"handler_mismatches\": " << action_source.handler_mismatches;
    out << ", \"callback_matches\": " << action_source.callback_matches;
    out << ", \"callback_mismatches\": " << action_source.callback_mismatches;
    out << ", \"first_source_selection_draw_index\": ";
    if (action_source.first_source_selection_draw_index.has_value()) {
        out << *action_source.first_source_selection_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_action_source_draw_index\": ";
    if (action_source.first_action_source_draw_index.has_value()) {
        out << *action_source.first_action_source_draw_index;
    } else {
        out << "null";
    }
    out << ", \"events\": [";
    for (std::size_t i = 0; i < action_source.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = action_source.events[i];
        out << "{\"kind\": \"" << action_source_checkpoint_kind_name(event.kind) << "\"";
        out << ", \"draw_index\": ";
        if (event.draw_index.has_value()) {
            out << *event.draw_index;
        } else {
            out << "null";
        }
        out << ", \"actor_slot\": ";
        if (event.actor_slot.has_value()) {
            out << *event.actor_slot;
        } else {
            out << "null";
        }
        out << ", \"source_slot\": ";
        if (event.source_slot.has_value()) {
            out << *event.source_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (event.target_slot.has_value()) {
            out << *event.target_slot;
        } else {
            out << "null";
        }
        out << ", \"action_id\": ";
        if (event.action_id.has_value()) {
            out << *event.action_id;
        } else {
            out << "null";
        }
        out << ", \"source_field6_0x6\": ";
        if (event.source_field6_0x6.has_value()) {
            out << *event.source_field6_0x6;
        } else {
            out << "null";
        }
        out << ", \"actor_field6_0x6\": ";
        if (event.actor_field6_0x6.has_value()) {
            out << *event.actor_field6_0x6;
        } else {
            out << "null";
        }
        out << ", \"handler_pc\": ";
        if (event.handler_pc.has_value()) {
            out << "\"" << json_escape(*event.handler_pc) << "\"";
        } else {
            out << "null";
        }
        out << ", \"callback_pc\": ";
        if (event.callback_pc.has_value()) {
            out << "\"" << json_escape(*event.callback_pc) << "\"";
        } else {
            out << "null";
        }
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto action_view_gate = summarize_action_view_gate_checkpoints(result.events);
    out << "  \"action_view_gate_checkpoints\": {";
    out << "\"status\": \"" << action_view_gate_checkpoint_status_name(action_view_gate.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_action_view_gate_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_gate_events\": " << action_view_gate.observed_gate_events;
    out << ", \"events_with_aux_list_root\": "
        << action_view_gate.events_with_aux_list_root;
    out << ", \"events_with_query_args\": " << action_view_gate.events_with_query_args;
    out << ", \"events_with_query_result\": " << action_view_gate.events_with_query_result;
    out << ", \"events_with_selected_record_mode\": "
        << action_view_gate.events_with_selected_record_mode;
    out << ", \"query_args_match\": " << action_view_gate.query_args_match;
    out << ", \"query_args_mismatch\": " << action_view_gate.query_args_mismatch;
    out << ", \"selected_mode_matches\": " << action_view_gate.selected_mode_matches;
    out << ", \"selected_mode_mismatches\": "
        << action_view_gate.selected_mode_mismatches;
    out << ", \"events_with_action_child_thread\": "
        << action_view_gate.events_with_action_child_thread;
    out << ", \"events_with_child_payload\": " << action_view_gate.events_with_child_payload;
    out << ", \"events_with_nested_payload\": " << action_view_gate.events_with_nested_payload;
    out << ", \"events_with_child_thread_state\": "
        << action_view_gate.events_with_child_thread_state;
    out << ", \"events_with_scheduler_chain\": "
        << action_view_gate.events_with_scheduler_chain;
    out << ", \"events_with_mode0_fallback_flag\": "
        << action_view_gate.events_with_mode0_fallback_flag;
    out << ", \"mode0_fallback_reached_events\": "
        << action_view_gate.mode0_fallback_reached_events;
    out << ", \"observed_mode0e_camera_draws\": "
        << action_view_gate.observed_mode0e_camera_draws;
    out << ", \"observed_mode0_fallback_draws\": "
        << action_view_gate.observed_mode0_fallback_draws;
    out << ", \"observed_attack_hit_draws\": "
        << action_view_gate.observed_attack_hit_draws;
    out << ", \"first_gate_draw_index\": ";
    if (action_view_gate.first_gate_draw_index.has_value()) {
        out << *action_view_gate.first_gate_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_mode0e_draw_index\": ";
    if (action_view_gate.first_mode0e_draw_index.has_value()) {
        out << *action_view_gate.first_mode0e_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_mode0_fallback_draw_index\": ";
    if (action_view_gate.first_mode0_fallback_draw_index.has_value()) {
        out << *action_view_gate.first_mode0_fallback_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_attack_hit_draw_index\": ";
    if (action_view_gate.first_attack_hit_draw_index.has_value()) {
        out << *action_view_gate.first_attack_hit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"gate_events_before_first_mode0e\": "
        << action_view_gate.gate_events_before_first_mode0e;
    out << ", \"gate_events_before_first_attack_hit\": "
        << action_view_gate.gate_events_before_first_attack_hit;
    out << ", \"mode0e_draws_before_first_attack_hit\": "
        << action_view_gate.mode0e_draws_before_first_attack_hit;
    out << ", \"mode0_fallback_draws_before_first_mode0e\": "
        << action_view_gate.mode0_fallback_draws_before_first_mode0e;
    out << ", \"mode0_fallback_draws_before_first_attack_hit\": "
        << action_view_gate.mode0_fallback_draws_before_first_attack_hit;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < action_view_gate.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = action_view_gate.events[i];
        out << "{\"draw_index\": ";
        if (event.draw_index.has_value()) {
            out << *event.draw_index;
        } else {
            out << "null";
        }
        out << ", \"active_slot\": ";
        if (event.active_slot.has_value()) {
            out << *event.active_slot;
        } else {
            out << "null";
        }
        out << ", \"source_slot\": ";
        if (event.source_slot.has_value()) {
            out << *event.source_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (event.target_slot.has_value()) {
            out << *event.target_slot;
        } else {
            out << "null";
        }
        out << ", \"source_field6_0x6\": ";
        if (event.source_field6_0x6.has_value()) {
            out << *event.source_field6_0x6;
        } else {
            out << "null";
        }
        out << ", \"actor_field6_0x6\": ";
        if (event.actor_field6_0x6.has_value()) {
            out << *event.actor_field6_0x6;
        } else {
            out << "null";
        }
        out << ", \"aux_list_root\": ";
        if (event.aux_list_root.has_value()) {
            out << "\"" << json_escape(*event.aux_list_root) << "\"";
        } else {
            out << "null";
        }
        out << ", \"query_arg0\": ";
        if (event.query_arg0.has_value()) {
            out << *event.query_arg0;
        } else {
            out << "null";
        }
        out << ", \"query_arg1\": ";
        if (event.query_arg1.has_value()) {
            out << *event.query_arg1;
        } else {
            out << "null";
        }
        out << ", \"query_arg2\": ";
        if (event.query_arg2.has_value()) {
            out << *event.query_arg2;
        } else {
            out << "null";
        }
        out << ", \"query_arg3\": ";
        if (event.query_arg3.has_value()) {
            out << *event.query_arg3;
        } else {
            out << "null";
        }
        out << ", \"query_result\": ";
        if (event.query_result.has_value()) {
            out << "\"" << json_escape(*event.query_result) << "\"";
        } else {
            out << "null";
        }
        out << ", \"selected_record_mode\": ";
        if (event.selected_record_mode.has_value()) {
            out << *event.selected_record_mode;
        } else {
            out << "null";
        }
        out << ", \"action_child_thread\": ";
        write_json_optional_string(out, event.action_child_thread);
        out << ", \"child_payload\": ";
        write_json_optional_string(out, event.child_payload);
        out << ", \"nested_payload\": ";
        write_json_optional_string(out, event.nested_payload);
        out << ", \"child_thread_state_byte\": ";
        write_json_optional_int(out, event.child_thread_state_byte);
        out << ", \"mode0_fallback_reached\": ";
        write_json_optional_bool(out, event.mode0_fallback_reached);
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto action_view = summarize_action_view_camera_checkpoints(
        result.events,
        options.expected_mode0e_camera_draws);
    out << "  \"action_view_camera_checkpoints\": {";
    out << "\"status\": \"" << action_view_camera_checkpoint_status_name(action_view.status) << "\"";
    out << ", \"expected_mode0e_camera_draws\": ";
    if (action_view.expected_mode0e_camera_draws.has_value()) {
        out << *action_view.expected_mode0e_camera_draws;
    } else {
        out << "null";
    }
    out << ", \"observed_mode0e_camera_draws\": " << action_view.observed_mode0e_camera_draws;
    out << ", \"observed_mode0_fallback_draws\": " << action_view.observed_mode0_fallback_draws;
    out << ", \"observed_attack_hit_draws\": " << action_view.observed_attack_hit_draws;
    out << ", \"first_mode0e_draw_index\": ";
    if (action_view.first_mode0e_draw_index.has_value()) {
        out << *action_view.first_mode0e_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_attack_hit_draw_index\": ";
    if (action_view.first_attack_hit_draw_index.has_value()) {
        out << *action_view.first_attack_hit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"mode0e_draws_before_first_attack_hit\": "
        << action_view.mode0e_draws_before_first_attack_hit;
    out << ", \"mode0e_draws_after_first_attack_hit\": "
        << action_view.mode0e_draws_after_first_attack_hit;
    out << "},\n";

    const auto turn_order = summarize_turn_order_checkpoints(
        result.events,
        options.expected_turn_order_draws);
    out << "  \"turn_order_checkpoints\": {";
    out << "\"status\": \"" << turn_order_checkpoint_status_name(turn_order.status) << "\"";
    out << ", \"expected_priority_jitter_draws\": ";
    if (turn_order.expected_priority_jitter_draws.has_value()) {
        out << *turn_order.expected_priority_jitter_draws;
    } else {
        out << "null";
    }
    out << ", \"observed_priority_jitter_draws\": "
        << turn_order.observed_priority_jitter_draws;
    out << ", \"first_priority_jitter_draw_index\": ";
    if (turn_order.first_priority_jitter_draw_index.has_value()) {
        out << *turn_order.first_priority_jitter_draw_index;
    } else {
        out << "null";
    }
    out << ", \"last_priority_jitter_draw_index\": ";
    if (turn_order.last_priority_jitter_draw_index.has_value()) {
        out << *turn_order.last_priority_jitter_draw_index;
    } else {
        out << "null";
    }
    out << ", \"draws_with_slot\": " << turn_order.draws_with_slot;
    out << ", \"draws_with_quick\": " << turn_order.draws_with_quick;
    out << ", \"draws_with_assigned_priority\": "
        << turn_order.draws_with_assigned_priority;
    out << ", \"draws_with_rand_value\": " << turn_order.draws_with_rand_value;
    out << ", \"events_with_expected_first_battle_quick\": "
        << turn_order.events_with_expected_first_battle_quick;
    out << ", \"quick_matches\": " << turn_order.quick_matches;
    out << ", \"quick_mismatches\": " << turn_order.quick_mismatches;
    out << ", \"events_with_fixed_priority_result\": "
        << turn_order.events_with_fixed_priority_result;
    out << ", \"fixed_priority_zero_results\": "
        << turn_order.fixed_priority_zero_results;
    out << ", \"fixed_priority_nonzero_results\": "
        << turn_order.fixed_priority_nonzero_results;
    out << ", \"priority_sources_with_fixed_priority_result\": "
        << turn_order.priority_sources_with_fixed_priority_result;
    out << ", \"priority_sources_missing_fixed_priority_result\": "
        << turn_order.priority_sources_missing_fixed_priority_result;
    out << ", \"events_with_queue_metadata\": " << turn_order.events_with_queue_metadata;
    out << ", \"queue_metadata_matches\": " << turn_order.queue_metadata_matches;
    out << ", \"queue_metadata_mismatches\": " << turn_order.queue_metadata_mismatches;
    out << ", \"observed_queue_entries\": " << turn_order.observed_queue_entries;
    out << ", \"observed_execution_order_entries\": "
        << turn_order.observed_execution_order_entries;
    out << ", \"queue_entries_with_slot\": " << turn_order.queue_entries_with_slot;
    out << ", \"queue_entries_with_quick\": " << turn_order.queue_entries_with_quick;
    out << ", \"queue_entries_with_assigned_priority\": "
        << turn_order.queue_entries_with_assigned_priority;
    out << ", \"priority_draws_with_expected_priority\": "
        << turn_order.priority_draws_with_expected_priority;
    out << ", \"priority_matches\": " << turn_order.priority_matches;
    out << ", \"priority_mismatches\": " << turn_order.priority_mismatches;
    out << ", \"incomplete_queue_entries\": " << turn_order.incomplete_queue_entries;
    out << ", \"incomplete_execution_order_entries\": "
        << turn_order.incomplete_execution_order_entries;
    out << ", \"execution_order_compared\": "
        << (turn_order.execution_order_compared ? "true" : "false");
    out << ", \"execution_order_exact\": "
        << (turn_order.execution_order_exact ? "true" : "false");
    out << ", \"priority_ties_observed\": "
        << (turn_order.priority_ties_observed ? "true" : "false");
    out << ", \"execution_order_matches\": " << turn_order.execution_order_matches;
    out << ", \"execution_order_mismatches\": "
        << turn_order.execution_order_mismatches;
    out << ", \"expected_execution_slots\": [";
    for (std::size_t i = 0; i < turn_order.expected_execution_slots.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << turn_order.expected_execution_slots[i];
    }
    out << "]";
    out << ", \"observed_execution_slots\": [";
    for (std::size_t i = 0; i < turn_order.observed_execution_slots.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << turn_order.observed_execution_slots[i];
    }
    out << "]";
    out << ", \"events\": [";
    for (std::size_t i = 0; i < turn_order.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = turn_order.draws[i];
        out << "{\"draw_index\": ";
        write_json_optional_int(out, draw.draw_index);
        out << ", \"kind\": \""
            << turn_order_checkpoint_kind_name(draw.kind) << "\"";
        out << ", \"slot\": ";
        write_json_optional_int(out, draw.slot);
        out << ", \"quick\": ";
        write_json_optional_int(out, draw.quick);
        out << ", \"queued_instruction\": ";
        write_json_optional_int(out, draw.queued_instruction);
        out << ", \"target_slot\": ";
        write_json_optional_int(out, draw.target_slot);
        out << ", \"initial_priority\": ";
        write_json_optional_int(out, draw.initial_priority);
        out << ", \"fixed_priority_result\": ";
        write_json_optional_int(out, draw.fixed_priority_result);
        out << ", \"fixed_priority_value\": ";
        write_json_optional_int(out, draw.fixed_priority_value);
        out << ", \"jitter_modulus\": ";
        write_json_optional_int(out, draw.jitter_modulus);
        out << ", \"sum_quick\": ";
        write_json_optional_int(out, draw.sum_quick);
        out << ", \"queued_count\": ";
        write_json_optional_int(out, draw.queued_count);
        out << ", \"queue_index\": ";
        write_json_optional_int(out, draw.queue_index);
        out << ", \"execution_index\": ";
        write_json_optional_int(out, draw.execution_index);
        out << ", \"assigned_priority\": ";
        write_json_optional_int(out, draw.assigned_priority);
        out << ", \"expected_assigned_priority\": ";
        write_json_optional_int(out, draw.expected_assigned_priority);
        out << ", \"expected_first_battle_quick\": ";
        write_json_optional_int(out, draw.expected_first_battle_quick);
        out << ", \"expected_jitter_modulus\": ";
        write_json_optional_int(out, draw.expected_jitter_modulus);
        out << ", \"expected_execution_slot\": ";
        write_json_optional_int(out, draw.expected_execution_slot);
        out << ", \"rand_value\": ";
        write_json_optional_int(out, draw.rand_value);
        out << ", \"priority_matches\": "
            << (draw.priority_matches ? "true" : "false");
        out << ", \"quick_matches\": "
            << (draw.quick_matches ? "true" : "false");
        out << ", \"queue_metadata_matches\": "
            << (draw.queue_metadata_matches ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto attack_resolution = summarize_attack_resolution_checkpoints(
        result.events,
        options.expected_attack_events,
        options.expected_crit_draws);
    out << "  \"attack_resolution_checkpoints\": {";
    out << "\"status\": \"" << attack_resolution_checkpoint_status_name(attack_resolution.status) << "\"";
    out << ", \"expected_attack_events\": ";
    if (attack_resolution.expected_attack_events.has_value()) {
        out << *attack_resolution.expected_attack_events;
    } else {
        out << "null";
    }
    out << ", \"expected_crit_draws\": ";
    if (attack_resolution.expected_crit_draws.has_value()) {
        out << *attack_resolution.expected_crit_draws;
    } else {
        out << "null";
    }
    out << ", \"observed_hit_draws\": " << attack_resolution.observed_hit_draws;
    out << ", \"observed_crit_draws\": " << attack_resolution.observed_crit_draws;
    out << ", \"observed_damage_spread_draws\": "
        << attack_resolution.observed_damage_spread_draws;
    out << ", \"observed_damage_bonus_draws\": "
        << attack_resolution.observed_damage_bonus_draws;
    out << ", \"first_hit_draw_index\": ";
    if (attack_resolution.first_hit_draw_index.has_value()) {
        out << *attack_resolution.first_hit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_damage_spread_draw_index\": ";
    if (attack_resolution.first_damage_spread_draw_index.has_value()) {
        out << *attack_resolution.first_damage_spread_draw_index;
    } else {
        out << "null";
    }
    out << ", \"damage_pairs_in_order\": "
        << attack_resolution.damage_pairs_in_order;
    out << ", \"damage_pairs_out_of_order\": "
        << attack_resolution.damage_pairs_out_of_order;
    out << "},\n";

    const auto attack_damage_values = summarize_attack_damage_value_checkpoints(result.events);
    out << "  \"attack_damage_value_checkpoints\": {";
    out << "\"status\": \""
        << attack_damage_value_checkpoint_status_name(attack_damage_values.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_attack_damage_value_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_attack_bursts\": "
        << attack_damage_values.observed_attack_bursts;
    out << ", \"orphan_damage_draw_events\": "
        << attack_damage_values.orphan_damage_draw_events;
    out << ", \"bursts_with_live_inputs\": "
        << attack_damage_values.bursts_with_live_inputs;
    out << ", \"bursts_with_required_draws\": "
        << attack_damage_values.bursts_with_required_draws;
    out << ", \"bursts_with_observed_attack_result\": "
        << attack_damage_values.bursts_with_observed_attack_result;
    out << ", \"bursts_with_observed_damage\": "
        << attack_damage_values.bursts_with_observed_damage;
    out << ", \"bursts_with_damage_apply\": "
        << attack_damage_values.bursts_with_damage_apply;
    out << ", \"bursts_with_damage_apply_fields\": "
        << attack_damage_values.bursts_with_damage_apply_fields;
    out << ", \"damage_apply_events_after_damage_draws\": "
        << attack_damage_values.damage_apply_events_after_damage_draws;
    out << ", \"damage_apply_order_mismatches\": "
        << attack_damage_values.damage_apply_order_mismatches;
    out << ", \"simulated_bursts\": " << attack_damage_values.simulated_bursts;
    out << ", \"attack_result_matches\": "
        << attack_damage_values.attack_result_matches;
    out << ", \"attack_result_mismatches\": "
        << attack_damage_values.attack_result_mismatches;
    out << ", \"damage_matches\": " << attack_damage_values.damage_matches;
    out << ", \"damage_mismatches\": " << attack_damage_values.damage_mismatches;
    out << ", \"damage_apply_matches\": " << attack_damage_values.damage_apply_matches;
    out << ", \"damage_apply_mismatches\": "
        << attack_damage_values.damage_apply_mismatches;
    out << ", \"hp_after_matches\": " << attack_damage_values.hp_after_matches;
    out << ", \"hp_after_mismatches\": " << attack_damage_values.hp_after_mismatches;
    out << ", \"lethal_matches\": " << attack_damage_values.lethal_matches;
    out << ", \"lethal_mismatches\": " << attack_damage_values.lethal_mismatches;
    out << ", \"missing_live_field_bursts\": "
        << attack_damage_values.missing_live_field_bursts;
    out << ", \"incomplete_draw_bursts\": "
        << attack_damage_values.incomplete_draw_bursts;
    out << ", \"missing_damage_apply_field_bursts\": "
        << attack_damage_values.missing_damage_apply_field_bursts;
    out << ", \"seed_transition_mismatches\": "
        << attack_damage_values.seed_transition_mismatches;
    out << ", \"first_hit_draw_index\": ";
    write_json_optional_int(out, attack_damage_values.first_hit_draw_index);
    out << ", \"first_damage_value_draw_index\": ";
    write_json_optional_int(out, attack_damage_values.first_damage_value_draw_index);
    out << ", \"attacks\": [";
    for (std::size_t i = 0; i < attack_damage_values.attacks.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& attack = attack_damage_values.attacks[i];
        out << "{\"attack_index\": " << attack.attack_index;
        out << ", \"active_slot\": ";
        write_json_optional_int(out, attack.active_slot);
        out << ", \"target_slot\": ";
        write_json_optional_int(out, attack.target_slot);
        out << ", \"hit_draw_index\": ";
        write_json_optional_int(out, attack.hit_draw_index);
        out << ", \"crit_draw_index\": ";
        write_json_optional_int(out, attack.crit_draw_index);
        out << ", \"damage_spread_draw_index\": ";
        write_json_optional_int(out, attack.damage_spread_draw_index);
        out << ", \"damage_bonus_draw_index\": ";
        write_json_optional_int(out, attack.damage_bonus_draw_index);
        out << ", \"damage_apply_draw_index\": ";
        write_json_optional_int(out, attack.damage_apply_draw_index);
        out << ", \"hit_rand\": ";
        write_json_optional_int(out, attack.hit_rand);
        out << ", \"crit_rand\": ";
        write_json_optional_int(out, attack.crit_rand);
        out << ", \"damage_spread_rand\": ";
        write_json_optional_int(out, attack.damage_spread_rand);
        out << ", \"damage_bonus_rand\": ";
        write_json_optional_int(out, attack.damage_bonus_rand);
        out << ", \"observed_attack_result\": ";
        write_json_optional_int(out, attack.observed_attack_result);
        out << ", \"expected_attack_result\": ";
        write_json_optional_int(out, attack.expected_attack_result);
        out << ", \"observed_damage\": ";
        write_json_optional_int(out, attack.observed_damage);
        out << ", \"expected_damage\": ";
        write_json_optional_int(out, attack.expected_damage);
        out << ", \"damage_apply_damage\": ";
        write_json_optional_int(out, attack.damage_apply_damage);
        out << ", \"hp_before\": ";
        write_json_optional_int(out, attack.hp_before);
        out << ", \"hp_after\": ";
        write_json_optional_int(out, attack.hp_after);
        out << ", \"expected_hp_after\": ";
        write_json_optional_int(out, attack.expected_hp_after);
        out << ", \"lethal\": ";
        write_json_optional_int(out, attack.lethal);
        out << ", \"expected_lethal\": ";
        write_json_optional_int(out, attack.expected_lethal);
        out << ", \"missing_live_input_fields\": "
            << attack.missing_live_input_fields;
        out << ", \"damage_apply_observed\": "
            << (attack.damage_apply_observed ? "true" : "false");
        out << ", \"damage_apply_fields_complete\": "
            << (attack.damage_apply_fields_complete ? "true" : "false");
        out << ", \"damage_apply_after_damage_draws\": "
            << (attack.damage_apply_after_damage_draws ? "true" : "false");
        out << ", \"simulated\": " << (attack.simulated ? "true" : "false");
        out << ", \"attack_result_matches\": "
            << (attack.attack_result_matches ? "true" : "false");
        out << ", \"damage_matches\": " << (attack.damage_matches ? "true" : "false");
        out << ", \"damage_apply_matches\": "
            << (attack.damage_apply_matches ? "true" : "false");
        out << ", \"hp_after_matches\": "
            << (attack.hp_after_matches ? "true" : "false");
        out << ", \"lethal_matches\": "
            << (attack.lethal_matches ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto crit_gate = summarize_crit_gate_checkpoints(result.events);
    out << "  \"crit_gate_checkpoints\": {";
    out << "\"status\": \"" << crit_gate_checkpoint_status_name(crit_gate.status) << "\"";
    out << ", \"rule\": \"" << json_escape(crit_gate_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_hit_draws\": " << crit_gate.observed_hit_draws;
    out << ", \"observed_crit_draws\": " << crit_gate.observed_crit_draws;
    out << ", \"hit_draws_with_instr_param\": "
        << crit_gate.hit_draws_with_instr_param;
    out << ", \"hit_draws_with_hit_success\": "
        << crit_gate.hit_draws_with_hit_success;
    out << ", \"expected_crit_draws_from_live_gate\": ";
    if (crit_gate.expected_crit_draws_from_live_gate.has_value()) {
        out << *crit_gate.expected_crit_draws_from_live_gate;
    } else {
        out << "null";
    }
    out << ", \"first_hit_draw_index\": ";
    if (crit_gate.first_hit_draw_index.has_value()) {
        out << *crit_gate.first_hit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_crit_draw_index\": ";
    if (crit_gate.first_crit_draw_index.has_value()) {
        out << *crit_gate.first_crit_draw_index;
    } else {
        out << "null";
    }
    out << ", \"hit_draws\": [";
    for (std::size_t i = 0; i < crit_gate.hit_draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = crit_gate.hit_draws[i];
        out << "{\"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"active_slot\": ";
        if (draw.active_slot.has_value()) {
            out << *draw.active_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (draw.target_slot.has_value()) {
            out << *draw.target_slot;
        } else {
            out << "null";
        }
        out << ", \"instr_param_0x6\": ";
        if (draw.instr_param_0x6.has_value()) {
            out << *draw.instr_param_0x6;
        } else {
            out << "null";
        }
        out << ", \"hit_success\": ";
        if (draw.hit_success.has_value()) {
            out << *draw.hit_success;
        } else {
            out << "null";
        }
        out << ", \"attack_result\": ";
        if (draw.attack_result.has_value()) {
            out << *draw.attack_result;
        } else {
            out << "null";
        }
        out << ", \"rand_value\": ";
        if (draw.rand_value.has_value()) {
            out << *draw.rand_value;
        } else {
            out << "null";
        }
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto counter = summarize_counter_checkpoints(
        result.events,
        options.expected_counter_roll_ceiling);
    out << "  \"counter_checkpoints\": {";
    out << "\"status\": \"" << counter_checkpoint_status_name(counter.status) << "\"";
    out << ", \"expected_counter_roll_ceiling\": ";
    if (counter.expected_counter_roll_ceiling.has_value()) {
        out << *counter.expected_counter_roll_ceiling;
    } else {
        out << "null";
    }
    out << ", \"observed_counter_rolls\": " << counter.observed_counter_rolls;
    out << ", \"first_counter_roll_draw_index\": ";
    write_json_optional_int(out, counter.first_counter_roll_draw_index);
    out << ", \"last_counter_roll_draw_index\": ";
    write_json_optional_int(out, counter.last_counter_roll_draw_index);
    out << ", \"draws_with_actor_slots\": " << counter.draws_with_actor_slots;
    out << ", \"draws_with_gate_inputs\": " << counter.draws_with_gate_inputs;
    out << ", \"draws_with_rand_value\": " << counter.draws_with_rand_value;
    out << ", \"draws_with_counter_result\": " << counter.draws_with_counter_result;
    out << ", \"draws_with_queue_result\": " << counter.draws_with_queue_result;
    out << ", \"draws_with_counter_chance_update\": "
        << counter.draws_with_counter_chance_update;
    out << ", \"live_gate_simulated_draws\": " << counter.live_gate_simulated_draws;
    out << ", \"observed_counter_gate_attempts\": "
        << counter.observed_counter_gate_attempts;
    out << ", \"gate_attempts_with_live_inputs\": "
        << counter.gate_attempts_with_live_inputs;
    out << ", \"expected_counter_rolls_from_gate_inputs\": "
        << counter.expected_counter_rolls_from_gate_inputs;
    out << ", \"expected_no_draw_gate_attempts\": "
        << counter.expected_no_draw_gate_attempts;
    out << ", \"no_draw_gate_attempts_simulated\": "
        << counter.no_draw_gate_attempts_simulated;
    out << ", \"observed_counter_follow_up_events\": "
        << counter.observed_counter_follow_up_events;
    out << ", \"expected_counter_follow_up_events\": "
        << counter.expected_counter_follow_up_events;
    out << ", \"counter_result_matches\": " << counter.counter_result_matches;
    out << ", \"counter_result_mismatches\": " << counter.counter_result_mismatches;
    out << ", \"queued_field_matches\": " << counter.queued_field_matches;
    out << ", \"queued_field_mismatches\": " << counter.queued_field_mismatches;
    out << ", \"counter_chance_update_matches\": "
        << counter.counter_chance_update_matches;
    out << ", \"counter_chance_update_mismatches\": "
        << counter.counter_chance_update_mismatches;
    out << ", \"seed_transition_mismatches\": "
        << counter.seed_transition_mismatches;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < counter.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = counter.draws[i];
        out << "{\"draw_index\": ";
        write_json_optional_int(out, draw.draw_index);
        out << ", \"kind\": \""
            << counter_checkpoint_kind_name(draw.kind) << "\"";
        out << ", \"attacker_slot\": ";
        write_json_optional_int(out, draw.attacker_slot);
        out << ", \"target_slot\": ";
        write_json_optional_int(out, draw.target_slot);
        out << ", \"target_status_flags\": ";
        write_json_optional_int(out, draw.target_status_flags);
        out << ", \"target_movement_flags\": ";
        write_json_optional_int(out, draw.target_movement_flags);
        out << ", \"target_base_counter_chance\": ";
        write_json_optional_int(out, draw.target_base_counter_chance);
        out << ", \"target_current_counter_chance\": ";
        write_json_optional_int(out, draw.target_current_counter_chance);
        out << ", \"attacker_action_marker\": ";
        write_json_optional_int(out, draw.attacker_action_marker);
        out << ", \"attack_was_critical\": ";
        write_json_optional_int(out, draw.attack_was_critical);
        out << ", \"counter_rand\": ";
        write_json_optional_int(out, draw.counter_rand);
        out << ", \"expected_counter_rolls\": ";
        write_json_optional_int(out, draw.expected_counter_rolls);
        out << ", \"counter_result\": ";
        write_json_optional_int(out, draw.counter_result);
        out << ", \"expected_counter_result\": ";
        write_json_optional_int(out, draw.expected_counter_result);
        out << ", \"expected_counter_follow_up\": ";
        write_json_optional_int(out, draw.expected_counter_follow_up);
        out << ", \"observed_counter_follow_up\": ";
        write_json_optional_int(out, draw.observed_counter_follow_up);
        out << ", \"queued_field7_0xc\": ";
        write_json_optional_int(out, draw.queued_field7_0xc);
        out << ", \"expected_queued_field7_0xc\": ";
        write_json_optional_int(out, draw.expected_queued_field7_0xc);
        out << ", \"updated_current_counter_chance\": ";
        write_json_optional_int(out, draw.updated_current_counter_chance);
        out << ", \"expected_updated_current_counter_chance\": ";
        write_json_optional_int(out, draw.expected_updated_current_counter_chance);
        out << ", \"expected_reason\": ";
        if (draw.expected_reason.has_value()) {
            out << "\"" << counter_result_reason_name(*draw.expected_reason) << "\"";
        } else {
            out << "null";
        }
        out << ", \"live_gate_simulated\": "
            << (draw.live_gate_simulated ? "true" : "false");
        out << ", \"counter_result_matches\": "
            << (draw.counter_result_matches ? "true" : "false");
        out << ", \"queued_field_matches\": "
            << (draw.queued_field_matches ? "true" : "false");
        out << ", \"counter_chance_update_matches\": "
            << (draw.counter_chance_update_matches ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto death_drop = summarize_death_drop_checkpoints(result.events);
    out << "  \"death_drop_checkpoints\": {";
    out << "\"status\": \"" << death_drop_checkpoint_status_name(death_drop.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_death_drop_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_damage_apply_events\": "
        << death_drop.observed_damage_apply_events;
    out << ", \"observed_death_handler_events\": "
        << death_drop.observed_death_handler_events;
    out << ", \"observed_drop_entry_events\": "
        << death_drop.observed_drop_entry_events;
    out << ", \"observed_drop_rolls\": " << death_drop.observed_drop_rolls;
    out << ", \"damage_events_with_live_death_fields\": "
        << death_drop.damage_events_with_live_death_fields;
    out << ", \"lethal_damage_events\": " << death_drop.lethal_damage_events;
    out << ", \"nonlethal_damage_events\": " << death_drop.nonlethal_damage_events;
    out << ", \"damage_events_with_death_handler\": "
        << death_drop.damage_events_with_death_handler;
    out << ", \"lethal_events_with_drop_entry\": "
        << death_drop.lethal_events_with_drop_entry;
    out << ", \"lethal_events_with_drop_roll\": "
        << death_drop.lethal_events_with_drop_roll;
    out << ", \"nonlethal_events_with_unexpected_drop\": "
        << death_drop.nonlethal_events_with_unexpected_drop;
    out << ", \"drop_rolls_after_drop_entry\": "
        << death_drop.drop_rolls_after_drop_entry;
    out << ", \"drop_rolls_before_drop_entry\": "
        << death_drop.drop_rolls_before_drop_entry;
    out << ", \"missing_live_death_field_events\": "
        << death_drop.missing_live_death_field_events;
    out << ", \"missing_death_handler_events\": "
        << death_drop.missing_death_handler_events;
    out << ", \"missing_drop_entry_events\": "
        << death_drop.missing_drop_entry_events;
    out << ", \"missing_drop_roll_events\": "
        << death_drop.missing_drop_roll_events;
    out << ", \"first_damage_apply_draw_index\": ";
    write_json_optional_int(out, death_drop.first_damage_apply_draw_index);
    out << ", \"first_death_handler_draw_index\": ";
    write_json_optional_int(out, death_drop.first_death_handler_draw_index);
    out << ", \"first_drop_entry_draw_index\": ";
    write_json_optional_int(out, death_drop.first_drop_entry_draw_index);
    out << ", \"first_drop_roll_draw_index\": ";
    write_json_optional_int(out, death_drop.first_drop_roll_draw_index);
    out << ", \"damage_flows\": [";
    for (std::size_t i = 0; i < death_drop.damage_flows.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& flow = death_drop.damage_flows[i];
        out << "{\"damage_event_index\": " << flow.damage_event_index;
        out << ", \"target_slot\": ";
        write_json_optional_int(out, flow.target_slot);
        out << ", \"enemy_entry_id\": ";
        write_json_optional_int(out, flow.enemy_entry_id);
        out << ", \"damage\": ";
        write_json_optional_int(out, flow.damage);
        out << ", \"hp_before\": ";
        write_json_optional_int(out, flow.hp_before);
        out << ", \"hp_after\": ";
        write_json_optional_int(out, flow.hp_after);
        out << ", \"lethal\": ";
        write_json_optional_int(out, flow.lethal);
        out << ", \"live_death_fields_complete\": "
            << (flow.live_death_fields_complete ? "true" : "false");
        out << ", \"observed_death_handler\": "
            << (flow.observed_death_handler ? "true" : "false");
        out << ", \"expects_drop_entry\": "
            << (flow.expects_drop_entry ? "true" : "false");
        out << ", \"observed_drop_entry\": "
            << (flow.observed_drop_entry ? "true" : "false");
        out << ", \"observed_drop_roll\": "
            << (flow.observed_drop_roll ? "true" : "false");
        out << ", \"unexpected_drop_for_nonlethal\": "
            << (flow.unexpected_drop_for_nonlethal ? "true" : "false");
        out << ", \"drop_roll_before_drop_entry\": "
            << (flow.drop_roll_before_drop_entry ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto drop = summarize_drop_checkpoints(
        result.events,
        options.expected_drop_rolls);
    out << "  \"drop_checkpoints\": {";
    out << "\"status\": \"" << drop_checkpoint_status_name(drop.status) << "\"";
    out << ", \"expected_drop_rolls\": ";
    if (drop.expected_drop_rolls.has_value()) {
        out << *drop.expected_drop_rolls;
    } else {
        out << "null";
    }
    out << ", \"observed_drop_rolls\": " << drop.observed_drop_rolls;
    out << ", \"first_drop_roll_draw_index\": ";
    if (drop.first_drop_roll_draw_index.has_value()) {
        out << *drop.first_drop_roll_draw_index;
    } else {
        out << "null";
    }
    out << ", \"last_drop_roll_draw_index\": ";
    if (drop.last_drop_roll_draw_index.has_value()) {
        out << *drop.last_drop_roll_draw_index;
    } else {
        out << "null";
    }
    out << ", \"draws_with_target_slot\": " << drop.draws_with_target_slot;
    out << ", \"draws_with_drop_row\": " << drop.draws_with_drop_row;
    out << ", \"draws_with_rand_value\": " << drop.draws_with_rand_value;
    out << ", \"observed_damage_bonus_draws\": " << drop.observed_damage_bonus_draws;
    out << ", \"damage_bonus_draws_before_first_drop\": "
        << drop.damage_bonus_draws_before_first_drop;
    out << ", \"damage_bonus_draws_after_first_drop\": "
        << drop.damage_bonus_draws_after_first_drop;
    out << ", \"observed_counter_rolls\": " << drop.observed_counter_rolls;
    out << ", \"counter_rolls_before_first_drop\": "
        << drop.counter_rolls_before_first_drop;
    out << ", \"observed_action_view_camera_draws\": "
        << drop.observed_action_view_camera_draws;
    out << ", \"action_view_camera_draws_before_first_drop\": "
        << drop.action_view_camera_draws_before_first_drop;
    out << ", \"observed_status_attempt_draws\": "
        << drop.observed_status_attempt_draws;
    out << ", \"status_attempt_draws_before_first_drop\": "
        << drop.status_attempt_draws_before_first_drop;
    out << ", \"draws\": [";
    for (std::size_t i = 0; i < drop.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = drop.draws[i];
        out << "{\"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (draw.target_slot.has_value()) {
            out << *draw.target_slot;
        } else {
            out << "null";
        }
        out << ", \"enemy_entry_id\": ";
        if (draw.enemy_entry_id.has_value()) {
            out << *draw.enemy_entry_id;
        } else {
            out << "null";
        }
        out << ", \"drop_row_index\": ";
        if (draw.drop_row_index.has_value()) {
            out << *draw.drop_row_index;
        } else {
            out << "null";
        }
        out << ", \"drop_item_id\": ";
        if (draw.drop_item_id.has_value()) {
            out << *draw.drop_item_id;
        } else {
            out << "null";
        }
        out << ", \"drop_amount\": ";
        if (draw.drop_amount.has_value()) {
            out << *draw.drop_amount;
        } else {
            out << "null";
        }
        out << ", \"rand_value\": ";
        if (draw.rand_value.has_value()) {
            out << *draw.rand_value;
        } else {
            out << "null";
        }
        out << ", \"rand_mod100\": ";
        if (draw.rand_mod100.has_value()) {
            out << *draw.rand_mod100;
        } else {
            out << "null";
        }
        out << ", \"drop_success\": ";
        if (draw.drop_success.has_value()) {
            out << *draw.drop_success;
        } else {
            out << "null";
        }
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto outcome = summarize_outcome_checkpoints(
        result.events,
        options.expected_end_turn_status_draws,
        options.expected_level_up_stat_rolls);
    out << "  \"outcome_checkpoints\": {";
    out << "\"status\": \"" << outcome_checkpoint_status_name(outcome.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_outcome_checkpoint_rule_detail()) << "\"";
    out << ", \"expected_end_turn_status_draws\": ";
    if (outcome.expected_end_turn_status_draws.has_value()) {
        out << *outcome.expected_end_turn_status_draws;
    } else {
        out << "null";
    }
    out << ", \"expected_level_up_stat_rolls\": ";
    if (outcome.expected_level_up_stat_rolls.has_value()) {
        out << *outcome.expected_level_up_stat_rolls;
    } else {
        out << "null";
    }
    out << ", \"observed_end_turn_status_draws\": "
        << outcome.observed_end_turn_status_draws;
    out << ", \"observed_level_up_stat_rolls\": "
        << outcome.observed_level_up_stat_rolls;
    out << ", \"observed_level_up_roll_1_draws\": "
        << outcome.observed_level_up_roll_1_draws;
    out << ", \"observed_level_up_roll_2_draws\": "
        << outcome.observed_level_up_roll_2_draws;
    out << ", \"observed_level_up_roll_3_draws\": "
        << outcome.observed_level_up_roll_3_draws;
    out << ", \"observed_run_case9_events\": "
        << outcome.observed_run_case9_events;
    out << ", \"observed_battle_success_events\": "
        << outcome.observed_battle_success_events;
    out << ", \"observed_level_up_entries\": "
        << outcome.observed_level_up_entries;
    out << ", \"observed_victory_branch_events\": "
        << outcome.observed_victory_branch_events;
    out << ", \"battle_success_events_with_reward_context\": "
        << outcome.battle_success_events_with_reward_context;
    out << ", \"level_up_entries_with_exp_context\": "
        << outcome.level_up_entries_with_exp_context;
    out << ", \"level_up_entries_with_expected_rolls\": "
        << outcome.level_up_entries_with_expected_rolls;
    out << ", \"expected_level_up_stat_rolls_from_entries\": "
        << outcome.expected_level_up_stat_rolls_from_entries;
    out << ", \"stat_rolls_after_level_up_entry\": "
        << outcome.stat_rolls_after_level_up_entry;
    out << ", \"stat_rolls_before_level_up_entry\": "
        << outcome.stat_rolls_before_level_up_entry;
    out << ", \"first_end_turn_status_draw_index\": ";
    write_json_optional_int(out, outcome.first_end_turn_status_draw_index);
    out << ", \"first_run_case9_draw_index\": ";
    write_json_optional_int(out, outcome.first_run_case9_draw_index);
    out << ", \"first_battle_success_draw_index\": ";
    write_json_optional_int(out, outcome.first_battle_success_draw_index);
    out << ", \"first_level_up_entry_draw_index\": ";
    write_json_optional_int(out, outcome.first_level_up_entry_draw_index);
    out << ", \"first_level_up_stat_roll_index\": ";
    write_json_optional_int(out, outcome.first_level_up_stat_roll_index);
    out << ", \"draws_with_actor_slot\": " << outcome.draws_with_actor_slot;
    out << ", \"draws_with_rand_value\": " << outcome.draws_with_rand_value;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < outcome.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = outcome.draws[i];
        out << "{\"draw_index\": ";
        write_json_optional_int(out, draw.draw_index);
        out << ", \"kind\": \""
            << outcome_checkpoint_kind_name(draw.kind) << "\"";
        out << ", \"owner\": \"" << json_escape(draw.owner) << "\"";
        out << ", \"actor_slot\": ";
        write_json_optional_int(out, draw.actor_slot);
        out << ", \"status_effect_id\": ";
        write_json_optional_int(out, draw.status_effect_id);
        out << ", \"stat_index\": ";
        write_json_optional_int(out, draw.stat_index);
        out << ", \"level\": ";
        write_json_optional_int(out, draw.level);
        out << ", \"level_before\": ";
        write_json_optional_int(out, draw.level_before);
        out << ", \"level_after\": ";
        write_json_optional_int(out, draw.level_after);
        out << ", \"exp_before\": ";
        write_json_optional_int(out, draw.exp_before);
        out << ", \"exp_after\": ";
        write_json_optional_int(out, draw.exp_after);
        out << ", \"next_level_exp\": ";
        write_json_optional_int(out, draw.next_level_exp);
        out << ", \"exp_awarded\": ";
        write_json_optional_int(out, draw.exp_awarded);
        out << ", \"expected_stat_rolls\": ";
        write_json_optional_int(out, draw.expected_stat_rolls);
        out << ", \"battle_outcome\": ";
        write_json_optional_int(out, draw.battle_outcome);
        out << ", \"victory\": ";
        write_json_optional_int(out, draw.victory);
        out << ", \"rand_value\": ";
        write_json_optional_int(out, draw.rand_value);
        out << "}";
    }
    out << "]";
    out << "}\n";
    out << "}\n";
}

} // namespace

const std::map<std::string, std::string>& known_rng_callsite_owners() {
    static const std::map<std::string, std::string> owners = {
        {"8001413C", "pre_ai_battle_start_camera"},
        {"800608DC", "pre_ai_attack_targeting_camera"},
        {"8008B428", "soldier_ai_action_decision"},
        {"8008A0F0", "soldier_ai_random_pc_target"},
        {"8008A618", "soldier_ai_attack_parameter"},
        {"800711F8", "turn_order_priority_jitter"},
        {"8008BC68", "enemy_attack_execution_setup"},
        {"80052BF0", "mode0e_action_view_camera"},
        {"800513D4", "mode0_action_view_camera_fallback"},
        {"80010BDC", "attack_hit_dodge"},
        {"80010C44", "attack_critical"},
        {"80010958", "damage_spread"},
        {"80010984", "damage_low_bit_bonus"},
        {"80010628", "pc_status_attempt"},
        {"80010774", "enemy_status_attempt"},
        {"80081A88", "counter_roll"},
        {"8002BAE8", "enemy_drop_roll"},
        {"8006FF38", "end_turn_status_cleanup"},
        {"801F2B6C", "level_up_stat_roll_1"},
        {"801F2C00", "level_up_stat_roll_2"},
        {"801F2D10", "level_up_stat_roll_3"},
    };
    return owners;
}

CheckpointParseResult parse_checkpoint_stream(std::istream& in) {
    CheckpointParseResult result;
    std::string line;
    int line_number = 0;

    while (std::getline(in, line)) {
        ++line_number;
        line = trim(std::move(line));
        if (line.empty() || line[0] == '#') {
            continue;
        }

        CheckpointEvent event;
        event.line_number = line_number;

        std::istringstream tokens(line);
        std::string token;
        while (tokens >> token) {
            if (token_is_comment(token)) {
                break;
            }
            parse_event_field(event, token, result);
        }

        finalize_event(event, result);
        result.events.push_back(std::move(event));
    }

    add_ordering_warnings(result);
    if (result.events.empty()) {
        result.warnings.push_back("checkpoint file contains no events");
    }
    return result;
}

int run_trace_checkpoints(const TraceCheckpointsOptions& options, std::ostream& out, std::ostream& err) {
    if (options.checkpoint_file.empty()) {
        err << "trace-checkpoints requires --checkpoint-file.\n";
        return 2;
    }

    std::ifstream input(options.checkpoint_file);
    if (!input) {
        err << "Failed to open checkpoint file: " << options.checkpoint_file.string() << "\n";
        return 1;
    }

    auto result = parse_checkpoint_stream(input);
    if (options.json) {
        write_json_report(options, result, out);
    } else {
        write_text_report(options, result, out);
    }
    return result.errors.empty() ? 0 : 1;
}

} // namespace savor::predict
