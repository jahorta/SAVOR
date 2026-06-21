#include "CheckpointTrace.h"

#include "ActionViewCameraModel.h"
#include "ActionSourceCheckpointModel.h"
#include "ActionViewGateCheckpointModel.h"
#include "AttackResolutionCheckpointModel.h"
#include "CritGateCheckpointModel.h"
#include "CounterCheckpointModel.h"
#include "DropCheckpointModel.h"
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
    out << "  skipped_fake_attacks_with_short_camera_gap: "
        << pre_ai.skipped_fake_attacks_with_short_camera_gap << "\n";
    out << "  skipped_fake_draws_with_frame_gap: "
        << pre_ai.skipped_fake_draws_with_frame_gap << "\n";
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
    out << "  observed_action_source_events: "
        << action_source.observed_action_source_events << "\n";
    out << "  events_with_actor_slot: " << action_source.events_with_actor_slot << "\n";
    out << "  events_with_source_slot: " << action_source.events_with_source_slot << "\n";
    out << "  events_with_action_id: " << action_source.events_with_action_id << "\n";
    out << "  events_with_handler_pc: " << action_source.events_with_handler_pc << "\n";
    out << "  events_with_source_field6: " << action_source.events_with_source_field6 << "\n";
    out << "  events_with_actor_field6: " << action_source.events_with_actor_field6 << "\n";
    out << "  field6_matches: " << action_source.field6_matches << "\n";
    out << "  field6_mismatches: " << action_source.field6_mismatches << "\n";
    out << "  handler_matches: " << action_source.handler_matches << "\n";
    out << "  handler_mismatches: " << action_source.handler_mismatches << "\n";
    out << "  first_action_source_draw_index: ";
    if (action_source.first_action_source_draw_index.has_value()) {
        out << *action_source.first_action_source_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    if (!action_source.events.empty()) {
        out << "  events:\n";
        for (const auto& event : action_source.events) {
            out << "    draw_index=";
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
    out << "  observed_mode0e_camera_draws: "
        << action_view_gate.observed_mode0e_camera_draws << "\n";
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
    if (!turn_order.draws.empty()) {
        out << "  draws:\n";
        for (const auto& draw : turn_order.draws) {
            out << "    draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " slot=";
            if (draw.slot.has_value()) {
                out << *draw.slot;
            } else {
                out << "unknown";
            }
            out << " quick=";
            if (draw.quick.has_value()) {
                out << *draw.quick;
            } else {
                out << "unknown";
            }
            out << " assigned_priority=";
            if (draw.assigned_priority.has_value()) {
                out << *draw.assigned_priority;
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
    out << "  draws_with_counter_result: " << counter.draws_with_counter_result << "\n";
    if (!counter.draws.empty()) {
        out << "  draws:\n";
        for (const auto& draw : counter.draws) {
            out << "    draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " attacker_slot=";
            if (draw.attacker_slot.has_value()) {
                out << *draw.attacker_slot;
            } else {
                out << "unknown";
            }
            out << " target_slot=";
            if (draw.target_slot.has_value()) {
                out << *draw.target_slot;
            } else {
                out << "unknown";
            }
            out << " cur_counter=";
            if (draw.target_current_counter_chance.has_value()) {
                out << *draw.target_current_counter_chance;
            } else {
                out << "unknown";
            }
            out << " crit=";
            if (draw.attack_was_critical.has_value()) {
                out << *draw.attack_was_critical;
            } else {
                out << "unknown";
            }
            out << " counter_rand=";
            if (draw.counter_rand.has_value()) {
                out << *draw.counter_rand;
            } else {
                out << "unknown";
            }
            out << " counter_result=";
            if (draw.counter_result.has_value()) {
                out << *draw.counter_result;
            } else {
                out << "unknown";
            }
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
    out << "  first_end_turn_status_draw_index: ";
    if (outcome.first_end_turn_status_draw_index.has_value()) {
        out << *outcome.first_end_turn_status_draw_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  first_level_up_stat_roll_index: ";
    if (outcome.first_level_up_stat_roll_index.has_value()) {
        out << *outcome.first_level_up_stat_roll_index << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  draws_with_actor_slot: " << outcome.draws_with_actor_slot << "\n";
    out << "  draws_with_rand_value: " << outcome.draws_with_rand_value << "\n";
    if (!outcome.draws.empty()) {
        out << "  draws:\n";
        for (const auto& draw : outcome.draws) {
            out << "    draw_index=";
            if (draw.draw_index.has_value()) {
                out << *draw.draw_index;
            } else {
                out << "unknown";
            }
            out << " owner=" << draw.owner;
            out << " actor_slot=";
            if (draw.actor_slot.has_value()) {
                out << *draw.actor_slot;
            } else {
                out << "unknown";
            }
            out << " status_effect_id=";
            if (draw.status_effect_id.has_value()) {
                out << *draw.status_effect_id;
            } else {
                out << "unknown";
            }
            out << " stat_index=";
            if (draw.stat_index.has_value()) {
                out << *draw.stat_index;
            } else {
                out << "unknown";
            }
            out << " level=";
            if (draw.level.has_value()) {
                out << *draw.level;
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
    out << ", \"skipped_fake_attacks_with_short_camera_gap\": "
        << pre_ai.skipped_fake_attacks_with_short_camera_gap;
    out << ", \"skipped_fake_draws_with_frame_gap\": "
        << pre_ai.skipped_fake_draws_with_frame_gap;
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
    out << ", \"observed_action_source_events\": "
        << action_source.observed_action_source_events;
    out << ", \"events_with_actor_slot\": " << action_source.events_with_actor_slot;
    out << ", \"events_with_source_slot\": " << action_source.events_with_source_slot;
    out << ", \"events_with_action_id\": " << action_source.events_with_action_id;
    out << ", \"events_with_handler_pc\": " << action_source.events_with_handler_pc;
    out << ", \"events_with_source_field6\": " << action_source.events_with_source_field6;
    out << ", \"events_with_actor_field6\": " << action_source.events_with_actor_field6;
    out << ", \"field6_matches\": " << action_source.field6_matches;
    out << ", \"field6_mismatches\": " << action_source.field6_mismatches;
    out << ", \"handler_matches\": " << action_source.handler_matches;
    out << ", \"handler_mismatches\": " << action_source.handler_mismatches;
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
        out << "{\"draw_index\": ";
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
    out << ", \"observed_mode0e_camera_draws\": "
        << action_view_gate.observed_mode0e_camera_draws;
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
    out << ", \"draws\": [";
    for (std::size_t i = 0; i < turn_order.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = turn_order.draws[i];
        out << "{\"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"slot\": ";
        if (draw.slot.has_value()) {
            out << *draw.slot;
        } else {
            out << "null";
        }
        out << ", \"quick\": ";
        if (draw.quick.has_value()) {
            out << *draw.quick;
        } else {
            out << "null";
        }
        out << ", \"assigned_priority\": ";
        if (draw.assigned_priority.has_value()) {
            out << *draw.assigned_priority;
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
    if (counter.first_counter_roll_draw_index.has_value()) {
        out << *counter.first_counter_roll_draw_index;
    } else {
        out << "null";
    }
    out << ", \"last_counter_roll_draw_index\": ";
    if (counter.last_counter_roll_draw_index.has_value()) {
        out << *counter.last_counter_roll_draw_index;
    } else {
        out << "null";
    }
    out << ", \"draws_with_actor_slots\": " << counter.draws_with_actor_slots;
    out << ", \"draws_with_gate_inputs\": " << counter.draws_with_gate_inputs;
    out << ", \"draws_with_counter_result\": " << counter.draws_with_counter_result;
    out << ", \"draws\": [";
    for (std::size_t i = 0; i < counter.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = counter.draws[i];
        out << "{\"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"attacker_slot\": ";
        if (draw.attacker_slot.has_value()) {
            out << *draw.attacker_slot;
        } else {
            out << "null";
        }
        out << ", \"target_slot\": ";
        if (draw.target_slot.has_value()) {
            out << *draw.target_slot;
        } else {
            out << "null";
        }
        out << ", \"target_status_flags\": ";
        if (draw.target_status_flags.has_value()) {
            out << *draw.target_status_flags;
        } else {
            out << "null";
        }
        out << ", \"target_movement_flags\": ";
        if (draw.target_movement_flags.has_value()) {
            out << *draw.target_movement_flags;
        } else {
            out << "null";
        }
        out << ", \"target_base_counter_chance\": ";
        if (draw.target_base_counter_chance.has_value()) {
            out << *draw.target_base_counter_chance;
        } else {
            out << "null";
        }
        out << ", \"target_current_counter_chance\": ";
        if (draw.target_current_counter_chance.has_value()) {
            out << *draw.target_current_counter_chance;
        } else {
            out << "null";
        }
        out << ", \"attacker_action_marker\": ";
        if (draw.attacker_action_marker.has_value()) {
            out << *draw.attacker_action_marker;
        } else {
            out << "null";
        }
        out << ", \"attack_was_critical\": ";
        if (draw.attack_was_critical.has_value()) {
            out << *draw.attack_was_critical;
        } else {
            out << "null";
        }
        out << ", \"counter_rand\": ";
        if (draw.counter_rand.has_value()) {
            out << *draw.counter_rand;
        } else {
            out << "null";
        }
        out << ", \"counter_result\": ";
        if (draw.counter_result.has_value()) {
            out << *draw.counter_result;
        } else {
            out << "null";
        }
        out << ", \"queued_field7_0xc\": ";
        if (draw.queued_field7_0xc.has_value()) {
            out << *draw.queued_field7_0xc;
        } else {
            out << "null";
        }
        out << ", \"updated_current_counter_chance\": ";
        if (draw.updated_current_counter_chance.has_value()) {
            out << *draw.updated_current_counter_chance;
        } else {
            out << "null";
        }
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
    out << ", \"first_end_turn_status_draw_index\": ";
    if (outcome.first_end_turn_status_draw_index.has_value()) {
        out << *outcome.first_end_turn_status_draw_index;
    } else {
        out << "null";
    }
    out << ", \"first_level_up_stat_roll_index\": ";
    if (outcome.first_level_up_stat_roll_index.has_value()) {
        out << *outcome.first_level_up_stat_roll_index;
    } else {
        out << "null";
    }
    out << ", \"draws_with_actor_slot\": " << outcome.draws_with_actor_slot;
    out << ", \"draws_with_rand_value\": " << outcome.draws_with_rand_value;
    out << ", \"draws\": [";
    for (std::size_t i = 0; i < outcome.draws.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& draw = outcome.draws[i];
        out << "{\"draw_index\": ";
        if (draw.draw_index.has_value()) {
            out << *draw.draw_index;
        } else {
            out << "null";
        }
        out << ", \"owner\": \"" << json_escape(draw.owner) << "\"";
        out << ", \"actor_slot\": ";
        if (draw.actor_slot.has_value()) {
            out << *draw.actor_slot;
        } else {
            out << "null";
        }
        out << ", \"status_effect_id\": ";
        if (draw.status_effect_id.has_value()) {
            out << *draw.status_effect_id;
        } else {
            out << "null";
        }
        out << ", \"stat_index\": ";
        if (draw.stat_index.has_value()) {
            out << *draw.stat_index;
        } else {
            out << "null";
        }
        out << ", \"level\": ";
        if (draw.level.has_value()) {
            out << *draw.level;
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
