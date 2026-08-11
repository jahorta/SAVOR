#include "BattlePredictorCli.h"

#include "ActionViewStdJsonCache.h"
#include "BattlePredictionDbInput.h"
#include "BattlePredictionScenario.h"
#include "DbCopy.h"
#include "CliResourceInputCompatibility.h"

#include <Core/Input/SoaBattle/BattleCommandCodec.h>
#include <Core/Memory/Soa/Battle/BattleContextCodec.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

void add_unique_error(std::vector<std::string>& errors, std::string error) {
    if (std::find(errors.begin(), errors.end(), error) == errors.end()) {
        errors.push_back(std::move(error));
    }
}

BattleSourceSelection* ensure_scripted_source_selection(
    BattlePredictorCliOptions& options,
    std::vector<std::string>& errors) {
    if (!options.source_selection.has_value()) {
        options.source_selection = BattleSourceSelection{
            .producer_kind = BattleSourceProducerKind::ScriptedBattleRequest,
        };
    }
    if (options.source_selection->producer_kind
        != BattleSourceProducerKind::ScriptedBattleRequest) {
        add_unique_error(
            errors,
            "scripted battle source options conflict with another source producer");
        return nullptr;
    }
    return &*options.source_selection;
}

void apply_scenario_defaults(
    BattlePredictorCliOptions& options,
    std::vector<std::string>& errors) {
    if (!options.scenario_name.has_value()) {
        if (const auto profile = battle_prediction_profile_by_name(options.profile_name);
            profile.has_value()) {
            options.profile_name = profile->name;
        }
        return;
    }

    const auto scenario = battle_prediction_scenario_by_name(*options.scenario_name);
    if (!scenario.has_value()) {
        add_unique_error(errors, "Unsupported --scenario: " + *options.scenario_name);
        return;
    }
    options.scenario_name = scenario->name;
    if (!options.source_selection.has_value()) {
        options.source_selection = scenario->source_selection;
    }

    if (options.profile_explicit) {
        const auto profile = battle_prediction_profile_by_name(options.profile_name);
        if ((!profile.has_value() || profile->name != scenario->profile_name)
            && !options.allow_profile_overrides) {
            add_unique_error(
                errors,
                "--scenario " + scenario->name
                    + " requires --profile " + scenario->profile_name);
        }
    } else {
        options.profile_name = scenario->profile_name;
    }

    if (const auto profile = battle_prediction_profile_by_name(options.profile_name);
        profile.has_value()) {
        options.profile_name = profile->name;
    }
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

std::string trim_copy(std::string_view value) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    std::size_t first = 0;
    while (first < value.size() && is_space(static_cast<unsigned char>(value[first]))) {
        ++first;
    }
    std::size_t last = value.size();
    while (last > first && is_space(static_cast<unsigned char>(value[last - 1]))) {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

bool read_start_seed_list(
    const std::filesystem::path& path,
    std::vector<std::uint32_t>& out,
    std::ostream& err) {
    std::ifstream file(path);
    if (!file.is_open()) {
        err << "Failed opening --start-seed-list " << path.string() << "\n";
        return false;
    }

    std::string line;
    int line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        if (const auto comment = line.find('#'); comment != std::string::npos) {
            line.erase(comment);
        }
        const auto value = trim_copy(line);
        if (value.empty()) {
            continue;
        }
        std::uint32_t seed = 0;
        if (!parse_u32_seed(value, seed)) {
            err << "--start-seed-list " << path.string()
                << " line " << line_number
                << " is not a decimal or hex uint32 seed: " << value << "\n";
            return false;
        }
        out.push_back(seed);
    }

    if (out.empty()) {
        err << "--start-seed-list " << path.string() << " did not contain any seeds.\n";
        return false;
    }
    return true;
}

std::string json_escape_local(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (const char c : value) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
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
    input.start_boundary = BattlePredictionStartBoundary::BattleCoordinatorStart;
    input.turn_index = input.profile.supported_turn_index;
    input.scenario_name = options.scenario_name;
    input.source_selection = options.source_selection;
    input.source_validation.expected_encounter = options.expected_encounter;
    input.options.allow_profile_overrides = options.allow_profile_overrides;
    input.options.emit_causal_diagnostics =
        options.emit_causal_diagnostics;
    input.turn_plan.fake_attack_count = static_cast<std::uint32_t>(*options.fake_attacks);
    input.turn_plan.commands = *commands;
    return true;
}

} // namespace

