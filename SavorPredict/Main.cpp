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
        << "  SavorPredict write-first-battle-probe-layer-validation-profile --output PATH\n"
        << "  SavorPredict write-first-battle-action-motion-invocation-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-predictor-validation-profile --output PATH\n"
        << "  SavorPredict write-first-battle-turn-order-validation-profile --output PATH\n"
        << "  SavorPredict write-first-battle-field6-watch-profile --output PATH\n"
        << "  SavorPredict write-first-battle-view-eligibility-profile --output PATH\n"
        << "  SavorPredict write-first-battle-view-placement-cache-profile --output PATH\n"
        << "  SavorPredict write-first-battle-view-placement-frame-thread-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-view-placement-semantic-hooks-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-predictor-live-comparison-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-movement-destination-stop-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-action-view-service-lifecycle-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-visual-publication-order-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-pc-worker-selector-lifetime-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-queued-instruction-param-profile --output PATH\n"
        << "  SavorPredict write-first-battle-mode1-pathing-lifetime-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-mode1-state6-progress-profile --output PATH [--list-max 128|256] [--activation queued-state|counter-followup]\n"
        << "  SavorPredict write-first-battle-action-view-pathing-loop-profile --output PATH\n"
        << "  SavorPredict write-first-battle-thread-pathing-timing-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-battle-thread-producer-profile --output PATH [--list-max 128|256]\n"
        << "  SavorPredict write-first-battle-action-view-resource-profile --output PATH\n"
        << "  SavorPredict write-first-battle-action-view-selector-coverage-profile --output PATH\n"
        << "  SavorPredict write-first-battle-thread-list-profile --output PATH\n"
        << "  SavorPredict write-first-battle-pre-handler-frame-pathing-profile --output PATH\n"
        << "  SavorPredict write-first-battle-float-motion-profile --output PATH\n"
        << "  SavorPredict write-first-battle-move-increment-read-watch-profile --output PATH\n"
        << "  SavorPredict run-battle-job (--turn-job-id N | --exec-job-id N) --iso PATH --dolphin-base-dir PATH [--db-root PATH] [--run-root PATH] [--worker-exe PATH] [--probe-mode capture|progress-only|control-only] [--probe-cpu-core default|jit|interpreter] [--capture-profile PATH] [--sandbox-mode minimal|full-copy] [--timeout-ms N] [--battle-run-ms N] [--poll-ms N] [--override-start-rng-seed N] [--override-fake-attacks N] [--action-view-std-json-dir PATH] [--std-disc-dump-root PATH] [--spice-file-parsing-exe PATH]\n"
        << "  SavorPredict run-battle-jobs --exec-job-id N [--exec-job-id N ...] [--exec-job-list PATH] [--exec-job-seed EXEC_ID:SEED[:FAKE_ATTACKS]] [--exec-job-seed-list PATH] [--exec-job-fake-attacks EXEC_ID:FAKE_ATTACKS] [--exec-job-fake-attacks-list PATH] --iso PATH --dolphin-base-dir PATH [--db-root PATH] [--run-root PATH] [--worker-exe PATH] [--probe-mode capture|progress-only|control-only] [--probe-cpu-core default|jit|interpreter] [--capture-profile PATH] [--sandbox-mode minimal|full-copy] [--max-workers N] [--wait-for-workers-ready] [--timeout-ms N] [--battle-run-ms N] [--poll-ms N] [--override-start-rng-seed N] [--override-fake-attacks N] [--action-view-std-json-dir PATH] [--std-disc-dump-root PATH] [--spice-file-parsing-exe PATH]\n"
        << "  SavorPredict predict-battle (--context-file PATH --turn-plan-hex HEX --fake-attacks N --start-seed N | (--turn-job-id N | --exec-job-id N) [--start-seed N | --start-seed-list PATH]) [--db-root PATH] [--scenario first-battle-soldiers] [--encounter-event-id N] [--scripted-battle-script NAME --scripted-battle-section NAME --scripted-battle-payload-offset N] [--profile first-battle-soldiers] [--action-view-std-json-dir PATH] [--std-disc-dump-root PATH] [--spice-file-parsing-exe PATH] [--format text|json] [--allow-seed-candidate-fallback] [--allow-profile-overrides]\n"
        << "    Custom --start-seed values begin at the battle coordinator; stored job seeds retain their captured-turn boundary.\n"
        << "  SavorPredict trace-job (--turn-job-id N | --exec-job-id N) [--db-root PATH] [--format text|json] [--max-distance N]\n\n"
        << "  SavorPredict trace-checkpoints --checkpoint-file PATH [--turn-job-id N | --exec-job-id N] [--db-root PATH] [--action-view-std-json-dir PATH] [--std-disc-dump-root PATH] [--spice-file-parsing-exe PATH] [--format text|json] [--expected-fake-attacks N] [--expected-enemy-setup-draws N] [--expected-mode0e-camera-draws N] [--expected-turn-order-draws N] [--expected-attack-events N] [--expected-crit-draws N] [--expected-counter-roll-ceiling N] [--expected-drop-rolls N] [--expected-end-turn-status-draws N] [--expected-level-up-stat-rolls N]\n\n"
        << "Defaults:\n"
        << "  prepare-db --source D:/SoaSimDBDebug --dest D:/SavorPredictDB\n"
        << "  predict-battle --exec-job-id 147896\n"
        << "  predict-battle --scenario first-battle-soldiers --exec-job-id 147896 --format text\n"
        << "  trace-job --db-root D:/SavorPredictDB --format text --max-distance 5000\n\n"
        << "  action-view STD JSON cache: <db-root>/.std_json, generated from D:/SoAGC/2002-12-19-gc-us-final_Skies_of_Arcadia_Legends when available\n\n"
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

