#include "BattlePredictorCli.h"

#include "BattlePredictionDbInput.h"
#include "BattleJobRunOptions.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>

namespace savor::predict {
namespace {

bool parse_ll(std::string_view value, long long& out) {
    std::string owned(value);
    char* end = nullptr;
    out = std::strtoll(owned.c_str(), &end, 10);
    return end != owned.c_str() && *end == '\0';
}

bool parse_int(std::string_view value, int& out) {
    std::string owned(value);
    char* end = nullptr;
    const long parsed = std::strtol(owned.c_str(), &end, 10);
    if (end == owned.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

bool parse_u32_seed(std::string_view value, std::uint32_t& out) {
    std::string normalized(value);
    int base = 10;
    if (normalized.rfind("0x", 0) == 0 || normalized.rfind("0X", 0) == 0) {
        normalized.erase(0, 2);
        base = 16;
    }
    if (normalized.empty()) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(normalized.c_str(), &end, base);
    if (end == normalized.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<std::uint32_t>(parsed);
    return parsed <= 0xFFFFFFFFul;
}

bool require_value(
    const std::vector<std::string>& args,
    std::size_t& index,
    const std::string& option,
    std::string& value,
    std::vector<std::string>& errors) {
    if (index + 1 >= args.size()) {
        errors.push_back(option + " requires a value.");
        return false;
    }
    value = args[++index];
    return true;
}

bool read_binary_file(const std::filesystem::path& path, std::string& out, std::ostream& err) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        err << "Failed opening " << path.string() << "\n";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    out = buffer.str();
    return true;
}

bool build_input_from_context_file(
    const BattlePredictorCliOptions& options,
    BattlePredictionInput& input,
    std::ostream& err) {
    if (!options.start_seed.has_value()) {
        err << "--start-seed is required with --context-file.\n";
        return false;
    }
    if (!options.fake_attacks.has_value()) {
        err << "--fake-attacks is required with --context-file.\n";
        return false;
    }

    std::string context_blob;
    if (!read_binary_file(options.context_file, context_blob, err)) {
        return false;
    }
    if (!soa::battle::ctx::codec::decode(context_blob, input.context)) {
        err << "Failed decoding context file " << options.context_file.string() << ".\n";
        return false;
    }

    const auto commands = soa::battle::actions::decode_battle_turn_commands_hex(options.turn_plan_hex);
    if (!commands.has_value()) {
        err << "Failed decoding --turn-plan-hex as BattleTurnCommandSet.\n";
        return false;
    }

    input.starting_rng_seed = *options.start_seed;
    input.turn_plan.fake_attack_count = static_cast<std::uint32_t>(*options.fake_attacks);
    input.turn_plan.commands = *commands;
    return true;
}

} // namespace

BattlePredictorCliParseResult parse_predict_battle_tokens(const std::vector<std::string>& args) {
    BattlePredictorCliParseResult result;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& arg = args[i];
        std::string value;
        if (arg == "--context-file") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.context_file = value;
            }
        } else if (arg == "--turn-plan-hex") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.turn_plan_hex = value;
            }
        } else if (arg == "--fake-attacks") {
            int parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_int(value, parsed) && parsed >= 0) {
                result.options.fake_attacks = parsed;
            } else {
                result.errors.push_back("--fake-attacks requires a non-negative integer.");
            }
        } else if (arg == "--start-seed") {
            std::uint32_t parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_u32_seed(value, parsed)) {
                result.options.start_seed = parsed;
            } else {
                result.errors.push_back("--start-seed requires a decimal or hex uint32.");
            }
        } else if (arg == "--turn-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.turn_job_id = parsed;
            } else {
                result.errors.push_back("--turn-job-id requires an integer.");
            }
        } else if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (require_value(args, i, arg, value, result.errors) && parse_ll(value, parsed)) {
                result.options.exec_job_id = parsed;
            } else {
                result.errors.push_back("--exec-job-id requires an integer.");
            }
        } else if (arg == "--db-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.db_root = value;
            }
        } else if (arg == "--profile") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.profile_name = value;
            }
        } else if (arg == "--format") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (value == "text") {
                    result.options.json = false;
                } else if (value == "json") {
                    result.options.json = true;
                } else {
                    result.errors.push_back("--format must be text or json.");
                }
            }
        } else if (arg == "--allow-seed-candidate-fallback") {
            result.options.allow_seed_candidate_fallback = true;
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back("Unknown predict-battle option: " + arg);
        }
    }

    if (!result.help_requested) {
        const auto validation = validate_predict_battle_options(result.options);
        result.errors.insert(result.errors.end(), validation.begin(), validation.end());
    }
    return result;
}