BattlePredictorCliParseResult parse_predict_battle_tokens(const std::vector<std::string>& args) {
    BattlePredictorCliParseResult result;
    cli_detail::DiscDumpRootOptions disc_dump_root_options;

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
        } else if (arg == "--start-seed-list") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.start_seed_list = value;
            }
        } else if (arg == "--scenario") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.scenario_name = value;
            }
        } else if (arg == "--encounter-event-id") {
            int parsed = -1;
            if (require_value(args, i, arg, value, result.errors)
                && parse_int(value, parsed) && parsed >= 0) {
                result.options.expected_encounter = BattleEncounterIdentity{
                    .source_kind = BattleEncounterSourceKind::EventDefinition,
                    .encounter_id = parsed,
                };
            } else {
                result.errors.push_back(
                    "--encounter-event-id requires a non-negative integer.");
            }
        } else if (arg == "--scripted-battle-script") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (auto* selection = ensure_scripted_source_selection(
                        result.options, result.errors)) {
                    selection->scripted_request.script_identity = value;
                }
            }
        } else if (arg == "--scripted-battle-section") {
            if (require_value(args, i, arg, value, result.errors)) {
                if (auto* selection = ensure_scripted_source_selection(
                        result.options, result.errors)) {
                    selection->scripted_request.section_identity = value;
                }
            }
        } else if (arg == "--scripted-battle-payload-offset") {
            int parsed = -1;
            if (require_value(args, i, arg, value, result.errors)
                && parse_int(value, parsed) && parsed >= 0) {
                if (auto* selection = ensure_scripted_source_selection(
                        result.options, result.errors)) {
                    selection->scripted_request.instruction_payload_offset = parsed;
                }
            } else {
                result.errors.push_back(
                    "--scripted-battle-payload-offset requires a non-negative integer.");
            }
        } else if (arg == "--action-view-std-json-dir") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.action_view_std_json_dir = value;
            }
        } else if (arg == "--disc-dump-root"
            || arg == "--std-disc-dump-root") {
            if (require_value(args, i, arg, value, result.errors)) {
                disc_dump_root_options.observe(arg, value, result.errors);
            }
        } else if (arg == "--spice-file-parsing-exe") {
            if (require_value(args, i, arg, value, result.errors)) {
                result.options.spice_file_parsing_exe = value;
                cli_detail::add_spice_file_parsing_exe_warning(
                    result.warnings);
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
                result.options.profile_explicit = true;
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
        } else if (arg == "--allow-profile-overrides") {
            result.options.allow_profile_overrides = true;
        } else if (arg == "--emit-causal-diagnostics") {
            result.options.emit_causal_diagnostics = true;
        } else if (arg == "--help" || arg == "-h") {
            result.help_requested = true;
        } else {
            result.errors.push_back("Unknown predict-battle option: " + arg);
        }
    }

    disc_dump_root_options.finalize(
        result.options.disc_dump_root,
        result.errors,
        result.warnings);
    if (!result.help_requested) {
        apply_scenario_defaults(result.options, result.errors);
        const auto validation = validate_predict_battle_options(result.options);
        for (const auto& error : validation) {
            add_unique_error(result.errors, error);
        }
    }
    return result;
}

std::vector<std::string> validate_predict_battle_options(
    const BattlePredictorCliOptions& raw_options) {
    std::vector<std::string> errors;
    auto options = raw_options;
    apply_scenario_defaults(options, errors);
    const bool has_context_file = !options.context_file.empty();
    const bool has_turn = options.turn_job_id.has_value();
    const bool has_exec = options.exec_job_id.has_value();
    const bool has_db_selector = has_turn || has_exec;
    const bool has_start_seed_list = !options.start_seed_list.empty();

    if (has_turn && has_exec) {
        errors.push_back("Specify only one of --turn-job-id or --exec-job-id.");
    }
    if (has_context_file == has_db_selector) {
        errors.push_back("Specify either --context-file with --turn-plan-hex, or one DB job selector.");
    }
    if (has_context_file && options.turn_plan_hex.empty()) {
        errors.push_back("--turn-plan-hex is required with --context-file.");
    }
    if (has_start_seed_list && options.start_seed.has_value()) {
        errors.push_back("Specify only one of --start-seed or --start-seed-list.");
    }
    if (has_start_seed_list && has_context_file) {
        errors.push_back("--start-seed-list is only supported with a DB job selector.");
    }
    if (has_start_seed_list && !has_db_selector) {
        errors.push_back("--start-seed-list requires --turn-job-id or --exec-job-id.");
    }
    if (has_db_selector && is_mutable_debug_db_root(options.db_root)) {
        errors.push_back("Refusing to use D:/SoaSimDBDebug for prediction; use D:/SavorPredictDB.");
    }
    if (!options.action_view_std_json_dir.empty()
        && !std::filesystem::is_directory(options.action_view_std_json_dir)) {
        errors.push_back(
            "--action-view-std-json-dir must name an existing directory: "
            + options.action_view_std_json_dir.string());
    }
    if (has_turn && *options.turn_job_id <= 0) {
        errors.push_back("--turn-job-id must be positive.");
    }
    if (has_exec && *options.exec_job_id <= 0) {
        errors.push_back("--exec-job-id must be positive.");
    }
    if (options.source_selection.has_value()) {
        const auto& selection = *options.source_selection;
        if (selection.producer_kind
                != BattleSourceProducerKind::ScriptedBattleRequest
            || selection.scripted_request.script_identity.empty()
            || selection.scripted_request.section_identity.empty()
            || selection.scripted_request.instruction_payload_offset < 0) {
            errors.push_back(
                "A scripted battle source requires --scripted-battle-script, "
                "--scripted-battle-section, and --scripted-battle-payload-offset.");
        }
    }
    const auto profile = battle_prediction_profile_by_name(options.profile_name);
    if (!profile.has_value()) {
        errors.push_back("Unsupported --profile: " + options.profile_name);
    }
    return errors;
}

