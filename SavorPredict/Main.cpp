#include "BattleJobBatchRunOptions.h"
#include "BattleJobBatchWorkerRun.h"
#include "BattleJobRunOptions.h"
#include "BattleJobWorkerRun.h"
#include "BattlePredictorCli.h"
#include "CheckpointTrace.h"
#include "DbCopy.h"
#include "LiveCaptureProfile.h"
#include "TraceJob.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void print_usage(std::ostream& out) {
    out << "SavorPredict exploratory CLI\n\n"
        << "Usage:\n"
        << "  SavorPredict prepare-db [--source PATH] [--dest PATH] [--overwrite]\n"
        << "  SavorPredict write-first-battle-capture-profile --output PATH\n"
        << "  SavorPredict run-battle-job (--turn-job-id N | --exec-job-id N) --iso PATH --dolphin-base-dir PATH [--db-root PATH] [--run-root PATH] [--worker-exe PATH] [--capture-profile PATH] [--sandbox-mode minimal|full-copy] [--timeout-ms N] [--poll-ms N]\n"
        << "  SavorPredict run-battle-jobs --exec-job-id N [--exec-job-id N ...] [--exec-job-list PATH] --iso PATH --dolphin-base-dir PATH [--db-root PATH] [--run-root PATH] [--worker-exe PATH] [--capture-profile PATH] [--sandbox-mode minimal|full-copy] [--max-workers N] [--timeout-ms N] [--poll-ms N]\n"
        << "  SavorPredict predict-battle (--context-file PATH --turn-plan-hex HEX --fake-attacks N --start-seed N | --turn-job-id N | --exec-job-id N) [--db-root PATH] [--profile first-battle] [--format text|json] [--allow-seed-candidate-fallback]\n"
        << "  SavorPredict trace-job (--turn-job-id N | --exec-job-id N) [--db-root PATH] [--format text|json] [--max-distance N]\n\n"
        << "  SavorPredict trace-checkpoints --checkpoint-file PATH [--turn-job-id N | --exec-job-id N] [--db-root PATH] [--format text|json] [--expected-fake-attacks N] [--expected-enemy-setup-draws N] [--expected-mode0e-camera-draws N] [--expected-turn-order-draws N] [--expected-attack-events N] [--expected-crit-draws N] [--expected-counter-roll-ceiling N] [--expected-drop-rolls N] [--expected-end-turn-status-draws N] [--expected-level-up-stat-rolls N]\n\n"
        << "Defaults:\n"
        << "  prepare-db --source D:/SoaSimDBDebug --dest D:/SavorPredictDB\n"
        << "  predict-battle --db-root D:/SavorPredictDB --profile first-battle --format text\n"
        << "  trace-job --db-root D:/SavorPredictDB --format text --max-distance 5000\n\n"
        << "Policy:\n"
        << "  D:/SoaSimDBDebug is only the mutable prepare-db source. Analysis commands must use D:/SavorPredictDB.\n";
}

bool require_value(int argc, char** argv, int& index, std::string_view option, std::string& value, std::ostream& err) {
    if (index + 1 >= argc) {
        err << option << " requires a value.\n";
        return false;
    }
    value = argv[++index];
    return true;
}

bool parse_ll(const std::string& value, long long& out) {
    char* end = nullptr;
    out = std::strtoll(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0';
}

bool parse_int(const std::string& value, int& out) {
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

std::string normalize_policy_path(const std::filesystem::path& path) {
    auto normalized = path.lexically_normal().generic_string();
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    while (normalized.size() > 1 && normalized.back() == '/') {
        normalized.pop_back();
    }
    return normalized;
}

bool reject_mutable_debug_db_root(const std::filesystem::path& db_root, std::ostream& err) {
    if (normalize_policy_path(db_root) != "d:/soasimdbdebug") {
        return false;
    }

    err << "Refusing to use D:/SoaSimDBDebug as an analysis DB root.\n"
        << "D:/SoaSimDBDebug is mutable and may change jobs or schema while we work.\n"
        << "Run prepare-db once to copy it to D:/SavorPredictDB, then use --db-root D:/SavorPredictDB.\n";
    return true;
}

int run_prepare_db(int argc, char** argv) {
    savor::predict::PrepareDbOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--source") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.source = value;
        } else if (arg == "--dest") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.dest = value;
        } else if (arg == "--overwrite") {
            options.overwrite = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown prepare-db option: " << arg << "\n";
            return 2;
        }
    }

    return savor::predict::run_prepare_db(options, std::cout, std::cerr);
}

int run_trace_job(int argc, char** argv) {
    savor::predict::TraceJobOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--turn-job-id") {
            long long parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_ll(value, parsed)) {
                std::cerr << "--turn-job-id requires an integer.\n";
                return 2;
            }
            options.turn_job_id = parsed;
        } else if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_ll(value, parsed)) {
                std::cerr << "--exec-job-id requires an integer.\n";
                return 2;
            }
            options.exec_job_id = parsed;
        } else if (arg == "--db-root") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.db_root = value;
        } else if (arg == "--format") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            if (value == "text") {
                options.json = false;
            } else if (value == "json") {
                options.json = true;
            } else {
                std::cerr << "--format must be text or json.\n";
                return 2;
            }
        } else if (arg == "--max-distance") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--max-distance requires a non-negative integer.\n";
                return 2;
            }
            options.max_distance = parsed;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown trace-job option: " << arg << "\n";
            return 2;
        }
    }

    if (reject_mutable_debug_db_root(options.db_root, std::cerr)) {
        return 2;
    }

    return savor::predict::run_trace_job(options, std::cout, std::cerr);
}

