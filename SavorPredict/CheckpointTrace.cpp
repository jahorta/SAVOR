#include "CheckpointTrace.h"

#include "ActionViewCameraModel.h"
#include "ActionSetupCheckpointModel.h"
#include "ActionSourceCheckpointModel.h"
#include "ActionViewGateCheckpointModel.h"
#include "ActionViewResourceCheckpointModel.h"
#include "AttackDamageValueCheckpointModel.h"
#include "AttackResolutionCheckpointModel.h"
#include "BattlePredictionDbInput.h"
#include "CliResourceInputCompatibility.h"
#include "CritGateCheckpointModel.h"
#include "CounterCheckpointModel.h"
#include "DropCheckpointModel.h"
#include "DeathDropCheckpointModel.h"
#include "EffectCheckpointModel.h"
#include "Field6WatchpointModel.h"
#include "OutcomeCheckpointModel.h"
#include "PreAiCheckpointModel.h"
#include "SstActionCommandCheckpointModel.h"
#include "TurnOrderCheckpointModel.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <initializer_list>
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
    const long parsed = std::strtol(value.c_str(), &end, 0);
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

bool parse_json_string_token(const std::string& line, std::size_t& index, std::string& out) {
    if (index >= line.size() || line[index] != '"') {
        return false;
    }
    ++index;
    out.clear();
    while (index < line.size()) {
        const char c = line[index++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (index >= line.size()) return false;
            const char esc = line[index++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

void skip_json_ws(const std::string& line, std::size_t& index) {
    while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) != 0) {
        ++index;
    }
}

bool parse_json_value_token(const std::string& line, std::size_t& index, std::string& out) {
    skip_json_ws(line, index);
    if (index >= line.size()) {
        return false;
    }
    if (line[index] == '"') {
        return parse_json_string_token(line, index, out);
    }
    if (line[index] == '[' || line[index] == '{') {
        const char open = line[index];
        const char close = open == '[' ? ']' : '}';
        const auto start = index;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        while (index < line.size()) {
            const char c = line[index++];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == open) {
                ++depth;
            } else if (c == close) {
                --depth;
                if (depth == 0) {
                    out = line.substr(start, index - start);
                    return true;
                }
            }
        }
        return false;
    }
    const auto start = index;
    while (index < line.size() && line[index] != ',' && line[index] != '}') {
        ++index;
    }
    out = trim(line.substr(start, index - start));
    return !out.empty();
}

bool parse_json_event_fields(CheckpointEvent& event, const std::string& line, CheckpointParseResult& result) {
    std::size_t index = 0;
    skip_json_ws(line, index);
    if (index >= line.size() || line[index] != '{') {
        return false;
    }
    ++index;

    for (;;) {
        skip_json_ws(line, index);
        if (index < line.size() && line[index] == '}') {
            ++index;
            skip_json_ws(line, index);
            if (index != line.size()) {
                result.errors.push_back("line " + std::to_string(event.line_number) + ": trailing text after JSON object");
            }
            return true;
        }

        std::string key;
        if (!parse_json_string_token(line, index, key)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": expected JSON object key");
            return true;
        }
        skip_json_ws(line, index);
        if (index >= line.size() || line[index] != ':') {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": expected ':' after JSON object key");
            return true;
        }
        ++index;

        std::string value;
        if (!parse_json_value_token(line, index, value)) {
            result.errors.push_back("line " + std::to_string(event.line_number) + ": expected JSON value for key '" + key + "'");
            return true;
        }
        event.fields[key] = value;

        skip_json_ws(line, index);
        if (index < line.size() && line[index] == ',') {
            ++index;
            continue;
        }
        if (index < line.size() && line[index] == '}') {
            continue;
        }
        result.errors.push_back("line " + std::to_string(event.line_number) + ": expected ',' or '}' in JSON object");
        return true;
    }
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
            if (!parsed) {
                event.known_rng_owner.clear();
            }
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
        if (event.owns_rng_draw && !event.known_rng_owner.empty()) {
            ++counts[event.known_rng_owner];
        }
    }
    return counts;
}

std::optional<int> parse_event_field_int(
    const CheckpointEvent& event,
    const std::string& field_name) {
    const auto found = event.fields.find(field_name);
    if (found == event.fields.end()) {
        return std::nullopt;
    }
    int parsed = 0;
    if (!parse_int_value(found->second, parsed)) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<int> parse_first_event_field_int(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        if (const auto value = parse_event_field_int(event, field_name); value.has_value()) {
            return value;
        }
    }
    return std::nullopt;
}

std::optional<std::string> parse_first_event_field_pc(
    const CheckpointEvent& event,
    std::initializer_list<const char*> field_names) {
    for (const auto* field_name : field_names) {
        const auto found = event.fields.find(field_name);
        if (found != event.fields.end() && !found->second.empty()) {
            return normalize_pc(found->second);
        }
    }
    return std::nullopt;
}

std::optional<int> parse_slot_field_int(
    const CheckpointEvent& event,
    std::optional<int> slot,
    std::string_view suffix) {
    if (!slot.has_value()) {
        return std::nullopt;
    }
    const auto field_name = "slot" + std::to_string(*slot) + std::string(suffix);
    return parse_event_field_int(event, field_name);
}

std::optional<int> actor_slot_from_checkpoint_event(const CheckpointEvent& event) {
    if (event.active_slot.has_value()) {
        return event.active_slot;
    }
    return parse_first_event_field_int(event, {
        "active_slot",
        "actor_slot",
        "actor_slot_arg",
        "attacker_slot",
        "slot",
    });
}

std::optional<int> target_slot_from_checkpoint_event(const CheckpointEvent& event) {
    if (event.target_slot.has_value()) {
        return event.target_slot;
    }
    return parse_first_event_field_int(event, {
        "target_slot",
        "target_slot_arg",
        "target",
    });
}

std::string worker_name_for_checkpoint_pc(const CheckpointEvent& event) {
    if (event.pc == "80086F48") {
        return "PcDirectAttack_80086308";
    }
    if (event.pc == "80085CE0") {
        return "PcFallbackAttack_80085ce0";
    }
    if (event.pc == "8008BDAC") {
        return "EnemyDirectAttack_80087f6c";
    }
    if (event.pc == "8008BDDC") {
        return "EnemyFallbackAttack_80087844";
    }
    if (const auto worker_pc = parse_first_event_field_pc(event, {
            "selected_worker_pc",
            "worker_pc",
            "next_function_pc",
        }); worker_pc.has_value()) {
        if (*worker_pc == "80086308") {
            return "PcDirectAttack_80086308";
        }
        if (*worker_pc == "80085CE0") {
            return "PcFallbackAttack_80085ce0";
        }
        if (*worker_pc == "80087F6C") {
            return "EnemyDirectAttack_80087f6c";
        }
        if (*worker_pc == "80087844") {
            return "EnemyFallbackAttack_80087844";
        }
        return *worker_pc;
    }
    return {};
}

std::optional<int> inferred_param_for_checkpoint_pc(const CheckpointEvent& event) {
    if (event.pc == "80086D18" || event.pc == "80086E4C" || event.pc == "8008BDAC") {
        return 0;
    }
    if (event.pc == "80085608" || event.pc == "800856C4"
        || event.pc == "800856F8" || event.pc == "8008BDDC"
        || event.pc == "80085CE0") {
        return 1;
    }
    return std::nullopt;
}

std::optional<int> instr_param_from_checkpoint_event(
    const CheckpointEvent& event,
    std::optional<int> actor_slot) {
    if (const auto value = parse_first_event_field_int(event, {
            "instr_param_0x6",
            "r0_instr_param_0x6",
            "final_instr_param_0x6",
            "instr_param_after",
            "instr_param",
            "param_0x6",
        }); value.has_value()) {
        return value;
    }
    if (const auto value = parse_slot_field_int(event, actor_slot, "_instr_param_0x6");
        value.has_value()) {
        return value;
    }
    return inferred_param_for_checkpoint_pc(event);
}

bool is_instr_param_setpoint(const CheckpointEvent& event) {
    return event.checkpoint == "instr_param_set"
        || event.checkpoint == "worker_select"
        || event.pc == "80086D18"
        || event.pc == "80085608"
        || event.pc == "800856C4"
        || event.pc == "800856F8"
        || event.pc == "80086E4C"
        || event.pc == "8008A660"
        || event.pc == "8008A678"
        || event.pc == "8008A690"
        || event.pc == "8008BDAC"
        || event.pc == "8008BDDC"
        || event.pc == "80085CE0"
        || event.pc == "80086F48";
}

struct LiveAttackParamSnapshot {
    std::optional<int> actor_slot;
    std::optional<int> target_slot;
    std::optional<int> instr_param_0x6;
    std::optional<std::string> setpoint_pc;
    std::optional<std::string> worker;
};

struct PredictedAttackParamChain {
    int sequence = 0;
    std::optional<int> actor_slot;
    std::optional<int> target_slot;
    std::optional<int> instr_param_0x6;
    std::string worker;
    std::string status;
};

struct AttackParamComparisonChain {
    int chain_index = 0;
    std::optional<int> hit_draw_index;
    std::optional<int> actor_slot;
    std::optional<int> target_slot;
    std::optional<int> live_setpoint_instr_param_0x6;
    std::optional<int> live_consumer_instr_param_0x6;
    std::optional<std::string> live_setpoint_pc;
    std::optional<std::string> live_worker;
    bool crit_draw_observed = false;
    std::optional<int> predicted_sequence;
    std::optional<int> predicted_actor_slot;
    std::optional<int> predicted_target_slot;
    std::optional<int> predicted_instr_param_0x6;
    std::optional<std::string> predicted_worker;
    std::string match_status = "live_only";
};

struct AttackParamComparisonSummary {
    std::vector<AttackParamComparisonChain> chains;
    int predicted_chains = 0;
    int matched_chains = 0;
    int mismatched_chains = 0;
    int unknown_chains = 0;
    std::string predictor_status = "not_requested";
    std::vector<std::string> diagnostics;
};

bool has_selector(const TraceCheckpointsOptions& options) {
    return options.turn_job_id.has_value() || options.exec_job_id.has_value();
}

std::optional<std::uint32_t> first_live_rng_seed_before(
    const std::vector<CheckpointEvent>& events) {
    for (const auto& event : events) {
        if (event.owns_rng_draw && event.rng_seed_before.has_value()) {
            return *event.rng_seed_before;
        }
    }
    return std::nullopt;
}

std::vector<PredictedAttackParamChain> build_predicted_attack_param_chains(
    const TraceCheckpointsOptions& options,
    const std::vector<CheckpointEvent>& events,
    AttackParamComparisonSummary& summary) {
    std::vector<PredictedAttackParamChain> predicted;
    if (!has_selector(options)) {
        summary.predictor_status = "not_requested";
        return predicted;
    }

    BattlePredictionDbInputOptions input_options;
    input_options.db_root = options.db_root;
    input_options.selector.turn_job_id = options.turn_job_id;
    input_options.selector.exec_job_id = options.exec_job_id;
    input_options.resource_inputs = options.resource_inputs;
    input_options.allow_seed_candidate_fallback = true;
    if (const auto live_seed = first_live_rng_seed_before(events); live_seed.has_value()) {
        input_options.start_seed_override = *live_seed;
        summary.diagnostics.push_back(
            "prediction start seed overridden from first live RNG checkpoint");
    }

    std::ostringstream err;
    const auto db_input = build_battle_prediction_input_from_db_root(input_options, err);
    if (!db_input.has_value()) {
        summary.predictor_status = "unavailable";
        const auto text = trim(err.str());
        if (!text.empty()) {
            summary.diagnostics.push_back(text);
        }
        return predicted;
    }

    const auto prediction = predict_battle(db_input->input);
    summary.predictor_status = "available";
    for (const auto& event : prediction.events) {
        if (event.phase != "movement_setup" || event.label != "worker_select") {
            continue;
        }
        predicted.push_back(PredictedAttackParamChain{
            .sequence = event.sequence,
            .actor_slot = event.actor_slot >= 0
                ? std::optional<int>(event.actor_slot)
                : std::nullopt,
            .target_slot = event.target_slot >= 0
                ? std::optional<int>(event.target_slot)
                : std::nullopt,
            .instr_param_0x6 = event.instr_param_0x6,
            .worker = event.movement_worker,
            .status = battle_prediction_event_status_name(event.status),
        });
    }
    summary.predicted_chains = static_cast<int>(predicted.size());
    return predicted;
}

bool next_hit_before_crit(
    const std::vector<CheckpointEvent>& events,
    std::size_t start,
    std::size_t candidate) {
    for (std::size_t i = start + 1; i < candidate; ++i) {
        if (events[i].known_rng_owner == "attack_hit_dodge") {
            return true;
        }
    }
    return false;
}

std::optional<int> consumer_param_after_hit(
    const std::vector<CheckpointEvent>& events,
    std::size_t hit_index,
    std::optional<int> actor_slot) {
    for (std::size_t i = hit_index + 1; i < events.size(); ++i) {
        if (events[i].known_rng_owner == "attack_hit_dodge") {
            return std::nullopt;
        }
        if (events[i].pc != "80010C40" && events[i].checkpoint != "crit_gate") {
            continue;
        }
        return instr_param_from_checkpoint_event(events[i], actor_slot);
    }
    return std::nullopt;
}

bool crit_draw_after_hit(
    const std::vector<CheckpointEvent>& events,
    std::size_t hit_index) {
    for (std::size_t i = hit_index + 1; i < events.size(); ++i) {
        if (events[i].known_rng_owner == "attack_critical") {
            return !next_hit_before_crit(events, hit_index, i);
        }
        if (events[i].known_rng_owner == "attack_hit_dodge") {
            return false;
        }
    }
    return false;
}