int run_write_first_battle_probe_layer_validation_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-probe-layer-validation-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_probe_layer_validation_profile(
        output, std::cout, std::cerr);
}

int run_write_first_battle_action_motion_invocation_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed) || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-action-motion-invocation-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_action_motion_invocation_profile(
        output, std::cout, std::cerr, list_max);
}

int run_write_first_battle_predictor_validation_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-predictor-validation-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_predictor_validation_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_turn_order_validation_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-turn-order-validation-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_turn_order_validation_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_field6_watch_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-field6-watch-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_field6_watch_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_view_eligibility_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-view-eligibility-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_view_eligibility_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_view_placement_cache_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-view-placement-cache-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_view_placement_cache_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_view_placement_frame_thread_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-view-placement-frame-thread-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_view_placement_frame_thread_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_view_placement_semantic_hooks_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-view-placement-semantic-hooks-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_view_placement_semantic_hooks_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_predictor_live_comparison_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-predictor-live-comparison-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_predictor_live_comparison_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_movement_destination_stop_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-movement-destination-stop-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_movement_destination_stop_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_action_view_service_lifecycle_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr
                << "Unknown write-first-battle-action-view-service-lifecycle-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_action_view_service_lifecycle_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_visual_publication_order_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr
                << "Unknown write-first-battle-visual-publication-order-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_visual_publication_order_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_pc_worker_selector_lifetime_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr
                << "Unknown write-first-battle-pc-worker-selector-lifetime-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_pc_worker_selector_lifetime_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_queued_instruction_param_profile(int argc, char** argv) {
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
            std::cerr
                << "Unknown write-first-battle-queued-instruction-param-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_queued_instruction_param_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_mode1_pathing_lifetime_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr
                << "Unknown write-first-battle-mode1-pathing-lifetime-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_mode1_pathing_lifetime_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_mode1_state6_progress_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    auto activation = savor::predict::Mode1State6ProgressActivation::QueuedState;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--activation") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            if (value == "queued-state") {
                activation = savor::predict::Mode1State6ProgressActivation::QueuedState;
            } else if (value == "counter-followup") {
                activation =
                    savor::predict::Mode1State6ProgressActivation::CounterFollowup;
            } else {
                std::cerr
                    << "--activation must be queued-state or counter-followup.\n";
                return 2;
            }
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr
                << "Unknown write-first-battle-mode1-state6-progress-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_mode1_state6_progress_profile(
        output,
        std::cout,
        std::cerr,
        list_max,
        activation);
}

int run_write_first_battle_action_view_pathing_loop_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-action-view-pathing-loop-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_action_view_pathing_loop_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_thread_pathing_timing_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-first-battle-thread-pathing-timing-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_thread_pathing_timing_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_battle_thread_producer_profile(int argc, char** argv) {
    std::filesystem::path output;
    std::uint32_t list_max = 128;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        std::string value;
        if (arg == "--output") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            output = value;
        } else if (arg == "--list-max") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr)
                || !parse_int(value, parsed)
                || (parsed != 128 && parsed != 256)) {
                std::cerr << "--list-max must be 128 or 256.\n";
                return 2;
            }
            list_max = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--help" || arg == "-h") {
            print_usage(std::cout);
            return 0;
        } else {
            std::cerr << "Unknown write-battle-thread-producer-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_battle_thread_producer_profile(
        output,
        std::cout,
        std::cerr,
        list_max);
}

int run_write_first_battle_action_view_resource_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-action-view-resource-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_action_view_resource_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_action_view_selector_coverage_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-action-view-selector-coverage-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_action_view_selector_coverage_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_thread_list_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-thread-list-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_thread_list_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_pre_handler_frame_pathing_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-pre-handler-frame-pathing-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_pre_handler_frame_pathing_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_float_motion_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-float-motion-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_float_motion_profile(
        output,
        std::cout,
        std::cerr);
}