int run_write_first_battle_capture_profile(int argc, char** argv) {
    std::filesystem::path output;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-capture-profile option: " << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_capture_profile(output, std::cout, std::cerr);
}

int run_battle_job_command(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 2)));
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const auto parsed = savor::predict::parse_battle_job_run_tokens(args, argv[0]);
    if (parsed.help_requested) {
        print_usage(std::cout);
        return parsed.errors.empty() ? 0 : 2;
    }
    if (!parsed.errors.empty()) {
        for (const auto& error : parsed.errors) {
            std::cerr << error << "\n";
        }
        return 2;
    }
    return savor::predict::run_battle_job(parsed.options, std::cout, std::cerr);
}

int run_battle_jobs_command(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 2)));
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const auto parsed = savor::predict::parse_battle_job_batch_run_tokens(args, argv[0]);
    if (parsed.help_requested) {
        print_usage(std::cout);
        return parsed.errors.empty() ? 0 : 2;
    }
    if (!parsed.errors.empty()) {
        for (const auto& error : parsed.errors) {
            std::cerr << error << "\n";
        }
        return 2;
    }
    return savor::predict::run_battle_jobs(parsed.options, std::cout, std::cerr);
}

int run_predict_battle_command(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<std::size_t>(std::max(0, argc - 2)));
    for (int i = 2; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const auto parsed = savor::predict::parse_predict_battle_tokens(args);
    if (parsed.help_requested) {
        print_usage(std::cout);
        return parsed.errors.empty() ? 0 : 2;
    }
    if (!parsed.errors.empty()) {
        for (const auto& error : parsed.errors) {
            std::cerr << error << "\n";
        }
        return 2;
    }
    return savor::predict::run_predict_battle(parsed.options, std::cout, std::cerr);
}

int run_trace_checkpoints_command(int argc, char** argv) {
    savor::predict::TraceCheckpointsOptions options;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--checkpoint-file") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.checkpoint_file = value;
        } else if (arg == "--turn-job-id") {
            long long parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_ll(value, parsed)) {
                std::cerr << "--turn-job-id requires an integer.\n";
                return 2;
            }
            options.turn_job_id = parsed;
        } else if (arg == "--exec-job-id") {
            long long parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_ll(value, parsed)) {
                std::cerr << "--exec-job-id requires an integer.\n";
                return 2;
            }
            options.exec_job_id = parsed;
        } else if (arg == "--db-root") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.db_root = value;
        } else if (arg == "--format") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            if (value == "text") {
                options.json = false;
            } else if (value == "json") {
                options.json = true;
            } else {
                std::cerr << "--format must be text or json.\n";
                return 2;
            }
        } else if (arg == "--expected-fake-attacks") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-fake-attacks requires a non-negative integer.\n";
                return 2;
            }
            options.expected_fake_attacks = parsed;
        } else if (arg == "--expected-enemy-setup-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-enemy-setup-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_enemy_setup_draws = parsed;
        } else if (arg == "--expected-mode0e-camera-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-mode0e-camera-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_mode0e_camera_draws = parsed;
        } else if (arg == "--expected-turn-order-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-turn-order-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_turn_order_draws = parsed;
        } else if (arg == "--expected-attack-events") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-attack-events requires a non-negative integer.\n";
                return 2;
            }
            options.expected_attack_events = parsed;
        } else if (arg == "--expected-crit-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-crit-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_crit_draws = parsed;
        } else if (arg == "--expected-counter-roll-ceiling") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-counter-roll-ceiling requires a non-negative integer.\n";
                return 2;
            }
            options.expected_counter_roll_ceiling = parsed;
        } else if (arg == "--expected-drop-rolls") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-drop-rolls requires a non-negative integer.\n";
                return 2;
            }
            options.expected_drop_rolls = parsed;
        } else if (arg == "--expected-end-turn-status-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-end-turn-status-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_end_turn_status_draws = parsed;
        } else if (arg == "--expected-level-up-stat-rolls") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-level-up-stat-rolls requires a non-negative integer.\n";
                return 2;
            }
            options.expected_level_up_stat_rolls = parsed;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown trace-checkpoints option: " << arg << "\n";
            return 2;
        }
    }

    if (reject_mutable_debug_db_root(options.db_root, std::cerr)) {
        return 2;
    }

    return savor::predict::run_trace_checkpoints(options, std::cout, std::cerr);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "-h") {
        print_usage(argc < 2 ? std::cerr : std::cout);
        return argc < 2 ? 2 : 0;
    }

    const std::string command = argv[1];
    if (command == "prepare-db") {
        return run_prepare_db(argc, argv);
    }
    if (command == "write-first-battle-capture-profile") {
        return run_write_first_battle_capture_profile(argc, argv);
    }
    if (command == "run-battle-job") {
        return run_battle_job_command(argc, argv);
    }
    if (command == "run-battle-jobs") {
        return run_battle_jobs_command(argc, argv);
    }
    if (command == "predict-battle") {
        return run_predict_battle_command(argc, argv);
    }
    if (command == "trace-job") {
        return run_trace_job(argc, argv);
    }
    if (command == "trace-checkpoints") {
        return run_trace_checkpoints_command(argc, argv);
    }

    std::cerr << "Unknown command: " << command << "\n";
    print_usage(std::cerr);
    return 2;
}