std::string compare_attack_param_chain(
    const AttackParamComparisonChain& chain) {
    bool compared = false;
    bool matched = true;
    if (chain.live_consumer_instr_param_0x6.has_value()
        && chain.predicted_instr_param_0x6.has_value()) {
        compared = true;
        matched = matched
            && *chain.live_consumer_instr_param_0x6 == *chain.predicted_instr_param_0x6;
    }
    if (chain.live_worker.has_value() && chain.predicted_worker.has_value()) {
        compared = true;
        matched = matched && *chain.live_worker == *chain.predicted_worker;
    }
    if (chain.actor_slot.has_value() && chain.predicted_actor_slot.has_value()) {
        compared = true;
        matched = matched && *chain.actor_slot == *chain.predicted_actor_slot;
    }
    if (chain.target_slot.has_value() && chain.predicted_target_slot.has_value()) {
        compared = true;
        matched = matched && *chain.target_slot == *chain.predicted_target_slot;
    }
    if (!chain.predicted_sequence.has_value()) {
        return "live_only";
    }
    if (!compared) {
        return "unknown";
    }
    return matched ? "match" : "mismatch";
}

AttackParamComparisonSummary summarize_attack_param_comparison(
    const TraceCheckpointsOptions& options,
    const std::vector<CheckpointEvent>& events) {
    AttackParamComparisonSummary summary;
    const auto predicted = build_predicted_attack_param_chains(options, events, summary);
    std::map<int, LiveAttackParamSnapshot> latest_by_actor;
    std::optional<LiveAttackParamSnapshot> latest_attack_begin;

    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        const auto actor_slot = actor_slot_from_checkpoint_event(event);
        const auto target_slot = target_slot_from_checkpoint_event(event);

        if (event.checkpoint == "attack_begin") {
            LiveAttackParamSnapshot begin;
            begin.actor_slot = actor_slot;
            begin.target_slot = target_slot;
            if (begin.actor_slot.has_value()) {
                begin.instr_param_0x6 =
                    instr_param_from_checkpoint_event(event, begin.actor_slot);
                latest_by_actor[*begin.actor_slot] = begin;
            }
            latest_attack_begin = begin;
            continue;
        }

        if (is_instr_param_setpoint(event)) {
            if (actor_slot.has_value()) {
                auto& snapshot = latest_by_actor[*actor_slot];
                snapshot.actor_slot = actor_slot;
                if (target_slot.has_value()) {
                    snapshot.target_slot = target_slot;
                }
                if (const auto param = instr_param_from_checkpoint_event(event, actor_slot);
                    param.has_value()) {
                    snapshot.instr_param_0x6 = param;
                    snapshot.setpoint_pc = event.pc;
                }
                if (const auto worker = worker_name_for_checkpoint_pc(event); !worker.empty()) {
                    snapshot.worker = worker;
                }
            }
            continue;
        }

        if (event.known_rng_owner != "attack_hit_dodge") {
            continue;
        }

        AttackParamComparisonChain chain;
        chain.chain_index = static_cast<int>(summary.chains.size()) + 1;
        chain.hit_draw_index = event.rng_draw_index_before;
        chain.actor_slot = actor_slot;
        chain.target_slot = target_slot;
        if (!chain.actor_slot.has_value() && latest_attack_begin.has_value()) {
            chain.actor_slot = latest_attack_begin->actor_slot;
        }
        if (!chain.target_slot.has_value() && latest_attack_begin.has_value()) {
            chain.target_slot = latest_attack_begin->target_slot;
        }
        if (chain.actor_slot.has_value()) {
            const auto found = latest_by_actor.find(*chain.actor_slot);
            if (found != latest_by_actor.end()) {
                chain.live_setpoint_instr_param_0x6 = found->second.instr_param_0x6;
                chain.live_setpoint_pc = found->second.setpoint_pc;
                chain.live_worker = found->second.worker;
                if (!chain.target_slot.has_value()) {
                    chain.target_slot = found->second.target_slot;
                }
            }
        }
        chain.live_consumer_instr_param_0x6 =
            instr_param_from_checkpoint_event(event, chain.actor_slot);
        if (!chain.live_consumer_instr_param_0x6.has_value()) {
            chain.live_consumer_instr_param_0x6 =
                consumer_param_after_hit(events, i, chain.actor_slot);
        }
        chain.crit_draw_observed = crit_draw_after_hit(events, i);

        const auto predicted_index = summary.chains.size();
        if (predicted_index < predicted.size()) {
            const auto& expected = predicted[predicted_index];
            chain.predicted_sequence = expected.sequence;
            chain.predicted_actor_slot = expected.actor_slot;
            chain.predicted_target_slot = expected.target_slot;
            chain.predicted_instr_param_0x6 = expected.instr_param_0x6;
            if (!expected.worker.empty()) {
                chain.predicted_worker = expected.worker;
            }
        }
        chain.match_status = compare_attack_param_chain(chain);
        if (chain.match_status == "match") {
            ++summary.matched_chains;
        } else if (chain.match_status == "mismatch") {
            ++summary.mismatched_chains;
        } else if (chain.match_status == "unknown") {
            ++summary.unknown_chains;
        }
        summary.chains.push_back(std::move(chain));
    }

    if (summary.predictor_status == "available"
        && summary.chains.size() < predicted.size()) {
        summary.diagnostics.push_back(
            "prediction has more movement worker_select events than live hit draws");
    }
    return summary;
}

void write_attack_param_comparison_text(
    const AttackParamComparisonSummary& comparison,
    std::ostream& out) {
    out << "\nPredictor/live attack parameter comparison\n";
    out << "  predictor_status: " << comparison.predictor_status << "\n";
    out << "  live_chains: " << comparison.chains.size() << "\n";
    out << "  predicted_chains: " << comparison.predicted_chains << "\n";
    out << "  matched_chains: " << comparison.matched_chains << "\n";
    out << "  mismatched_chains: " << comparison.mismatched_chains << "\n";
    out << "  unknown_chains: " << comparison.unknown_chains << "\n";
    if (!comparison.diagnostics.empty()) {
        out << "  diagnostics:\n";
        for (const auto& diagnostic : comparison.diagnostics) {
            out << "    - " << diagnostic << "\n";
        }
    }
    if (comparison.chains.empty()) {
        out << "  chains: none\n";
        return;
    }
    out << "  chains:\n";
    for (const auto& chain : comparison.chains) {
        out << "    #" << chain.chain_index
            << " status=" << chain.match_status;
        out << " hit_draw=";
        write_optional_int(out, chain.hit_draw_index);
        out << " actor=";
        write_optional_int(out, chain.actor_slot);
        out << " target=";
        write_optional_int(out, chain.target_slot);
        out << " live_setpoint_pc=";
        write_optional_string(out, chain.live_setpoint_pc);
        out << " live_setpoint_instr_param_0x6=";
        write_optional_int(out, chain.live_setpoint_instr_param_0x6);
        out << " live_consumer_instr_param_0x6=";
        write_optional_int(out, chain.live_consumer_instr_param_0x6);
        out << " live_worker=";
        write_optional_string(out, chain.live_worker);
        out << " crit_draw_observed=" << (chain.crit_draw_observed ? "true" : "false");
        out << " predicted_sequence=";
        write_optional_int(out, chain.predicted_sequence);
        out << " predicted_actor=";
        write_optional_int(out, chain.predicted_actor_slot);
        out << " predicted_target=";
        write_optional_int(out, chain.predicted_target_slot);
        out << " predicted_instr_param_0x6=";
        write_optional_int(out, chain.predicted_instr_param_0x6);
        out << " predicted_worker=";
        write_optional_string(out, chain.predicted_worker);
        out << "\n";
    }
}

void write_attack_param_comparison_json(
    const AttackParamComparisonSummary& comparison,
    std::ostream& out) {
    out << "  \"attack_param_comparison\": {";
    out << "\"predictor_status\": \"" << json_escape(comparison.predictor_status) << "\"";
    out << ", \"live_chains\": " << comparison.chains.size();
    out << ", \"predicted_chains\": " << comparison.predicted_chains;
    out << ", \"matched_chains\": " << comparison.matched_chains;
    out << ", \"mismatched_chains\": " << comparison.mismatched_chains;
    out << ", \"unknown_chains\": " << comparison.unknown_chains;
    out << ", \"diagnostics\": [";
    for (std::size_t i = 0; i < comparison.diagnostics.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << "\"" << json_escape(comparison.diagnostics[i]) << "\"";
    }
    out << "]";
    out << ", \"chains\": [";
    for (std::size_t i = 0; i < comparison.chains.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& chain = comparison.chains[i];
        out << "{\"chain_index\": " << chain.chain_index;
        out << ", \"match_status\": \"" << json_escape(chain.match_status) << "\"";
        out << ", \"hit_draw_index\": ";
        write_json_optional_int(out, chain.hit_draw_index);
        out << ", \"actor_slot\": ";
        write_json_optional_int(out, chain.actor_slot);
        out << ", \"target_slot\": ";
        write_json_optional_int(out, chain.target_slot);
        out << ", \"live_setpoint_pc\": ";
        write_json_optional_string(out, chain.live_setpoint_pc);
        out << ", \"live_setpoint_instr_param_0x6\": ";
        write_json_optional_int(out, chain.live_setpoint_instr_param_0x6);
        out << ", \"live_consumer_instr_param_0x6\": ";
        write_json_optional_int(out, chain.live_consumer_instr_param_0x6);
        out << ", \"live_worker\": ";
        write_json_optional_string(out, chain.live_worker);
        out << ", \"crit_draw_observed\": "
            << (chain.crit_draw_observed ? "true" : "false");
        out << ", \"predicted_sequence\": ";
        write_json_optional_int(out, chain.predicted_sequence);
        out << ", \"predicted_actor_slot\": ";
        write_json_optional_int(out, chain.predicted_actor_slot);
        out << ", \"predicted_target_slot\": ";
        write_json_optional_int(out, chain.predicted_target_slot);
        out << ", \"predicted_instr_param_0x6\": ";
        write_json_optional_int(out, chain.predicted_instr_param_0x6);
        out << ", \"predicted_worker\": ";
        write_json_optional_string(out, chain.predicted_worker);
        out << "}";
    }
    out << "]";
    out << "},\n";
}