int run_write_first_battle_move_increment_read_watch_profile(int argc, char** argv) {
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
            std::cerr << "Unknown write-first-battle-move-increment-read-watch-profile option: "
                << arg << "\n";
            return 2;
        }
    }
    return savor::predict::write_first_battle_move_increment_read_watch_profile(
        output,
        std::cout,
        std::cerr);
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
        } else if (arg == "--action-view-std-json-dir") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.action_view_std_json_dir = value;
            if (!std::filesystem::is_directory(options.action_view_std_json_dir)) {
                std::cerr << "--action-view-std-json-dir must name an existing directory: "
                          << options.action_view_std_json_dir.string() << "\n";
                return 2;
            }
        } else if (arg == "--std-disc-dump-root") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.std_disc_dump_root = value;
        } else if (arg == "--spice-file-parsing-exe") {
            if (!require_value(argc, argv, i, arg, value, std::cerr)) {
                return 2;
            }
            options.spice_file_parsing_exe = value;
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
    if (command == "write-first-battle-probe-layer-validation-profile") {
        return run_write_first_battle_probe_layer_validation_profile(argc, argv);
    }
    if (command == "write-first-battle-action-motion-invocation-profile") {
        return run_write_first_battle_action_motion_invocation_profile(argc, argv);
    }
    if (command == "write-first-battle-predictor-validation-profile") {
        return run_write_first_battle_predictor_validation_profile(argc, argv);
    }
    if (command == "write-first-battle-turn-order-validation-profile") {
        return run_write_first_battle_turn_order_validation_profile(argc, argv);
    }
    if (command == "write-first-battle-field6-watch-profile") {
        return run_write_first_battle_field6_watch_profile(argc, argv);
    }
    if (command == "write-first-battle-view-eligibility-profile") {
        return run_write_first_battle_view_eligibility_profile(argc, argv);
    }
    if (command == "write-first-battle-view-placement-cache-profile") {
        return run_write_first_battle_view_placement_cache_profile(argc, argv);
    }
    if (command == "write-first-battle-view-placement-frame-thread-profile") {
        return run_write_first_battle_view_placement_frame_thread_profile(argc, argv);
    }
    if (command == "write-first-battle-view-placement-semantic-hooks-profile") {
        return run_write_first_battle_view_placement_semantic_hooks_profile(argc, argv);
    }
    if (command == "write-first-battle-predictor-live-comparison-profile") {
        return run_write_first_battle_predictor_live_comparison_profile(argc, argv);
    }
    if (command == "write-first-battle-movement-destination-stop-profile") {
        return run_write_first_battle_movement_destination_stop_profile(argc, argv);
    }
    if (command == "write-first-battle-action-view-service-lifecycle-profile") {
        return run_write_first_battle_action_view_service_lifecycle_profile(argc, argv);
    }
    if (command == "write-first-battle-visual-publication-order-profile") {
        return run_write_first_battle_visual_publication_order_profile(argc, argv);
    }
    if (command == "write-first-battle-pc-worker-selector-lifetime-profile") {
        return run_write_first_battle_pc_worker_selector_lifetime_profile(argc, argv);
    }
    if (command == "write-first-battle-queued-instruction-param-profile") {
        return run_write_first_battle_queued_instruction_param_profile(argc, argv);
    }
    if (command == "write-first-battle-mode1-pathing-lifetime-profile") {
        return run_write_first_battle_mode1_pathing_lifetime_profile(argc, argv);
    }
    if (command == "write-first-battle-mode1-state6-progress-profile") {
        return run_write_first_battle_mode1_state6_progress_profile(argc, argv);
    }
    if (command == "write-first-battle-action-view-pathing-loop-profile") {
        return run_write_first_battle_action_view_pathing_loop_profile(argc, argv);
    }
    if (command == "write-first-battle-thread-pathing-timing-profile") {
        return run_write_first_battle_thread_pathing_timing_profile(argc, argv);
    }
    if (command == "write-battle-thread-producer-profile") {
        return run_write_battle_thread_producer_profile(argc, argv);
    }
    if (command == "write-first-battle-action-view-resource-profile") {
        return run_write_first_battle_action_view_resource_profile(argc, argv);
    }
    if (command == "write-first-battle-action-view-selector-coverage-profile") {
        return run_write_first_battle_action_view_selector_coverage_profile(argc, argv);
    }
    if (command == "write-first-battle-thread-list-profile") {
        return run_write_first_battle_thread_list_profile(argc, argv);
    }
    if (command == "write-first-battle-pre-handler-frame-pathing-profile") {
        return run_write_first_battle_pre_handler_frame_pathing_profile(argc, argv);
    }
    if (command == "write-first-battle-float-motion-profile") {
        return run_write_first_battle_float_motion_profile(argc, argv);
    }
    if (command == "write-first-battle-move-increment-read-watch-profile") {
        return run_write_first_battle_move_increment_read_watch_profile(argc, argv);
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