std::vector<std::string> validate_predict_battle_options(const BattlePredictorCliOptions& options) {
    std::vector<std::string> errors;
    const bool has_context_file = !options.context_file.empty();
    const bool has_turn = options.turn_job_id.has_value();
    const bool has_exec = options.exec_job_id.has_value();
    const bool has_db_selector = has_turn || has_exec;

    if (has_turn && has_exec) {
        errors.push_back("Specify only one of --turn-job-id or --exec-job-id.");
    }
    if (has_context_file == has_db_selector) {
        errors.push_back("Specify either --context-file with --turn-plan-hex, or one DB job selector.");
    }
    if (has_context_file && options.turn_plan_hex.empty()) {
        errors.push_back("--turn-plan-hex is required with --context-file.");
    }
    if (has_db_selector && is_mutable_debug_db_root(options.db_root)) {
        errors.push_back("Refusing to use D:/SoaSimDBDebug for prediction; use D:/SavorPredictDB.");
    }
    if (has_turn && *options.turn_job_id <= 0) {
        errors.push_back("--turn-job-id must be positive.");
    }
    if (has_exec && *options.exec_job_id <= 0) {
        errors.push_back("--exec-job-id must be positive.");
    }
    if (!battle_prediction_profile_by_name(options.profile_name).has_value()) {
        errors.push_back("Unsupported --profile: " + options.profile_name);
    }
    return errors;
}

int run_predict_battle(const BattlePredictorCliOptions& options, std::ostream& out, std::ostream& err) {
    const auto profile = battle_prediction_profile_by_name(options.profile_name);
    if (!profile.has_value()) {
        err << "Unsupported profile: " << options.profile_name << "\n";
        return 2;
    }

    BattlePredictionInput input;
    input.profile = *profile;

    const bool from_context_file = !options.context_file.empty();
    if (from_context_file) {
        if (!build_input_from_context_file(options, input, err)) {
            return 1;
        }
        const auto result = predict_battle(input);
        if (options.json) {
            write_battle_prediction_json(result, out);
        } else {
            write_battle_prediction_text(result, out);
        }
        return result.has_unsupported_events || !result.errors.empty() ? 1 : 0;
    }

    BattlePredictionDbInputOptions db_options;
    db_options.db_root = options.db_root;
    db_options.selector.turn_job_id = options.turn_job_id;
    db_options.selector.exec_job_id = options.exec_job_id;
    db_options.profile_name = options.profile_name;
    db_options.start_seed_override = options.start_seed;
    db_options.fake_attacks_override = options.fake_attacks;
    db_options.allow_seed_candidate_fallback = options.allow_seed_candidate_fallback;

    const auto db_input = build_battle_prediction_input_from_db_root(db_options, err);
    if (!db_input.has_value()) {
        return 1;
    }

    const auto result = predict_battle(db_input->input);
    if (options.json) {
        write_battle_prediction_run_json(db_input->metadata, result, out);
    } else {
        write_battle_prediction_db_metadata_text(db_input->metadata, out);
        out << "\n";
        write_battle_prediction_text(result, out);
    }
    return result.has_unsupported_events || !result.errors.empty() ? 1 : 0;
}

} // namespace savor::predict