void write_text_report(
    const TraceCheckpointsOptions& options,
    const CheckpointParseResult& result,
    std::ostream& out) {
    out << "SavorPredict trace-checkpoints\n";
    out << "  checkpoint_file: " << options.checkpoint_file.string() << "\n";
    if (options.resource_inputs != nullptr) {
        out << "  resource_inputs: provider="
            << battle_predictor_resource_provider_kind_name(
                   options.resource_inputs->provider_kind)
            << " status="
            << battle_predictor_resource_input_status_name(
                   options.resource_inputs->status)
            << " bundle_digest="
            << options.resource_inputs->bundle_digest
            << " adapter_version="
            << options.resource_inputs->adapter_version
            << " spice_revision="
            << options.resource_inputs->spice_revision
            << " sources=" << options.resource_inputs->sources.size()
            << "\n";
    } else {
        out << "  resource_inputs: unavailable\n";
    }
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

    const auto effects = summarize_effect_checkpoints(result.events);
    out << "\n8004 effect RNG checkpoints\n";
    out << "  status: " << effect_checkpoint_status_name(effects.status) << "\n";
    out << "  rule: " << first_battle_effect_checkpoint_rule_detail() << "\n";
    out << "  observed_combat_effect_draws: " << effects.observed_combat_effect_draws << "\n";
    out << "  observed_binary_position_draws: "
        << effects.observed_binary_position_draws << "\n";
    out << "  observed_four_way_position_draws: "
        << effects.observed_four_way_position_draws << "\n";
    out << "  observed_scale_x_draws: " << effects.observed_scale_x_draws << "\n";
    out << "  observed_scale_y_draws: " << effects.observed_scale_y_draws << "\n";
    out << "  observed_scale_z_draws: " << effects.observed_scale_z_draws << "\n";
    out << "  observed_variant_index_draws: "
        << effects.observed_variant_index_draws << "\n";
    out << "  observed_axis_assignment_draws: "
        << effects.observed_axis_assignment_draws << "\n";
    out << "  combat_effect_draws_with_loop_count: "
        << effects.combat_effect_draws_with_loop_count << "\n";
    out << "  combat_effect_draws_with_flags: "
        << effects.combat_effect_draws_with_flags << "\n";
    out << "  combat_effect_draws_with_variant_count: "
        << effects.combat_effect_draws_with_variant_count << "\n";
    out << "  combat_effect_draws_with_axis_mode: "
        << effects.combat_effect_draws_with_axis_mode << "\n";
    out << "  combat_effect_draws_with_source_key: "
        << effects.combat_effect_draws_with_source_key << "\n";
    out << "  combat_effect_draws_with_source_subtype: "
        << effects.combat_effect_draws_with_source_subtype << "\n";
    out << "  combat_effect_draws_with_source_secondary: "
        << effects.combat_effect_draws_with_source_secondary << "\n";
    out << "  combat_effect_draws_with_source_resource_id: "
        << effects.combat_effect_draws_with_source_resource_id << "\n";
    out << "  combat_effect_draws_with_buffer_pointer: "
        << effects.combat_effect_draws_with_buffer_pointer << "\n";
    out << "  observed_effect_record_copy_events: "
        << effects.observed_effect_record_copy_events << "\n";
    out << "  effect_record_copy_events_with_effect_buffer: "
        << effects.effect_record_copy_events_with_effect_buffer << "\n";
    out << "  effect_record_copy_events_with_parent_action_thread: "
        << effects.effect_record_copy_events_with_parent_action_thread << "\n";
    out << "  effect_record_copy_events_with_source_key: "
        << effects.effect_record_copy_events_with_source_key << "\n";
    out << "  effect_record_copy_events_with_loop_count: "
        << effects.effect_record_copy_events_with_loop_count << "\n";
    out << "  effect_record_copy_events_matching_source_record_fields: "
        << effects.effect_record_copy_events_matching_source_record_fields << "\n";
    out << "  effect_record_copy_events_matching_combat_effect_buffer: "
        << effects.effect_record_copy_events_matching_combat_effect_buffer << "\n";
    out << "  observed_combat_effect_buffers: "
        << effects.observed_combat_effect_buffers << "\n";
    out << "  complete_binary_variant_iterations: "
        << effects.complete_binary_variant_iterations << "\n";
    out << "  complete_binary_variant_buffers: "
        << effects.complete_binary_variant_buffers << "\n";
    out << "  complete_binary_variant_16_loop_buffers: "
        << effects.complete_binary_variant_16_loop_buffers << "\n";
    out << "  complete_binary_variant_6_loop_buffers: "
        << effects.complete_binary_variant_6_loop_buffers << "\n";
    out << "  complete_binary_variant_4_loop_buffers: "
        << effects.complete_binary_variant_4_loop_buffers << "\n";
    out << "  complete_first_battle_landed_attack_effect_pairs: "
        << effects.complete_first_battle_landed_attack_effect_pairs << "\n";
    out << "  complete_first_battle_16_6_effect_pairs: "
        << effects.complete_first_battle_16_6_effect_pairs << "\n";
    out << "  complete_first_battle_16_4_effect_pairs: "
        << effects.complete_first_battle_16_4_effect_pairs << "\n";
    out << "  complete_first_battle_landed_attack_effect_pair_iterations: "
        << effects.complete_first_battle_landed_attack_effect_pair_iterations << "\n";
    out << "  complete_first_battle_landed_attack_effect_pair_draws: "
        << effects.complete_first_battle_landed_attack_effect_pair_draws << "\n";
    out << "  complete_first_battle_landed_attack_effect_pairs_with_matching_source_key: "
        << effects.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key << "\n";
    out << "  complete_first_battle_landed_attack_effect_pairs_without_matching_source_key: "
        << effects.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key << "\n";
    out << "  complete_first_battle_effect_pairs_by_source_key:\n";
    if (effects.complete_first_battle_effect_pairs_by_source_key.empty()) {
        out << "    none\n";
    } else {
        for (const auto& pair_count : effects.complete_first_battle_effect_pairs_by_source_key) {
            out << "    source_key_" << pair_count.source_key << ": "
                << pair_count.pair_count
                << " pairs loops=" << pair_count.first_loop_count
                << "+" << pair_count.second_loop_count
                << " draws_per_pair=" << pair_count.draw_count << "\n";
        }
    }
    out << "  unpaired_first_battle_effect_buffers: "
        << effects.unpaired_first_battle_effect_buffers << "\n";
    out << "  incomplete_binary_variant_iteration_remainder: "
        << effects.incomplete_binary_variant_iteration_remainder << "\n";
    if (!effects.record_copy_events.empty()) {
        out << "  record_copy_events:\n";
        for (const auto& event : effects.record_copy_events) {
            out << "    draw_index=";
            write_optional_int(out, event.draw_index);
            out << " effect_buffer=";
            write_optional_string(out, event.effect_buffer);
            out << " source_record=";
            write_optional_string(out, event.source_record);
            out << " parent_action_thread=";
            write_optional_string(out, event.parent_action_thread);
            out << " copied_parent_action_thread=";
            write_optional_string(out, event.copied_parent_action_thread);
            out << " source_key=";
            write_optional_int(out, event.source_key);
            out << " source_record_key=";
            write_optional_int(out, event.source_record_key);
            out << " loop_count=";
            write_optional_int(out, event.loop_count);
            out << " source_record_loop_count=";
            write_optional_int(out, event.source_record_loop_count);
            out << " matched_combat_effect_first_draw_index=";
            write_optional_int(out, event.matched_combat_effect_first_draw_index);
            out << "\n";
        }
    }
    out << "  observed_emitter_spawn_draws: "
        << effects.observed_emitter_spawn_draws << "\n";
    out << "  observed_emitter_source_gate_events: "
        << effects.observed_emitter_source_gate_events << "\n";
    out << "  emitter_source_gate_events_with_outer_count: "
        << effects.emitter_source_gate_events_with_outer_count << "\n";
    out << "  emitter_source_gate_events_with_child_count: "
        << effects.emitter_source_gate_events_with_child_count << "\n";
    out << "  emitter_source_gate_events_with_variant_count: "
        << effects.emitter_source_gate_events_with_variant_count << "\n";
    out << "  emitter_source_gate_events_with_axis_mode: "
        << effects.emitter_source_gate_events_with_axis_mode << "\n";
    out << "  observed_particle_tick_draws: "
        << effects.observed_particle_tick_draws << "\n";
    out << "  particle_tick_draws_with_payload_source: "
        << effects.particle_tick_draws_with_payload_source << "\n";
    out << "  particle_tick_draws_with_lifetime: "
        << effects.particle_tick_draws_with_lifetime << "\n";
    out << "  first_combat_effect_draw_index: ";
    write_optional_int(out, effects.first_combat_effect_draw_index);
    out << "\n";
    out << "  last_combat_effect_draw_index: ";
    write_optional_int(out, effects.last_combat_effect_draw_index);
    out << "\n";

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
    out << "  fake_attack_attempt_transitions: "
        << pre_ai.fake_attack_attempt_transitions << "\n";
    out << "  draw_to_skip_fake_attack_transitions: "
        << pre_ai.draw_to_skip_fake_attack_transitions << "\n";
    out << "  skip_to_draw_fake_attack_transitions: "
        << pre_ai.skip_to_draw_fake_attack_transitions << "\n";
    out << "  draw_to_draw_fake_attack_transitions: "
        << pre_ai.draw_to_draw_fake_attack_transitions << "\n";
    out << "  skip_to_skip_fake_attack_transitions: "
        << pre_ai.skip_to_skip_fake_attack_transitions << "\n";
    out << "  draw_to_skip_transitions_with_previous_frame_gap: "
        << pre_ai.draw_to_skip_transitions_with_previous_frame_gap << "\n";
    out << "  skip_to_draw_transitions_with_previous_frame_gap: "
        << pre_ai.skip_to_draw_transitions_with_previous_frame_gap << "\n";
    out << "  min_draw_to_skip_previous_camera_frame_gap: ";
    write_optional_int(out, pre_ai.min_draw_to_skip_previous_camera_frame_gap);
    out << "\n";
    out << "  max_draw_to_skip_previous_camera_frame_gap: ";
    write_optional_int(out, pre_ai.max_draw_to_skip_previous_camera_frame_gap);
    out << "\n";
    out << "  min_skip_to_draw_previous_camera_frame_gap: ";
    write_optional_int(out, pre_ai.min_skip_to_draw_previous_camera_frame_gap);
    out << "\n";
    out << "  max_skip_to_draw_previous_camera_frame_gap: ";
    write_optional_int(out, pre_ai.max_skip_to_draw_previous_camera_frame_gap);
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
    out << "  events_with_action_sequence_id: "
        << action_source.events_with_action_sequence_id << "\n";
    out << "  source_selection_events_with_action_sequence_id: "
        << action_source.source_selection_events_with_action_sequence_id << "\n";
    out << "  action_source_events_with_action_sequence_id: "
        << action_source.action_source_events_with_action_sequence_id << "\n";
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
    out << "  source_selection_bridge_pairs_by_action_sequence_id: "
        << action_source.source_selection_bridge_pairs_by_action_sequence_id << "\n";
    out << "  source_selection_bridge_missing_by_action_sequence_id: "
        << action_source.source_selection_bridge_missing_by_action_sequence_id << "\n";
    out << "  source_selection_bridge_order_matches: "
        << action_source.source_selection_bridge_order_matches << "\n";
    out << "  source_selection_bridge_order_mismatches: "
        << action_source.source_selection_bridge_order_mismatches << "\n";
    out << "  source_selection_bridge_pairing_strategy: "
        << action_source.source_selection_bridge_pairing_strategy << "\n";
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
            out << " action_sequence_id=";
            write_optional_int(out, event.action_sequence_id);
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
            out << " matched_source_selection_draw_index=";
            write_optional_int(out, event.matched_source_selection_draw_index);
            out << " matched_source_selection_source_slot=";
            write_optional_int(out, event.matched_source_selection_source_slot);
            out << " source_selection_before_bridge=";
            write_optional_bool(out, event.source_selection_before_bridge);
            out << " source_slot_matches_selection=";
            write_optional_bool(out, event.source_slot_matches_selection);
            out << "\n";
        }
    }

    const auto sst_action_command = summarize_sst_action_command_checkpoints(result.events);
    out << "\nSST action command field6 checkpoints\n";
    out << "  status: "
        << sst_action_command_checkpoint_status_name(sst_action_command.status) << "\n";
    out << "  rule: " << first_battle_sst_action_command_checkpoint_rule_detail() << "\n";
    out << "  observed_store_events: "
        << sst_action_command.observed_store_events << "\n";
    out << "  observed_case2_store_events: "
        << sst_action_command.observed_case2_store_events << "\n";
    out << "  observed_case8_store_events: "
        << sst_action_command.observed_case8_store_events << "\n";
    out << "  events_with_source_field6: "
        << sst_action_command.events_with_source_field6 << "\n";
    out << "  events_with_destination_field6: "
        << sst_action_command.events_with_destination_field6 << "\n";
    out << "  events_with_written_field6_register: "
        << sst_action_command.events_with_written_field6_register << "\n";
    out << "  events_with_action_sequence_id: "
        << sst_action_command.events_with_action_sequence_id << "\n";
    out << "  source_destination_matches: "
        << sst_action_command.source_destination_matches << "\n";
    out << "  source_destination_mismatches: "
        << sst_action_command.source_destination_mismatches << "\n";
    out << "  key8_store_events: "
        << sst_action_command.key8_store_events << "\n";
    out << "  first_store_draw_index: ";
    write_optional_int(out, sst_action_command.first_store_draw_index);
    out << "\n";
    out << "  first_key8_store_draw_index: ";
    write_optional_int(out, sst_action_command.first_key8_store_draw_index);
    out << "\n";
    if (!sst_action_command.events.empty()) {
        out << "  events:\n";
        for (const auto& event : sst_action_command.events) {
            out << "    kind=" << sst_action_command_checkpoint_kind_name(event.kind);
            out << " draw_index=";
            write_optional_int(out, event.draw_index);
            out << " action_sequence_id=";
            write_optional_int(out, event.action_sequence_id);
            out << " source_field6=";
            write_optional_int(out, event.source_field6);
            out << " destination_field6=";
            write_optional_int(out, event.destination_field6);
            out << " written_field6_register=";
            write_optional_int(out, event.written_field6_register);
            out << " source_matches_destination=";
            write_optional_bool(out, event.source_matches_destination);
            out << "\n";
        }
    }

    const auto field6_watchpoints = summarize_field6_watchpoints(result.events);
    out << "\nField6 access watchpoints\n";
    out << "  status: " << field6_watchpoint_status(field6_watchpoints) << "\n";
    out << "  rule: " << first_battle_field6_watchpoint_rule_detail() << "\n";
    out << "  observed_events: " << field6_watchpoints.observed_events << "\n";
    out << "  confirmed_events: " << field6_watchpoints.confirmed_events << "\n";
    out << "  unattributed_delta_events: "
        << field6_watchpoints.unattributed_delta_events << "\n";
    out << "  confirmed_reads: " << field6_watchpoints.confirmed_reads << "\n";
    out << "  confirmed_writes: " << field6_watchpoints.confirmed_writes << "\n";
    out << "  producer_not_seen_reads: "
        << field6_watchpoints.producer_not_seen_reads << "\n";
    if (!field6_watchpoints.events.empty()) {
        out << "  events:\n";
        for (const auto& event : field6_watchpoints.events) {
            out << "    seq=";
            write_optional_int(out, event.capture_sequence);
            out << " draw_index=";
            write_optional_int(out, event.draw_index);
            out << " pc=" << event.pc;
            out << " label=" << (event.label.empty() ? "unknown" : event.label);
            out << " addr=" << (event.address.empty() ? "unknown" : event.address);
            out << " watch_access=" << (event.watch_access.empty() ? "unknown" : event.watch_access);
            out << " decoded_access=" << (event.decoded_access.empty() ? "unknown" : event.decoded_access);
            out << " mnemonic=" << (event.mnemonic.empty() ? "unknown" : event.mnemonic);
            out << " value=" << (event.value.empty() ? "unknown" : event.value);
            out << " memory_value=" << (event.memory_value.empty() ? "unknown" : event.memory_value);
            out << " confirmed=" << (event.confirmed_current_instruction ? "true" : "false");
            out << " producer_not_seen=" << (event.producer_not_seen ? "true" : "false");
            out << "\n";
        }
    }

    const auto action_view_gate = summarize_action_view_gate_checkpoints(
        result.events,
        ActionViewGateCheckpointOptions{
            .resource_inputs = options.resource_inputs,
        });
    const auto action_view_resource = summarize_action_view_resource_checkpoints(
        result.events,
        action_view_gate);
    out << "\nAction-view STD resource materialization\n";
    out << "  status: "
        << action_view_resource_checkpoint_status_name(action_view_resource.status) << "\n";
    out << "  rule: " << first_battle_action_view_resource_checkpoint_rule_detail() << "\n";
    out << "  observed_cache_producer_materialize_events: "
        << action_view_resource.observed_cache_producer_materialize_events << "\n";
    out << "  observed_cache_producer_table_store_events: "
        << action_view_resource.observed_cache_producer_table_store_events << "\n";
    out << "  observed_cache_producer_key_store_events: "
        << action_view_resource.observed_cache_producer_key_store_events << "\n";
    out << "  complete_cache_producers: "
        << action_view_resource.complete_cache_producers << "\n";
    out << "  observed_cache_lookup_events: "
        << action_view_resource.observed_cache_lookup_events << "\n";
    out << "  observed_cache_table_read_events: "
        << action_view_resource.observed_cache_table_read_events << "\n";
    out << "  observed_cache_result_store_events: "
        << action_view_resource.observed_cache_result_store_events << "\n";
    out << "  complete_cache_hits: "
        << action_view_resource.complete_cache_hits << "\n";
    out << "  selector_aux_roots: "
        << action_view_resource.selector_aux_roots << "\n";
    out << "  selector_aux_roots_linked_to_cache_hits: "
        << action_view_resource.selector_aux_roots_linked_to_cache_hits << "\n";
    out << "  selector_aux_roots_linked_to_cache_producers: "
        << action_view_resource.selector_aux_roots_linked_to_cache_producers << "\n";
    out << "  selector_aux_roots_with_chain_samples: "
        << action_view_resource.selector_aux_roots_with_chain_samples << "\n";
    out << "  selector_aux_roots_with_matching_chain: "
        << action_view_resource.selector_aux_roots_with_matching_chain << "\n";
    out << "  selector_aux_roots_with_mismatching_chain: "
        << action_view_resource.selector_aux_roots_with_mismatching_chain << "\n";
    out << "  selector_aux_roots_with_loaded_resource_root_field_match: "
        << action_view_resource.selector_aux_roots_with_loaded_resource_root_field_match << "\n";
    out << "  selector_aux_roots_with_loaded_resource_root_field_mismatch: "
        << action_view_resource.selector_aux_roots_with_loaded_resource_root_field_mismatch << "\n";
    if (!action_view_resource.selector_root_links.empty()) {
        out << "  selector_root_links:\n";
        for (const auto& link : action_view_resource.selector_root_links) {
            out << "    seq=";
            write_optional_int(out, link.capture_sequence);
            out << " active_slot=";
            write_optional_int(out, link.active_slot);
            out << " target_slot=";
            write_optional_int(out, link.target_slot);
            out << " aux_root=";
            write_optional_string(out, link.aux_list_root);
            out << " chain_payload_0x24=";
            write_optional_string(out, link.chain_payload_0x24);
            out << " chain_loaded_resource_0x10=";
            write_optional_string(out, link.chain_loaded_resource_0x10);
            out << " chain_aux_root_0x30=";
            write_optional_string(out, link.chain_aux_root_0x30);
            out << " chain_matches=";
            write_optional_bool(out, link.chain_aux_root_matches_query);
            out << " cache_key=";
            write_optional_string(out, link.cache_expected_key);
            out << " cache_slot=";
            write_optional_int(out, link.cache_slot);
            out << " root_field_ptr=";
            write_optional_string(out, link.cache_root_field_ptr);
            out << " root_field_matches_loaded_resource_plus_0x30=";
            write_optional_bool(out, link.cache_root_field_matches_loaded_resource_plus_0x30);
            out << " producer_loaded_file_ptr=";
            write_optional_string(out, link.producer_loaded_file_ptr);
            out << " matched_resource_stem=";
            write_optional_string(out, link.matched_resource_stem);
            out << " matched_std0_filename=";
            write_optional_string(out, link.matched_std0_filename);
            out << " linked_to_cache_hit="
                << (link.linked_to_cache_hit ? "true" : "false");
            out << " linked_to_cache_producer="
                << (link.linked_to_cache_producer ? "true" : "false");
            out << "\n";
        }
    }

    out << "\nAction-view gate checkpoints\n";
    out << "  status: " << action_view_gate_checkpoint_status_name(action_view_gate.status) << "\n";
    out << "  rule: " << first_battle_action_view_gate_checkpoint_rule_detail() << "\n";
    out << "  observed_gate_events: " << action_view_gate.observed_gate_events << "\n";
    out << "  legacy_gate_events: " << action_view_gate.legacy_gate_events << "\n";
    out << "  query_call_events: " << action_view_gate.query_call_events << "\n";
    out << "  query_result_events: " << action_view_gate.query_result_events << "\n";
    out << "  query_call_events_with_query_args: "
        << action_view_gate.query_call_events_with_query_args << "\n";
    out << "  query_result_events_with_query_result: "
        << action_view_gate.query_result_events_with_query_result << "\n";
    out << "  observed_dispatch_events: " << action_view_gate.observed_dispatch_events << "\n";
    out << "  dispatch_events_with_payload_mode: "
        << action_view_gate.dispatch_events_with_payload_mode << "\n";
    out << "  dispatch_events_with_effective_mode: "
        << action_view_gate.dispatch_events_with_effective_mode << "\n";
    out << "  dispatch_events_with_spicestd_payload_fields: "
        << action_view_gate.dispatch_events_with_spicestd_payload_fields << "\n";
    out << "  dispatch_serialized_mode0_events: "
        << action_view_gate.dispatch_serialized_mode0_events << "\n";
    out << "  dispatch_effective_mode0_events: "
        << action_view_gate.dispatch_effective_mode0_events << "\n";
    out << "  dispatch_effective_mode0e_events: "
        << action_view_gate.dispatch_effective_mode0e_events << "\n";
    out << "  dispatch_mode0_to_mode0e_rewrites: "
        << action_view_gate.dispatch_mode0_to_mode0e_rewrites << "\n";
    out << "  dispatch_mode0_stays_mode0_events: "
        << action_view_gate.dispatch_mode0_stays_mode0_events << "\n";
    out << "  events_with_aux_list_root: " << action_view_gate.events_with_aux_list_root << "\n";
    out << "  events_with_query_args: " << action_view_gate.events_with_query_args << "\n";
    out << "  events_with_query_result: " << action_view_gate.events_with_query_result << "\n";
    out << "  events_with_selected_record_mode: "
        << action_view_gate.events_with_selected_record_mode << "\n";
    out << "  query_args_match: " << action_view_gate.query_args_match << "\n";
    out << "  query_args_mismatch: " << action_view_gate.query_args_mismatch << "\n";
    out << "  events_with_selector_inputs: "
        << action_view_gate.events_with_selector_inputs << "\n";
    out << "  selector_model_comparisons: "
        << action_view_gate.selector_model_comparisons << "\n";
    out << "  selector_query_args_match: "
        << action_view_gate.selector_query_args_match << "\n";
    out << "  selector_query_args_mismatch: "
        << action_view_gate.selector_query_args_mismatch << "\n";
    out << "  selector_model_missing_expected_query: "
        << action_view_gate.selector_model_missing_expected_query << "\n";
    out << "  observed_helper_call_events: "
        << action_view_gate.observed_helper_call_events << "\n";
    out << "  helper_call_events_with_selector_inputs: "
        << action_view_gate.helper_call_events_with_selector_inputs << "\n";
    out << "  selector_helper_call_comparisons: "
        << action_view_gate.selector_helper_call_comparisons << "\n";
    out << "  selector_helper_call_matches: "
        << action_view_gate.selector_helper_call_matches << "\n";
    out << "  selector_helper_call_mismatches: "
        << action_view_gate.selector_helper_call_mismatches << "\n";
    out << "  selector_helper_call_missing_expected: "
        << action_view_gate.selector_helper_call_missing_expected << "\n";
    out << "  events_with_aux_table_fingerprint: "
        << action_view_gate.events_with_aux_table_fingerprint << "\n";
    out << "  events_with_aux_table_count: "
        << action_view_gate.events_with_aux_table_count << "\n";
    out << "  aux_table_count_matches_query_result: "
        << action_view_gate.aux_table_count_matches_query_result << "\n";
    out << "  aux_table_count_mismatches_query_result: "
        << action_view_gate.aux_table_count_mismatches_query_result << "\n";
    out << "  aux_table_count_missing_query_result: "
        << action_view_gate.aux_table_count_missing_query_result << "\n";
    out << "  aux_table_fingerprint_matches_known_std0: "
        << action_view_gate.aux_table_fingerprint_matches_known_std0 << "\n";
    out << "  aux_table_fingerprint_ambiguous_known_std0: "
        << action_view_gate.aux_table_fingerprint_ambiguous_known_std0 << "\n";
    out << "  aux_table_fingerprint_matches_actor_slot_std0: "
        << action_view_gate.aux_table_fingerprint_matches_actor_slot_std0 << "\n";
    out << "  aux_table_fingerprint_mismatches_actor_slot_std0: "
        << action_view_gate.aux_table_fingerprint_mismatches_actor_slot_std0 << "\n";
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
    out << "  events_with_action_sequence_id: "
        << action_view_gate.events_with_action_sequence_id << "\n";
    out << "  action_sequence_order_comparisons: "
        << action_view_gate.action_sequence_order_comparisons << "\n";
    out << "  action_sequence_order_matches: "
        << action_view_gate.action_sequence_order_matches << "\n";
    out << "  action_sequence_order_mismatches: "
        << action_view_gate.action_sequence_order_mismatches << "\n";
    out << "  action_sequence_order_missing_camera_or_hit: "
        << action_view_gate.action_sequence_order_missing_camera_or_hit << "\n";
    if (!action_view_gate.events.empty()) {
        out << "  events:\n";
        for (const auto& event : action_view_gate.events) {
            out << "    checkpoint=" << event.checkpoint;
            out << " draw_index=";
            if (event.draw_index.has_value()) {
                out << *event.draw_index;
            } else {
                out << "unknown";
            }
            out << " action_sequence_id=";
            write_optional_int(out, event.action_sequence_id);
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
            out << " actor_subtype_0x8=";
            write_optional_int(out, event.actor_subtype_0x8);
            out << " gate_category_0x2f=";
            write_optional_int(out, event.gate_category_0x2f);
            out << " gate_state_0x30=";
            write_optional_int(out, event.gate_state_0x30);
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
            out << " query_result_count=";
            write_optional_int(out, event.query_result_count);
            out << " selector_expected_query=";
            if (event.selector_expected_query.has_value()) {
                out << "("
                    << event.selector_expected_query->action_key << ","
                    << event.selector_expected_query->secondary_key << ","
                    << event.selector_expected_query->location_code << ","
                    << event.selector_expected_query->opcode << ")";
            } else {
                out << "unknown";
            }
            out << " selector_query_args_match=";
            write_optional_bool(out, event.selector_query_args_match);
            out << " sampled_aux_table_rows=" << event.sampled_aux_table_rows;
            out << " sampled_aux_table_includes_sentinel="
                << (event.sampled_aux_table_includes_sentinel ? "true" : "false");
            out << " sampled_aux_table_count=";
            write_optional_int(out, event.sampled_aux_table_count);
            out << " matched_query_result_count=";
            write_optional_int(out, event.matched_query_result_count);
            out << " sampled_aux_table_count_matches_query_result=";
            write_optional_bool(out, event.sampled_aux_table_count_matches_query_result);
            out << " matched_std0_candidate_count=" << event.matched_std0_candidate_count;
            out << " matched_resource_stem=";
            write_optional_string(out, event.matched_resource_stem);
            out << " matched_std_filename=";
            write_optional_string(out, event.matched_std_filename);
            out << " matched_std0_filename=";
            write_optional_string(out, event.matched_std0_filename);
            out << " matched_std0_source_path=";
            write_optional_string(out, event.matched_std0_source_path);
            if (event.matched_std0_json_path.has_value()) {
                out << " matched_std0_json_path=";
                write_optional_string(out, event.matched_std0_json_path);
            }
            out << " matched_std0_materialization_source=";
            write_optional_string(out, event.matched_std0_materialization_source);
            out << " matched_std0_sample_row_offset=";
            write_optional_int(out, event.matched_std0_sample_row_offset);
            out << " actor_slot_expected_std0_filename=";
            write_optional_string(out, event.actor_slot_expected_std0_filename);
            out << " actor_slot_expected_std0_matches_sample=";
            write_optional_bool(out, event.actor_slot_expected_std0_matches_sample);
            out << " actor_slot_expected_std0_sample_row_offset=";
            write_optional_int(out, event.actor_slot_expected_std0_sample_row_offset);
            out << " selected_record_mode=";
            if (event.selected_record_mode.has_value()) {
                out << *event.selected_record_mode;
            } else {
                out << "unknown";
            }
            out << " helper_call_event=" << (event.helper_call_event ? "true" : "false");
            out << " helper_call_site_pc=";
            if (event.helper_call_site_pc.has_value()) {
                out << "0x" << std::hex << std::uppercase << *event.helper_call_site_pc
                    << std::nouppercase << std::dec;
            } else {
                out << "unknown";
            }
            out << " helper_callee=";
            write_optional_string(out, event.helper_callee);
            out << " helper_actor_slot=";
            write_optional_int(out, event.helper_actor_slot);
            out << " helper_mode_arg=";
            write_optional_int(out, event.helper_mode_arg);
            out << " selector_expected_helper_role=";
            write_optional_string(out, event.selector_expected_helper_role);
            out << " selector_expected_helper_callee=";
            write_optional_string(out, event.selector_expected_helper_callee);
            out << " selector_expected_helper_mode_arg=";
            write_optional_int(out, event.selector_expected_helper_mode_arg);
            out << " selector_expected_spawned_record_mode=";
            write_optional_int(out, event.selector_expected_spawned_record_mode);
            out << " selector_helper_call_matches=";
            write_optional_bool(out, event.selector_helper_call_matches);
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
            out << " matched_mode0e_draw_index=";
            write_optional_int(out, event.matched_mode0e_draw_index);
            out << " matched_attack_hit_draw_index=";
            write_optional_int(out, event.matched_attack_hit_draw_index);
            out << " gate_before_mode0e_draw=";
            write_optional_bool(out, event.gate_before_mode0e_draw);
            out << " gate_before_attack_hit_draw=";
            write_optional_bool(out, event.gate_before_attack_hit_draw);
            out << " mode0e_draw_before_attack_hit_draw=";
            write_optional_bool(out, event.mode0e_draw_before_attack_hit_draw);
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
    out << "  qsort_call_count: ";
    if (turn_order.qsort_call_count.has_value()) {
        out << *turn_order.qsort_call_count << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  qsort_element_size: ";
    if (turn_order.qsort_element_size.has_value()) {
        out << *turn_order.qsort_element_size << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  qsort_comparator: ";
    if (turn_order.qsort_comparator.has_value()) {
        out << *turn_order.qsort_comparator << "\n";
    } else {
        out << "unknown\n";
    }
    out << "  events_with_queue_metadata: " << turn_order.events_with_queue_metadata << "\n";
    out << "  queue_metadata_matches: " << turn_order.queue_metadata_matches << "\n";
    out << "  queue_metadata_mismatches: " << turn_order.queue_metadata_mismatches << "\n";
    out << "  observed_queue_entries: " << turn_order.observed_queue_entries << "\n";
    out << "  observed_qsort_output_entries: "
        << turn_order.observed_qsort_output_entries << "\n";
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
    out << "  incomplete_qsort_output_entries: "
        << turn_order.incomplete_qsort_output_entries << "\n";
    out << "  incomplete_execution_order_entries: "
        << turn_order.incomplete_execution_order_entries << "\n";
    out << "  qsort_output_compared: "
        << (turn_order.qsort_output_compared ? "true" : "false") << "\n";
    out << "  qsort_output_exact: "
        << (turn_order.qsort_output_exact ? "true" : "false") << "\n";
    out << "  qsort_output_matches: "
        << turn_order.qsort_output_matches << "\n";
    out << "  qsort_output_mismatches: "
        << turn_order.qsort_output_mismatches << "\n";
    out << "  execution_order_compared: "
        << (turn_order.execution_order_compared ? "true" : "false") << "\n";
    out << "  execution_order_exact: "
        << (turn_order.execution_order_exact ? "true" : "false") << "\n";
    out << "  priority_ties_observed: "
        << (turn_order.priority_ties_observed ? "true" : "false") << "\n";
    out << "  priority_tie_groups: " << turn_order.priority_tie_groups << "\n";
    out << "  priority_tied_entries: " << turn_order.priority_tied_entries << "\n";
    out << "  tie_groups_with_observed_execution_order: "
        << turn_order.tie_groups_with_observed_execution_order << "\n";
    out << "  tie_groups_matching_queue_ascending: "
        << turn_order.tie_groups_matching_queue_ascending << "\n";
    out << "  tie_groups_matching_queue_descending: "
        << turn_order.tie_groups_matching_queue_descending << "\n";
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
    if (!turn_order.expected_qsort_slots.empty()) {
        out << "  expected_qsort_slots:";
        for (const auto slot : turn_order.expected_qsort_slots) {
            out << " " << slot;
        }
        out << "\n";
    }
    if (!turn_order.observed_qsort_slots.empty()) {
        out << "  observed_qsort_slots:";
        for (const auto slot : turn_order.observed_qsort_slots) {
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
    if (!turn_order.tie_groups.empty()) {
        out << "  priority_tie_groups_detail:\n";
        for (const auto& group : turn_order.tie_groups) {
            out << "    assigned_priority=" << group.assigned_priority;
            out << " queue_indices=";
            if (group.queue_indices.empty()) {
                out << "unknown";
            } else {
                for (std::size_t i = 0; i < group.queue_indices.size(); ++i) {
                    if (i != 0) {
                        out << ",";
                    }
                    out << group.queue_indices[i];
                }
            }
            out << " slots_by_queue_order=";
            for (std::size_t i = 0; i < group.slots_by_queue_order.size(); ++i) {
                if (i != 0) {
                    out << ",";
                }
                out << group.slots_by_queue_order[i];
            }
            out << " observed_execution_slots=";
            if (group.observed_execution_slots.empty()) {
                out << "unknown";
            } else {
                for (std::size_t i = 0; i < group.observed_execution_slots.size(); ++i) {
                    if (i != 0) {
                        out << ",";
                    }
                    out << group.observed_execution_slots[i];
                }
            }
            out << " observed_order_compared="
                << (group.observed_order_compared ? "true" : "false");
            out << " observed_order_matches_queue_ascending="
                << (group.observed_order_matches_queue_ascending ? "true" : "false");
            out << " observed_order_matches_queue_descending="
                << (group.observed_order_matches_queue_descending ? "true" : "false");
            out << "\n";
        }
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
    out << "  observed_attack_begin_events: "
        << attack_damage_values.observed_attack_begin_events << "\n";
    out << "  attack_begins_with_actor_slot: "
        << attack_damage_values.attack_begins_with_actor_slot << "\n";
    out << "  attack_begins_with_target_slot: "
        << attack_damage_values.attack_begins_with_target_slot << "\n";
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

    const auto attack_param_comparison =
        summarize_attack_param_comparison(options, result.events);
    write_attack_param_comparison_text(attack_param_comparison, out);

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
    out << "  observed_drop_path_events: "
        << death_drop.observed_drop_path_events << "\n";
    out << "  observed_drop_entry_events: "
        << death_drop.observed_drop_entry_events << "\n";
    out << "  observed_drop_rolls: " << death_drop.observed_drop_rolls << "\n";
    out << "  damage_events_with_live_death_fields: "
        << death_drop.damage_events_with_live_death_fields << "\n";
    out << "  lethal_damage_events: " << death_drop.lethal_damage_events << "\n";
    out << "  nonlethal_damage_events: " << death_drop.nonlethal_damage_events << "\n";
    out << "  damage_events_with_death_handler: "
        << death_drop.damage_events_with_death_handler << "\n";
    out << "  lethal_events_with_drop_path: "
        << death_drop.lethal_events_with_drop_path << "\n";
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
    out << "  missing_drop_path_events: "
        << death_drop.missing_drop_path_events << "\n";
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
    out << "  first_drop_path_draw_index: ";
    write_optional_int(out, death_drop.first_drop_path_draw_index);
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
            out << " drop_path=" << (flow.observed_drop_path ? "true" : "false");
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
    out << "  first_battle_drop_rows_validated: "
        << drop.first_battle_drop_rows_validated << "\n";
    out << "  drop_rolls_with_live_outcome_fields: "
        << drop.drop_rolls_with_live_outcome_fields << "\n";
    out << "  drop_rolls_missing_live_outcome_fields: "
        << drop.drop_rolls_missing_live_outcome_fields << "\n";
    out << "  drop_table_matches: " << drop.drop_table_matches << "\n";
    out << "  drop_table_mismatches: " << drop.drop_table_mismatches << "\n";
    out << "  drop_outcome_matches: " << drop.drop_outcome_matches << "\n";
    out << "  drop_outcome_mismatches: " << drop.drop_outcome_mismatches << "\n";
    out << "  disabled_first_battle_rows_observed: "
        << drop.disabled_first_battle_rows_observed << "\n";
    out << "  successful_drop_rolls: " << drop.successful_drop_rolls << "\n";
    out << "  failed_drop_rolls: " << drop.failed_drop_rolls << "\n";
    out << "  drop_rolls_after_success: " << drop.drop_rolls_after_success << "\n";
    out << "  final_drop_row_index: ";
    write_optional_int(out, drop.final_drop_row_index);
    out << "\n";
    out << "  final_drop_item_id: ";
    write_optional_int(out, drop.final_drop_item_id);
    out << "\n";
    out << "  final_drop_amount: ";
    write_optional_int(out, drop.final_drop_amount);
    out << "\n";
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
            out << " threshold=";
            if (draw.drop_threshold.has_value()) {
                out << *draw.drop_threshold;
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
            out << " expected_drop_item_id=";
            write_optional_int(out, draw.expected_drop_item_id);
            out << " expected_drop_amount=";
            write_optional_int(out, draw.expected_drop_amount);
            out << " expected_drop_threshold=";
            write_optional_int(out, draw.expected_drop_threshold);
            out << " expected_drop_success=";
            write_optional_int(out, draw.expected_drop_success);
            out << " first_battle_row_validated="
                << (draw.first_battle_row_validated ? "true" : "false");
            out << " drop_table_matches="
                << (draw.drop_table_matches ? "true" : "false");
            out << " drop_outcome_matches="
                << (draw.drop_outcome_matches ? "true" : "false");
            out << " roll_after_success="
                << (draw.roll_after_success ? "true" : "false");
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
    out << "  \"schema_version\": 2,\n";
    out << "  \"checkpoint_file\": \"" << json_escape(options.checkpoint_file.string()) << "\",\n";
    out << "  \"resource_inputs\": ";
    if (options.resource_inputs == nullptr) {
        out << "null";
    } else {
        const auto& resources = *options.resource_inputs;
        out << "{"
            << "\"provider_kind\":\""
            << battle_predictor_resource_provider_kind_name(
                   resources.provider_kind)
            << "\",\"status\":\""
            << battle_predictor_resource_input_status_name(resources.status)
            << "\",\"adapter_version\":\""
            << json_escape(resources.adapter_version)
            << "\",\"spice_revision\":\""
            << json_escape(resources.spice_revision)
            << "\",\"bundle_digest\":\""
            << json_escape(resources.bundle_digest)
            << "\",\"sources\":[";
        for (std::size_t source_index = 0;
             source_index < resources.sources.size();
             ++source_index) {
            if (source_index != 0) {
                out << ',';
            }
            const auto& source = resources.sources[source_index];
            out << "{\"logical_role\":\""
                << json_escape(source.logical_role)
                << "\",\"relative_path\":\""
                << json_escape(source.relative_path)
                << "\",\"normalized_relative_path\":\""
                << json_escape(source.normalized_relative_path)
                << "\",\"source_path\":\""
                << json_escape(source.source_path)
                << "\",\"size_bytes\":" << source.size_bytes
                << ",\"sha256\":\""
                << json_escape(source.sha256)
                << "\",\"parser_identity\":\""
                << json_escape(source.parser_identity)
                << "\",\"parser_status\":\""
                << json_escape(source.parser_status) << "\"}";
        }
        out << "],\"diagnostic_count\":"
            << resources.diagnostics.size() << "}";
    }
    out << ",\n";
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

    const auto effects = summarize_effect_checkpoints(result.events);
    out << "  \"effect_checkpoints\": {";
    out << "\"status\": \"" << effect_checkpoint_status_name(effects.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_effect_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_combat_effect_draws\": " << effects.observed_combat_effect_draws;
    out << ", \"observed_binary_position_draws\": "
        << effects.observed_binary_position_draws;
    out << ", \"observed_four_way_position_draws\": "
        << effects.observed_four_way_position_draws;
    out << ", \"observed_scale_x_draws\": " << effects.observed_scale_x_draws;
    out << ", \"observed_scale_y_draws\": " << effects.observed_scale_y_draws;
    out << ", \"observed_scale_z_draws\": " << effects.observed_scale_z_draws;
    out << ", \"observed_variant_index_draws\": "
        << effects.observed_variant_index_draws;
    out << ", \"observed_axis_assignment_draws\": "
        << effects.observed_axis_assignment_draws;
    out << ", \"combat_effect_draws_with_loop_count\": "
        << effects.combat_effect_draws_with_loop_count;
    out << ", \"combat_effect_draws_with_flags\": "
        << effects.combat_effect_draws_with_flags;
    out << ", \"combat_effect_draws_with_variant_count\": "
        << effects.combat_effect_draws_with_variant_count;
    out << ", \"combat_effect_draws_with_axis_mode\": "
        << effects.combat_effect_draws_with_axis_mode;
    out << ", \"combat_effect_draws_with_source_key\": "
        << effects.combat_effect_draws_with_source_key;
    out << ", \"combat_effect_draws_with_source_subtype\": "
        << effects.combat_effect_draws_with_source_subtype;
    out << ", \"combat_effect_draws_with_source_secondary\": "
        << effects.combat_effect_draws_with_source_secondary;
    out << ", \"combat_effect_draws_with_source_resource_id\": "
        << effects.combat_effect_draws_with_source_resource_id;
    out << ", \"combat_effect_draws_with_buffer_pointer\": "
        << effects.combat_effect_draws_with_buffer_pointer;
    out << ", \"observed_effect_record_copy_events\": "
        << effects.observed_effect_record_copy_events;
    out << ", \"effect_record_copy_events_with_effect_buffer\": "
        << effects.effect_record_copy_events_with_effect_buffer;
    out << ", \"effect_record_copy_events_with_parent_action_thread\": "
        << effects.effect_record_copy_events_with_parent_action_thread;
    out << ", \"effect_record_copy_events_with_source_key\": "
        << effects.effect_record_copy_events_with_source_key;
    out << ", \"effect_record_copy_events_with_loop_count\": "
        << effects.effect_record_copy_events_with_loop_count;
    out << ", \"effect_record_copy_events_matching_source_record_fields\": "
        << effects.effect_record_copy_events_matching_source_record_fields;
    out << ", \"effect_record_copy_events_matching_combat_effect_buffer\": "
        << effects.effect_record_copy_events_matching_combat_effect_buffer;
    out << ", \"observed_combat_effect_buffers\": "
        << effects.observed_combat_effect_buffers;
    out << ", \"complete_binary_variant_iterations\": "
        << effects.complete_binary_variant_iterations;
    out << ", \"complete_binary_variant_buffers\": "
        << effects.complete_binary_variant_buffers;
    out << ", \"complete_binary_variant_16_loop_buffers\": "
        << effects.complete_binary_variant_16_loop_buffers;
    out << ", \"complete_binary_variant_6_loop_buffers\": "
        << effects.complete_binary_variant_6_loop_buffers;
    out << ", \"complete_binary_variant_4_loop_buffers\": "
        << effects.complete_binary_variant_4_loop_buffers;
    out << ", \"complete_first_battle_landed_attack_effect_pairs\": "
        << effects.complete_first_battle_landed_attack_effect_pairs;
    out << ", \"complete_first_battle_16_6_effect_pairs\": "
        << effects.complete_first_battle_16_6_effect_pairs;
    out << ", \"complete_first_battle_16_4_effect_pairs\": "
        << effects.complete_first_battle_16_4_effect_pairs;
    out << ", \"complete_first_battle_landed_attack_effect_pair_iterations\": "
        << effects.complete_first_battle_landed_attack_effect_pair_iterations;
    out << ", \"complete_first_battle_landed_attack_effect_pair_draws\": "
        << effects.complete_first_battle_landed_attack_effect_pair_draws;
    out << ", \"complete_first_battle_landed_attack_effect_pairs_with_matching_source_key\": "
        << effects.complete_first_battle_landed_attack_effect_pairs_with_matching_source_key;
    out << ", \"complete_first_battle_landed_attack_effect_pairs_without_matching_source_key\": "
        << effects.complete_first_battle_landed_attack_effect_pairs_without_matching_source_key;
    out << ", \"complete_first_battle_effect_pairs_by_source_key\": [";
    for (std::size_t i = 0; i < effects.complete_first_battle_effect_pairs_by_source_key.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& pair_count = effects.complete_first_battle_effect_pairs_by_source_key[i];
        out << "{\"source_key\": " << pair_count.source_key
            << ", \"first_loop_count\": " << pair_count.first_loop_count
            << ", \"second_loop_count\": " << pair_count.second_loop_count
            << ", \"loop_count_sum\": " << pair_count.loop_count_sum
            << ", \"draw_count\": " << pair_count.draw_count
            << ", \"pair_count\": " << pair_count.pair_count << "}";
    }
    out << "]";
    out << ", \"unpaired_first_battle_effect_buffers\": "
        << effects.unpaired_first_battle_effect_buffers;
    out << ", \"incomplete_binary_variant_iteration_remainder\": "
        << effects.incomplete_binary_variant_iteration_remainder;
    out << ", \"record_copy_events\": [";
    for (std::size_t i = 0; i < effects.record_copy_events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = effects.record_copy_events[i];
        out << "{\"draw_index\": ";
        write_json_optional_int(out, event.draw_index);
        out << ", \"effect_buffer\": ";
        write_json_optional_string(out, event.effect_buffer);
        out << ", \"source_record\": ";
        write_json_optional_string(out, event.source_record);
        out << ", \"parent_action_thread\": ";
        write_json_optional_string(out, event.parent_action_thread);
        out << ", \"copied_parent_action_thread\": ";
        write_json_optional_string(out, event.copied_parent_action_thread);
        out << ", \"source_key\": ";
        write_json_optional_int(out, event.source_key);
        out << ", \"source_record_key\": ";
        write_json_optional_int(out, event.source_record_key);
        out << ", \"loop_count\": ";
        write_json_optional_int(out, event.loop_count);
        out << ", \"source_record_loop_count\": ";
        write_json_optional_int(out, event.source_record_loop_count);
        out << ", \"matched_combat_effect_first_draw_index\": ";
        write_json_optional_int(out, event.matched_combat_effect_first_draw_index);
        out << "}";
    }
    out << "]";
    out << ", \"observed_emitter_spawn_draws\": "
        << effects.observed_emitter_spawn_draws;
    out << ", \"observed_emitter_source_gate_events\": "
        << effects.observed_emitter_source_gate_events;
    out << ", \"emitter_source_gate_events_with_outer_count\": "
        << effects.emitter_source_gate_events_with_outer_count;
    out << ", \"emitter_source_gate_events_with_child_count\": "
        << effects.emitter_source_gate_events_with_child_count;
    out << ", \"emitter_source_gate_events_with_variant_count\": "
        << effects.emitter_source_gate_events_with_variant_count;
    out << ", \"emitter_source_gate_events_with_axis_mode\": "
        << effects.emitter_source_gate_events_with_axis_mode;
    out << ", \"observed_particle_tick_draws\": "
        << effects.observed_particle_tick_draws;
    out << ", \"particle_tick_draws_with_payload_source\": "
        << effects.particle_tick_draws_with_payload_source;
    out << ", \"particle_tick_draws_with_lifetime\": "
        << effects.particle_tick_draws_with_lifetime;
    out << ", \"first_combat_effect_draw_index\": ";
    write_json_optional_int(out, effects.first_combat_effect_draw_index);
    out << ", \"last_combat_effect_draw_index\": ";
    write_json_optional_int(out, effects.last_combat_effect_draw_index);
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
    out << ", \"fake_attack_attempt_transitions\": "
        << pre_ai.fake_attack_attempt_transitions;
    out << ", \"draw_to_skip_fake_attack_transitions\": "
        << pre_ai.draw_to_skip_fake_attack_transitions;
    out << ", \"skip_to_draw_fake_attack_transitions\": "
        << pre_ai.skip_to_draw_fake_attack_transitions;
    out << ", \"draw_to_draw_fake_attack_transitions\": "
        << pre_ai.draw_to_draw_fake_attack_transitions;
    out << ", \"skip_to_skip_fake_attack_transitions\": "
        << pre_ai.skip_to_skip_fake_attack_transitions;
    out << ", \"draw_to_skip_transitions_with_previous_frame_gap\": "
        << pre_ai.draw_to_skip_transitions_with_previous_frame_gap;
    out << ", \"skip_to_draw_transitions_with_previous_frame_gap\": "
        << pre_ai.skip_to_draw_transitions_with_previous_frame_gap;
    out << ", \"min_draw_to_skip_previous_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.min_draw_to_skip_previous_camera_frame_gap);
    out << ", \"max_draw_to_skip_previous_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.max_draw_to_skip_previous_camera_frame_gap);
    out << ", \"min_skip_to_draw_previous_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.min_skip_to_draw_previous_camera_frame_gap);
    out << ", \"max_skip_to_draw_previous_camera_frame_gap\": ";
    write_json_optional_int(out, pre_ai.max_skip_to_draw_previous_camera_frame_gap);
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
    out << ", \"events_with_action_sequence_id\": "
        << action_source.events_with_action_sequence_id;
    out << ", \"source_selection_events_with_action_sequence_id\": "
        << action_source.source_selection_events_with_action_sequence_id;
    out << ", \"action_source_events_with_action_sequence_id\": "
        << action_source.action_source_events_with_action_sequence_id;
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
    out << ", \"source_selection_bridge_pairs_by_action_sequence_id\": "
        << action_source.source_selection_bridge_pairs_by_action_sequence_id;
    out << ", \"source_selection_bridge_missing_by_action_sequence_id\": "
        << action_source.source_selection_bridge_missing_by_action_sequence_id;
    out << ", \"source_selection_bridge_order_matches\": "
        << action_source.source_selection_bridge_order_matches;
    out << ", \"source_selection_bridge_order_mismatches\": "
        << action_source.source_selection_bridge_order_mismatches;
    out << ", \"source_selection_bridge_pairing_strategy\": \""
        << json_escape(action_source.source_selection_bridge_pairing_strategy) << "\"";
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
        out << ", \"action_sequence_id\": ";
        write_json_optional_int(out, event.action_sequence_id);
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
        out << ", \"matched_source_selection_draw_index\": ";
        write_json_optional_int(out, event.matched_source_selection_draw_index);
        out << ", \"matched_source_selection_source_slot\": ";
        write_json_optional_int(out, event.matched_source_selection_source_slot);
        out << ", \"source_selection_before_bridge\": ";
        write_json_optional_bool(out, event.source_selection_before_bridge);
        out << ", \"source_slot_matches_selection\": ";
        write_json_optional_bool(out, event.source_slot_matches_selection);
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto sst_action_command = summarize_sst_action_command_checkpoints(result.events);
    out << "  \"sst_action_command_checkpoints\": {";
    out << "\"status\": \""
        << sst_action_command_checkpoint_status_name(sst_action_command.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_sst_action_command_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_store_events\": "
        << sst_action_command.observed_store_events;
    out << ", \"observed_case2_store_events\": "
        << sst_action_command.observed_case2_store_events;
    out << ", \"observed_case8_store_events\": "
        << sst_action_command.observed_case8_store_events;
    out << ", \"events_with_source_field6\": "
        << sst_action_command.events_with_source_field6;
    out << ", \"events_with_destination_field6\": "
        << sst_action_command.events_with_destination_field6;
    out << ", \"events_with_written_field6_register\": "
        << sst_action_command.events_with_written_field6_register;
    out << ", \"events_with_action_sequence_id\": "
        << sst_action_command.events_with_action_sequence_id;
    out << ", \"source_destination_matches\": "
        << sst_action_command.source_destination_matches;
    out << ", \"source_destination_mismatches\": "
        << sst_action_command.source_destination_mismatches;
    out << ", \"key8_store_events\": "
        << sst_action_command.key8_store_events;
    out << ", \"first_store_draw_index\": ";
    write_json_optional_int(out, sst_action_command.first_store_draw_index);
    out << ", \"first_key8_store_draw_index\": ";
    write_json_optional_int(out, sst_action_command.first_key8_store_draw_index);
    out << ", \"events\": [";
    for (std::size_t i = 0; i < sst_action_command.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = sst_action_command.events[i];
        out << "{\"kind\": \""
            << sst_action_command_checkpoint_kind_name(event.kind) << "\"";
        out << ", \"draw_index\": ";
        write_json_optional_int(out, event.draw_index);
        out << ", \"action_sequence_id\": ";
        write_json_optional_int(out, event.action_sequence_id);
        out << ", \"source_field6\": ";
        write_json_optional_int(out, event.source_field6);
        out << ", \"destination_field6\": ";
        write_json_optional_int(out, event.destination_field6);
        out << ", \"written_field6_register\": ";
        write_json_optional_int(out, event.written_field6_register);
        out << ", \"source_matches_destination\": ";
        write_json_optional_bool(out, event.source_matches_destination);
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto field6_watchpoints = summarize_field6_watchpoints(result.events);
    out << "  \"field6_access_watchpoints\": {";
    out << "\"status\": \"" << field6_watchpoint_status(field6_watchpoints) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_field6_watchpoint_rule_detail()) << "\"";
    out << ", \"observed_events\": " << field6_watchpoints.observed_events;
    out << ", \"confirmed_events\": " << field6_watchpoints.confirmed_events;
    out << ", \"unattributed_delta_events\": "
        << field6_watchpoints.unattributed_delta_events;
    out << ", \"confirmed_reads\": " << field6_watchpoints.confirmed_reads;
    out << ", \"confirmed_writes\": " << field6_watchpoints.confirmed_writes;
    out << ", \"producer_not_seen_reads\": "
        << field6_watchpoints.producer_not_seen_reads;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < field6_watchpoints.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = field6_watchpoints.events[i];
        out << "{\"capture_sequence\": ";
        write_json_optional_int(out, event.capture_sequence);
        out << ", \"draw_index\": ";
        write_json_optional_int(out, event.draw_index);
        out << ", \"pc\": \"" << json_escape(event.pc) << "\"";
        out << ", \"label\": \"" << json_escape(event.label) << "\"";
        out << ", \"address\": \"" << json_escape(event.address) << "\"";
        out << ", \"watch_access\": \"" << json_escape(event.watch_access) << "\"";
        out << ", \"decoded_access\": \"" << json_escape(event.decoded_access) << "\"";
        out << ", \"mnemonic\": \"" << json_escape(event.mnemonic) << "\"";
        out << ", \"value\": \"" << json_escape(event.value) << "\"";
        out << ", \"memory_value\": \"" << json_escape(event.memory_value) << "\"";
        out << ", \"source_pc\": \"" << json_escape(event.source_pc) << "\"";
        out << ", \"confirmed_current_instruction\": "
            << (event.confirmed_current_instruction ? "true" : "false");
        out << ", \"producer_not_seen\": "
            << (event.producer_not_seen ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    const auto action_view_gate = summarize_action_view_gate_checkpoints(
        result.events,
        ActionViewGateCheckpointOptions{
            .resource_inputs = options.resource_inputs,
        });
    const auto action_view_resource = summarize_action_view_resource_checkpoints(
        result.events,
        action_view_gate);
    out << "  \"action_view_resource_checkpoints\": {";
    out << "\"status\": \""
        << action_view_resource_checkpoint_status_name(action_view_resource.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_action_view_resource_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_cache_producer_materialize_events\": "
        << action_view_resource.observed_cache_producer_materialize_events;
    out << ", \"observed_cache_producer_table_store_events\": "
        << action_view_resource.observed_cache_producer_table_store_events;
    out << ", \"observed_cache_producer_key_store_events\": "
        << action_view_resource.observed_cache_producer_key_store_events;
    out << ", \"complete_cache_producers\": "
        << action_view_resource.complete_cache_producers;
    out << ", \"observed_cache_lookup_events\": "
        << action_view_resource.observed_cache_lookup_events;
    out << ", \"observed_cache_table_read_events\": "
        << action_view_resource.observed_cache_table_read_events;
    out << ", \"observed_cache_result_store_events\": "
        << action_view_resource.observed_cache_result_store_events;
    out << ", \"complete_cache_hits\": "
        << action_view_resource.complete_cache_hits;
    out << ", \"selector_aux_roots\": "
        << action_view_resource.selector_aux_roots;
    out << ", \"selector_aux_roots_linked_to_cache_hits\": "
        << action_view_resource.selector_aux_roots_linked_to_cache_hits;
    out << ", \"selector_aux_roots_linked_to_cache_producers\": "
        << action_view_resource.selector_aux_roots_linked_to_cache_producers;
    out << ", \"selector_aux_roots_with_chain_samples\": "
        << action_view_resource.selector_aux_roots_with_chain_samples;
    out << ", \"selector_aux_roots_with_matching_chain\": "
        << action_view_resource.selector_aux_roots_with_matching_chain;
    out << ", \"selector_aux_roots_with_mismatching_chain\": "
        << action_view_resource.selector_aux_roots_with_mismatching_chain;
    out << ", \"selector_aux_roots_with_loaded_resource_root_field_match\": "
        << action_view_resource.selector_aux_roots_with_loaded_resource_root_field_match;
    out << ", \"selector_aux_roots_with_loaded_resource_root_field_mismatch\": "
        << action_view_resource.selector_aux_roots_with_loaded_resource_root_field_mismatch;
    out << ", \"selector_root_links\": [";
    for (std::size_t i = 0; i < action_view_resource.selector_root_links.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& link = action_view_resource.selector_root_links[i];
        out << "{\"capture_sequence\": ";
        write_json_optional_int(out, link.capture_sequence);
        out << ", \"active_slot\": ";
        write_json_optional_int(out, link.active_slot);
        out << ", \"target_slot\": ";
        write_json_optional_int(out, link.target_slot);
        out << ", \"aux_root\": ";
        write_json_optional_string(out, link.aux_list_root);
        out << ", \"chain_payload_0x24\": ";
        write_json_optional_string(out, link.chain_payload_0x24);
        out << ", \"chain_loaded_resource_0x10\": ";
        write_json_optional_string(out, link.chain_loaded_resource_0x10);
        out << ", \"chain_aux_root_0x30\": ";
        write_json_optional_string(out, link.chain_aux_root_0x30);
        out << ", \"chain_matches\": ";
        write_json_optional_bool(out, link.chain_aux_root_matches_query);
        out << ", \"cache_key\": ";
        write_json_optional_string(out, link.cache_expected_key);
        out << ", \"cache_slot\": ";
        write_json_optional_int(out, link.cache_slot);
        out << ", \"root_field_ptr\": ";
        write_json_optional_string(out, link.cache_root_field_ptr);
        out << ", \"root_field_matches_loaded_resource_plus_0x30\": ";
        write_json_optional_bool(out, link.cache_root_field_matches_loaded_resource_plus_0x30);
        out << ", \"producer_loaded_file_ptr\": ";
        write_json_optional_string(out, link.producer_loaded_file_ptr);
        out << ", \"matched_resource_stem\": ";
        write_json_optional_string(out, link.matched_resource_stem);
        out << ", \"matched_std0_filename\": ";
        write_json_optional_string(out, link.matched_std0_filename);
        out << ", \"linked_to_cache_hit\": "
            << (link.linked_to_cache_hit ? "true" : "false");
        out << ", \"linked_to_cache_producer\": "
            << (link.linked_to_cache_producer ? "true" : "false");
        out << "}";
    }
    out << "]";
    out << "},\n";

    out << "  \"action_view_gate_checkpoints\": {";
    out << "\"status\": \"" << action_view_gate_checkpoint_status_name(action_view_gate.status) << "\"";
    out << ", \"rule\": \""
        << json_escape(first_battle_action_view_gate_checkpoint_rule_detail()) << "\"";
    out << ", \"observed_gate_events\": " << action_view_gate.observed_gate_events;
    out << ", \"legacy_gate_events\": " << action_view_gate.legacy_gate_events;
    out << ", \"query_call_events\": " << action_view_gate.query_call_events;
    out << ", \"query_result_events\": " << action_view_gate.query_result_events;
    out << ", \"query_call_events_with_query_args\": "
        << action_view_gate.query_call_events_with_query_args;
    out << ", \"query_result_events_with_query_result\": "
        << action_view_gate.query_result_events_with_query_result;
    out << ", \"observed_dispatch_events\": " << action_view_gate.observed_dispatch_events;
    out << ", \"dispatch_events_with_payload_mode\": "
        << action_view_gate.dispatch_events_with_payload_mode;
    out << ", \"dispatch_events_with_effective_mode\": "
        << action_view_gate.dispatch_events_with_effective_mode;
    out << ", \"dispatch_events_with_spicestd_payload_fields\": "
        << action_view_gate.dispatch_events_with_spicestd_payload_fields;
    out << ", \"dispatch_serialized_mode0_events\": "
        << action_view_gate.dispatch_serialized_mode0_events;
    out << ", \"dispatch_effective_mode0_events\": "
        << action_view_gate.dispatch_effective_mode0_events;
    out << ", \"dispatch_effective_mode0e_events\": "
        << action_view_gate.dispatch_effective_mode0e_events;
    out << ", \"dispatch_mode0_to_mode0e_rewrites\": "
        << action_view_gate.dispatch_mode0_to_mode0e_rewrites;
    out << ", \"dispatch_mode0_stays_mode0_events\": "
        << action_view_gate.dispatch_mode0_stays_mode0_events;
    out << ", \"events_with_aux_list_root\": "
        << action_view_gate.events_with_aux_list_root;
    out << ", \"events_with_query_args\": " << action_view_gate.events_with_query_args;
    out << ", \"events_with_query_result\": " << action_view_gate.events_with_query_result;
    out << ", \"events_with_selected_record_mode\": "
        << action_view_gate.events_with_selected_record_mode;
    out << ", \"query_args_match\": " << action_view_gate.query_args_match;
    out << ", \"query_args_mismatch\": " << action_view_gate.query_args_mismatch;
    out << ", \"events_with_selector_inputs\": "
        << action_view_gate.events_with_selector_inputs;
    out << ", \"selector_model_comparisons\": "
        << action_view_gate.selector_model_comparisons;
    out << ", \"selector_query_args_match\": "
        << action_view_gate.selector_query_args_match;
    out << ", \"selector_query_args_mismatch\": "
        << action_view_gate.selector_query_args_mismatch;
    out << ", \"selector_model_missing_expected_query\": "
        << action_view_gate.selector_model_missing_expected_query;
    out << ", \"observed_helper_call_events\": "
        << action_view_gate.observed_helper_call_events;
    out << ", \"helper_call_events_with_selector_inputs\": "
        << action_view_gate.helper_call_events_with_selector_inputs;
    out << ", \"selector_helper_call_comparisons\": "
        << action_view_gate.selector_helper_call_comparisons;
    out << ", \"selector_helper_call_matches\": "
        << action_view_gate.selector_helper_call_matches;
    out << ", \"selector_helper_call_mismatches\": "
        << action_view_gate.selector_helper_call_mismatches;
    out << ", \"selector_helper_call_missing_expected\": "
        << action_view_gate.selector_helper_call_missing_expected;
    out << ", \"events_with_aux_table_fingerprint\": "
        << action_view_gate.events_with_aux_table_fingerprint;
    out << ", \"events_with_aux_table_count\": "
        << action_view_gate.events_with_aux_table_count;
    out << ", \"aux_table_count_matches_query_result\": "
        << action_view_gate.aux_table_count_matches_query_result;
    out << ", \"aux_table_count_mismatches_query_result\": "
        << action_view_gate.aux_table_count_mismatches_query_result;
    out << ", \"aux_table_count_missing_query_result\": "
        << action_view_gate.aux_table_count_missing_query_result;
    out << ", \"aux_table_fingerprint_matches_known_std0\": "
        << action_view_gate.aux_table_fingerprint_matches_known_std0;
    out << ", \"aux_table_fingerprint_ambiguous_known_std0\": "
        << action_view_gate.aux_table_fingerprint_ambiguous_known_std0;
    out << ", \"aux_table_fingerprint_matches_actor_slot_std0\": "
        << action_view_gate.aux_table_fingerprint_matches_actor_slot_std0;
    out << ", \"aux_table_fingerprint_mismatches_actor_slot_std0\": "
        << action_view_gate.aux_table_fingerprint_mismatches_actor_slot_std0;
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
    out << ", \"events_with_action_sequence_id\": "
        << action_view_gate.events_with_action_sequence_id;
    out << ", \"action_sequence_order_comparisons\": "
        << action_view_gate.action_sequence_order_comparisons;
    out << ", \"action_sequence_order_matches\": "
        << action_view_gate.action_sequence_order_matches;
    out << ", \"action_sequence_order_mismatches\": "
        << action_view_gate.action_sequence_order_mismatches;
    out << ", \"action_sequence_order_missing_camera_or_hit\": "
        << action_view_gate.action_sequence_order_missing_camera_or_hit;
    out << ", \"events\": [";
    for (std::size_t i = 0; i < action_view_gate.events.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& event = action_view_gate.events[i];
        out << "{\"checkpoint\": \"" << json_escape(event.checkpoint) << "\"";
        out << ", \"draw_index\": ";
        if (event.draw_index.has_value()) {
            out << *event.draw_index;
        } else {
            out << "null";
        }
        out << ", \"action_sequence_id\": ";
        write_json_optional_int(out, event.action_sequence_id);
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
        out << ", \"actor_subtype_0x8\": ";
        write_json_optional_int(out, event.actor_subtype_0x8);
        out << ", \"gate_category_0x2f\": ";
        write_json_optional_int(out, event.gate_category_0x2f);
        out << ", \"gate_state_0x30\": ";
        write_json_optional_int(out, event.gate_state_0x30);
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
        out << ", \"query_result_count\": ";
        write_json_optional_int(out, event.query_result_count);
        out << ", \"selector_expected_query\": ";
        if (event.selector_expected_query.has_value()) {
            out << "{\"arg0\": " << event.selector_expected_query->action_key
                << ", \"arg1\": " << event.selector_expected_query->secondary_key
                << ", \"arg2\": " << event.selector_expected_query->location_code
                << ", \"arg3\": " << event.selector_expected_query->opcode
                << "}";
        } else {
            out << "null";
        }
        out << ", \"selector_query_args_match\": ";
        write_json_optional_bool(out, event.selector_query_args_match);
        out << ", \"sampled_aux_table_rows\": " << event.sampled_aux_table_rows;
        out << ", \"sampled_aux_table_includes_sentinel\": "
            << (event.sampled_aux_table_includes_sentinel ? "true" : "false");
        out << ", \"sampled_aux_table_count\": ";
        write_json_optional_int(out, event.sampled_aux_table_count);
        out << ", \"matched_query_result_count\": ";
        write_json_optional_int(out, event.matched_query_result_count);
        out << ", \"sampled_aux_table_count_matches_query_result\": ";
        write_json_optional_bool(out, event.sampled_aux_table_count_matches_query_result);
        out << ", \"matched_std0_candidate_count\": " << event.matched_std0_candidate_count;
        out << ", \"matched_resource_stem\": ";
        write_json_optional_string(out, event.matched_resource_stem);
        out << ", \"matched_std_filename\": ";
        write_json_optional_string(out, event.matched_std_filename);
        out << ", \"matched_std0_filename\": ";
        write_json_optional_string(out, event.matched_std0_filename);
        out << ", \"matched_std0_source_path\": ";
        write_json_optional_string(out, event.matched_std0_source_path);
        if (event.matched_std0_json_path.has_value()) {
            out << ", \"matched_std0_json_path\": ";
            write_json_optional_string(out, event.matched_std0_json_path);
        }
        out << ", \"matched_std0_materialization_source\": ";
        write_json_optional_string(out, event.matched_std0_materialization_source);
        out << ", \"matched_std0_sample_row_offset\": ";
        write_json_optional_int(out, event.matched_std0_sample_row_offset);
        out << ", \"actor_slot_expected_std0_filename\": ";
        write_json_optional_string(out, event.actor_slot_expected_std0_filename);
        out << ", \"actor_slot_expected_std0_matches_sample\": ";
        write_json_optional_bool(out, event.actor_slot_expected_std0_matches_sample);
        out << ", \"actor_slot_expected_std0_sample_row_offset\": ";
        write_json_optional_int(out, event.actor_slot_expected_std0_sample_row_offset);
        out << ", \"selected_record_mode\": ";
        if (event.selected_record_mode.has_value()) {
            out << *event.selected_record_mode;
        } else {
            out << "null";
        }
        out << ", \"helper_call_event\": " << (event.helper_call_event ? "true" : "false");
        out << ", \"helper_call_site_pc\": ";
        if (event.helper_call_site_pc.has_value()) {
            out << "\"0x" << std::hex << std::uppercase << *event.helper_call_site_pc
                << std::nouppercase << std::dec << "\"";
        } else {
            out << "null";
        }
        out << ", \"helper_callee\": ";
        write_json_optional_string(out, event.helper_callee);
        out << ", \"helper_actor_slot\": ";
        write_json_optional_int(out, event.helper_actor_slot);
        out << ", \"helper_mode_arg\": ";
        write_json_optional_int(out, event.helper_mode_arg);
        out << ", \"selector_expected_helper_role\": ";
        write_json_optional_string(out, event.selector_expected_helper_role);
        out << ", \"selector_expected_helper_callee\": ";
        write_json_optional_string(out, event.selector_expected_helper_callee);
        out << ", \"selector_expected_helper_mode_arg\": ";
        write_json_optional_int(out, event.selector_expected_helper_mode_arg);
        out << ", \"selector_expected_spawned_record_mode\": ";
        write_json_optional_int(out, event.selector_expected_spawned_record_mode);
        out << ", \"selector_helper_call_matches\": ";
        write_json_optional_bool(out, event.selector_helper_call_matches);
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
        out << ", \"matched_mode0e_draw_index\": ";
        write_json_optional_int(out, event.matched_mode0e_draw_index);
        out << ", \"matched_attack_hit_draw_index\": ";
        write_json_optional_int(out, event.matched_attack_hit_draw_index);
        out << ", \"gate_before_mode0e_draw\": ";
        write_json_optional_bool(out, event.gate_before_mode0e_draw);
        out << ", \"gate_before_attack_hit_draw\": ";
        write_json_optional_bool(out, event.gate_before_attack_hit_draw);
        out << ", \"mode0e_draw_before_attack_hit_draw\": ";
        write_json_optional_bool(out, event.mode0e_draw_before_attack_hit_draw);
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
    out << ", \"qsort_call_count\": ";
    if (turn_order.qsort_call_count.has_value()) {
        out << *turn_order.qsort_call_count;
    } else {
        out << "null";
    }
    out << ", \"qsort_element_size\": ";
    if (turn_order.qsort_element_size.has_value()) {
        out << *turn_order.qsort_element_size;
    } else {
        out << "null";
    }
    out << ", \"qsort_comparator\": ";
    if (turn_order.qsort_comparator.has_value()) {
        out << *turn_order.qsort_comparator;
    } else {
        out << "null";
    }
    out << ", \"events_with_queue_metadata\": " << turn_order.events_with_queue_metadata;
    out << ", \"queue_metadata_matches\": " << turn_order.queue_metadata_matches;
    out << ", \"queue_metadata_mismatches\": " << turn_order.queue_metadata_mismatches;
    out << ", \"observed_queue_entries\": " << turn_order.observed_queue_entries;
    out << ", \"observed_qsort_output_entries\": "
        << turn_order.observed_qsort_output_entries;
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
    out << ", \"incomplete_qsort_output_entries\": "
        << turn_order.incomplete_qsort_output_entries;
    out << ", \"incomplete_execution_order_entries\": "
        << turn_order.incomplete_execution_order_entries;
    out << ", \"qsort_output_compared\": "
        << (turn_order.qsort_output_compared ? "true" : "false");
    out << ", \"qsort_output_exact\": "
        << (turn_order.qsort_output_exact ? "true" : "false");
    out << ", \"qsort_output_matches\": " << turn_order.qsort_output_matches;
    out << ", \"qsort_output_mismatches\": "
        << turn_order.qsort_output_mismatches;
    out << ", \"execution_order_compared\": "
        << (turn_order.execution_order_compared ? "true" : "false");
    out << ", \"execution_order_exact\": "
        << (turn_order.execution_order_exact ? "true" : "false");
    out << ", \"priority_ties_observed\": "
        << (turn_order.priority_ties_observed ? "true" : "false");
    out << ", \"priority_tie_groups\": " << turn_order.priority_tie_groups;
    out << ", \"priority_tied_entries\": " << turn_order.priority_tied_entries;
    out << ", \"tie_groups_with_observed_execution_order\": "
        << turn_order.tie_groups_with_observed_execution_order;
    out << ", \"tie_groups_matching_queue_ascending\": "
        << turn_order.tie_groups_matching_queue_ascending;
    out << ", \"tie_groups_matching_queue_descending\": "
        << turn_order.tie_groups_matching_queue_descending;
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
    out << ", \"expected_qsort_slots\": [";
    for (std::size_t i = 0; i < turn_order.expected_qsort_slots.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << turn_order.expected_qsort_slots[i];
    }
    out << "]";
    out << ", \"observed_qsort_slots\": [";
    for (std::size_t i = 0; i < turn_order.observed_qsort_slots.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        out << turn_order.observed_qsort_slots[i];
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
    out << ", \"tie_groups\": [";
    for (std::size_t i = 0; i < turn_order.tie_groups.size(); ++i) {
        if (i != 0) {
            out << ", ";
        }
        const auto& group = turn_order.tie_groups[i];
        out << "{\"assigned_priority\": " << group.assigned_priority;
        out << ", \"queue_indices\": [";
        for (std::size_t j = 0; j < group.queue_indices.size(); ++j) {
            if (j != 0) {
                out << ", ";
            }
            out << group.queue_indices[j];
        }
        out << "]";
        out << ", \"slots_by_queue_order\": [";
        for (std::size_t j = 0; j < group.slots_by_queue_order.size(); ++j) {
            if (j != 0) {
                out << ", ";
            }
            out << group.slots_by_queue_order[j];
        }
        out << "]";
        out << ", \"observed_execution_slots\": [";
        for (std::size_t j = 0; j < group.observed_execution_slots.size(); ++j) {
            if (j != 0) {
                out << ", ";
            }
            out << group.observed_execution_slots[j];
        }
        out << "]";
        out << ", \"observed_order_compared\": "
            << (group.observed_order_compared ? "true" : "false");
        out << ", \"observed_order_matches_queue_ascending\": "
            << (group.observed_order_matches_queue_ascending ? "true" : "false");
        out << ", \"observed_order_matches_queue_descending\": "
            << (group.observed_order_matches_queue_descending ? "true" : "false");
        out << "}";
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
    out << ", \"observed_attack_begin_events\": "
        << attack_damage_values.observed_attack_begin_events;
    out << ", \"attack_begins_with_actor_slot\": "
        << attack_damage_values.attack_begins_with_actor_slot;
    out << ", \"attack_begins_with_target_slot\": "
        << attack_damage_values.attack_begins_with_target_slot;
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

    const auto attack_param_comparison =
        summarize_attack_param_comparison(options, result.events);
    write_attack_param_comparison_json(attack_param_comparison, out);

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
    out << ", \"observed_drop_path_events\": "
        << death_drop.observed_drop_path_events;
    out << ", \"observed_drop_entry_events\": "
        << death_drop.observed_drop_entry_events;
    out << ", \"observed_drop_rolls\": " << death_drop.observed_drop_rolls;
    out << ", \"damage_events_with_live_death_fields\": "
        << death_drop.damage_events_with_live_death_fields;
    out << ", \"lethal_damage_events\": " << death_drop.lethal_damage_events;
    out << ", \"nonlethal_damage_events\": " << death_drop.nonlethal_damage_events;
    out << ", \"damage_events_with_death_handler\": "
        << death_drop.damage_events_with_death_handler;
    out << ", \"lethal_events_with_drop_path\": "
        << death_drop.lethal_events_with_drop_path;
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
    out << ", \"missing_drop_path_events\": "
        << death_drop.missing_drop_path_events;
    out << ", \"missing_drop_entry_events\": "
        << death_drop.missing_drop_entry_events;
    out << ", \"missing_drop_roll_events\": "
        << death_drop.missing_drop_roll_events;
    out << ", \"first_damage_apply_draw_index\": ";
    write_json_optional_int(out, death_drop.first_damage_apply_draw_index);
    out << ", \"first_death_handler_draw_index\": ";
    write_json_optional_int(out, death_drop.first_death_handler_draw_index);
    out << ", \"first_drop_path_draw_index\": ";
    write_json_optional_int(out, death_drop.first_drop_path_draw_index);
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
        out << ", \"expects_drop_path\": "
            << (flow.expects_drop_path ? "true" : "false");
        out << ", \"observed_drop_path\": "
            << (flow.observed_drop_path ? "true" : "false");
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
    out << ", \"first_battle_drop_rows_validated\": "
        << drop.first_battle_drop_rows_validated;
    out << ", \"drop_rolls_with_live_outcome_fields\": "
        << drop.drop_rolls_with_live_outcome_fields;
    out << ", \"drop_rolls_missing_live_outcome_fields\": "
        << drop.drop_rolls_missing_live_outcome_fields;
    out << ", \"drop_table_matches\": " << drop.drop_table_matches;
    out << ", \"drop_table_mismatches\": " << drop.drop_table_mismatches;
    out << ", \"drop_outcome_matches\": " << drop.drop_outcome_matches;
    out << ", \"drop_outcome_mismatches\": " << drop.drop_outcome_mismatches;
    out << ", \"disabled_first_battle_rows_observed\": "
        << drop.disabled_first_battle_rows_observed;
    out << ", \"successful_drop_rolls\": " << drop.successful_drop_rolls;
    out << ", \"failed_drop_rolls\": " << drop.failed_drop_rolls;
    out << ", \"drop_rolls_after_success\": " << drop.drop_rolls_after_success;
    out << ", \"final_drop_row_index\": ";
    write_json_optional_int(out, drop.final_drop_row_index);
    out << ", \"final_drop_item_id\": ";
    write_json_optional_int(out, drop.final_drop_item_id);
    out << ", \"final_drop_amount\": ";
    write_json_optional_int(out, drop.final_drop_amount);
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
        out << ", \"drop_threshold\": ";
        if (draw.drop_threshold.has_value()) {
            out << *draw.drop_threshold;
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
        out << ", \"expected_drop_item_id\": ";
        write_json_optional_int(out, draw.expected_drop_item_id);
        out << ", \"expected_drop_amount\": ";
        write_json_optional_int(out, draw.expected_drop_amount);
        out << ", \"expected_drop_threshold\": ";
        write_json_optional_int(out, draw.expected_drop_threshold);
        out << ", \"expected_drop_success\": ";
        write_json_optional_int(out, draw.expected_drop_success);
        out << ", \"first_battle_row_validated\": "
            << (draw.first_battle_row_validated ? "true" : "false");
        out << ", \"drop_table_matches\": "
            << (draw.drop_table_matches ? "true" : "false");
        out << ", \"drop_outcome_matches\": "
            << (draw.drop_outcome_matches ? "true" : "false");
        out << ", \"roll_after_success\": "
            << (draw.roll_after_success ? "true" : "false");
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

TraceCheckpointsResourceCliParseResult
parse_trace_checkpoints_resource_input_tokens(
    const std::vector<std::string>& args) {
    TraceCheckpointsResourceCliParseResult result;
    cli_detail::DiscDumpRootOptions disc_dump_root_options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto& option = args[index];
        if (option != "--disc-dump-root"
            && option != "--std-disc-dump-root"
            && option != "--spice-file-parsing-exe") {
            continue;
        }
        if (index + 1 >= args.size()) {
            result.errors.push_back(option + " requires a value.");
            continue;
        }
        const auto& value = args[++index];
        if (option == "--spice-file-parsing-exe") {
            result.spice_file_parsing_exe = value;
            cli_detail::add_spice_file_parsing_exe_warning(
                result.warnings);
        } else {
            disc_dump_root_options.observe(
                option, value, result.errors);
        }
    }
    disc_dump_root_options.finalize(
        result.disc_dump_root,
        result.errors,
        result.warnings);
    return result;
}

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
        {"8002BAD8", "enemy_drop_roll"},
        {"8006FF38", "end_turn_status_cleanup"},
        {"801F2B6C", "level_up_stat_roll_1"},
        {"801F2C00", "level_up_stat_roll_2"},
        {"801F2D10", "level_up_stat_roll_3"},
        {"80041F3C", "effect_emitter_spawn_variant"},
        {"80041F60", "effect_emitter_spawn_offset_x"},
        {"80041F88", "effect_emitter_spawn_offset_y"},
        {"80041FB0", "effect_emitter_spawn_scale_a"},
        {"80041FCC", "effect_emitter_spawn_scale_b"},
        {"80041FE8", "effect_emitter_spawn_scale_c"},
        {"80042020", "effect_emitter_axis_variant"},
        {"800425A0", "effect_particle_motion_gate_x"},
        {"800425E0", "effect_particle_motion_offset_x"},
        {"80042630", "effect_particle_motion_gate_z"},
        {"80042670", "effect_particle_motion_offset_z"},
        {"80042F3C", "combat_effect_spawn_position_four_way"},
        {"80042FBC", "combat_effect_spawn_position_binary"},
        {"80043020", "combat_effect_spawn_scale_x"},
        {"80043048", "combat_effect_spawn_scale_y"},
        {"80043070", "combat_effect_spawn_scale_z"},
        {"800430FC", "combat_effect_spawn_variant_index"},
        {"80043200", "combat_effect_spawn_axis_assignment"},
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

        if (!parse_json_event_fields(event, line, result)) {
            std::istringstream tokens(line);
            std::string token;
            while (tokens >> token) {
                if (token_is_comment(token)) {
                    break;
                }
                parse_event_field(event, token, result);
            }
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

    auto resolved_options = options;
    if (!cli_detail::ensure_first_battle_resource_inputs(
            resolved_options.disc_dump_root,
            resolved_options.action_view_std_json_dir,
            resolved_options.resource_inputs,
            err)) {
        return 1;
    }

    std::ifstream input(resolved_options.checkpoint_file);
    if (!input) {
        err << "Failed to open checkpoint file: " << resolved_options.checkpoint_file.string() << "\n";
        return 1;
    }

    auto result = parse_checkpoint_stream(input);
    if (resolved_options.json) {
        write_json_report(resolved_options, result, out);
    } else {
        write_text_report(resolved_options, result, out);
    }
    return result.errors.empty() ? 0 : 1;
}

} // namespace savor::predict
