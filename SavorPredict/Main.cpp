#include "CheckpointTrace.h"
#include "DbCopy.h"
#include "TraceJob.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void print_usage(std::ostream& out) {
    out << "SavorPredict exploratory CLI\n\n"
        << "Usage:\n"
        << "  SavorPredict prepare-db [--source PATH] [--dest PATH] [--overwrite]\n"
        << "  SavorPredict trace-job (--turn-job-id N | --exec-job-id N) [--db-root PATH] [--format text|json] [--max-distance N]\n\n"
        << "  SavorPredict trace-checkpoints --checkpoint-file PATH [--turn-job-id N | --exec-job-id N] [--db-root PATH] [--format text|json] [--expected-mode0e-camera-draws N] [--expected-attack-events N] [--expected-crit-draws N]\n\n"
        << "Defaults:\n"
        << "  prepare-db --source D:/SoaSimDBDebug --dest D:/SavorPredictDB\n"
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
        } else if (arg == "--expected-mode0e-camera-draws") {
            int parsed = 0;
            if (!require_value(argc, argv, i, arg, value, std::cerr) || !parse_int(value, parsed) || parsed < 0) {
                std::cerr << "--expected-mode0e-camera-draws requires a non-negative integer.\n";
                return 2;
            }
            options.expected_mode0e_camera_draws = parsed;
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