int run_predict_battle(const BattlePredictorCliOptions& options, std::ostream& out, std::ostream& err) {
    auto resolved_options = options;
    std::vector<std::string> option_errors;
    apply_scenario_defaults(resolved_options, option_errors);
    for (const auto& validation_error : validate_predict_battle_options(resolved_options)) {
        add_unique_error(option_errors, validation_error);
    }
    if (!option_errors.empty()) {
        for (const auto& option_error : option_errors) {
            err << option_error << "\n";
        }
        return 2;
    }

    const auto profile = battle_prediction_profile_by_name(resolved_options.profile_name);
    if (!profile.has_value()) {
        err << "Unsupported profile: " << resolved_options.profile_name << "\n";
        return 2;
    }

    if (!cli_detail::ensure_first_battle_resource_inputs(
            resolved_options.disc_dump_root,
            resolved_options.action_view_std_json_dir,
            resolved_options.resource_inputs,
            err)) {
        return 1;
    }

    BattlePredictionInput input;
    input.profile = *profile;
    input.resource_inputs = resolved_options.resource_inputs;

    const bool from_context_file = !resolved_options.context_file.empty();
    if (from_context_file) {
        if (!build_input_from_context_file(resolved_options, input, err)) {
            return 1;
        }
        const auto result = predict_battle(input);
        if (resolved_options.json) {
            write_battle_prediction_json(result, out);
        } else {
            write_battle_prediction_text(result, out);
        }
        return result.has_missing_input_events
            || result.has_unsupported_events
            || result.has_ambiguous_events
            || !result.errors.empty() ? 1 : 0;
    }

    BattlePredictionDbInputOptions db_options;
    db_options.db_root = resolved_options.db_root;
    db_options.selector.turn_job_id = resolved_options.turn_job_id;
    db_options.selector.exec_job_id = resolved_options.exec_job_id;
    db_options.profile_name = resolved_options.profile_name;
    db_options.scenario_name = resolved_options.scenario_name;
    db_options.source_selection = resolved_options.source_selection;
    db_options.expected_encounter = resolved_options.expected_encounter;
    db_options.fake_attacks_override = resolved_options.fake_attacks;
    db_options.resource_inputs = resolved_options.resource_inputs;
    db_options.allow_seed_candidate_fallback = resolved_options.allow_seed_candidate_fallback;
    db_options.allow_profile_overrides = resolved_options.allow_profile_overrides;
    db_options.emit_causal_diagnostics =
        resolved_options.emit_causal_diagnostics;

    if (!resolved_options.start_seed_list.empty()) {
        std::vector<std::uint32_t> start_seeds;
        if (!read_start_seed_list(resolved_options.start_seed_list, start_seeds, err)) {
            return 1;
        }

        int aggregate_rc = 0;
        if (resolved_options.json) {
            out << "{\n";
            out << "  \"start_seed_list\": \"" << json_escape_local(resolved_options.start_seed_list.generic_string()) << "\",\n";
            out << "  \"runs\": [\n";
        }
        for (std::size_t i = 0; i < start_seeds.size(); ++i) {
            db_options.start_seed_override = start_seeds[i];
            const auto db_input = build_battle_prediction_input_from_db_root(db_options, err);
            if (!db_input.has_value()) {
                return 1;
            }

            const auto result = predict_battle(db_input->input);
            const int run_rc =
                result.has_missing_input_events
                || result.has_unsupported_events
                || result.has_ambiguous_events
                || !result.errors.empty() ? 1 : 0;
            if (run_rc != 0) {
                aggregate_rc = run_rc;
            }

            if (resolved_options.json) {
                std::ostringstream run_json;
                write_battle_prediction_run_json(db_input->metadata, result, run_json);
                if (i > 0) {
                    out << ",\n";
                }
                out << run_json.str();
            } else {
                if (i > 0) {
                    out << "\n";
                }
                out << "Prediction seed run " << (i + 1) << "/" << start_seeds.size() << "\n";
                write_battle_prediction_db_metadata_text(db_input->metadata, out);
                out << "\n";
                write_battle_prediction_text(result, out);
            }
        }
        if (resolved_options.json) {
            out << "\n";
            out << "  ]\n";
            out << "}\n";
        }
        return aggregate_rc;
    }

    db_options.start_seed_override = resolved_options.start_seed;

    const auto db_input = build_battle_prediction_input_from_db_root(db_options, err);
    if (!db_input.has_value()) {
        return 1;
    }

    const auto result = predict_battle(db_input->input);
    if (resolved_options.json) {
        write_battle_prediction_run_json(db_input->metadata, result, out);
    } else {
        write_battle_prediction_db_metadata_text(db_input->metadata, out);
        out << "\n";
        write_battle_prediction_text(result, out);
    }
    return result.has_missing_input_events
        || result.has_unsupported_events
        || result.has_ambiguous_events
        || !result.errors.empty() ? 1 : 0;
}

} // namespace savor::predict
