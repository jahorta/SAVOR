#include "CheckpointTrace.h"

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
