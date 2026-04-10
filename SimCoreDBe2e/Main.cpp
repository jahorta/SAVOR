#include <iostream>
#include <map>
#include <string>

#include "Cli.h"
#include "SeedProbeRealWorkerScenario.h"

int main(int argc, char** argv) {
    using namespace simcore::e2e;

    CliOptions options{};
    std::string parse_error;
    if (!ParseArgs(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << "\n\n";
        PrintUsage();
        return 2;
    }

    const std::map<std::string, bool (*)(const CliOptions&, const char*, std::string*)> scenarios{
        { "seedprobe_real_worker_smoke", &RunSeedProbeRealWorkerSmoke },
    };

    const auto it = scenarios.find(options.scenario);
    if (it == scenarios.end()) {
        std::cerr << "unknown --scenario: " << options.scenario << "\n";
        return 2;
    }

    std::cout << "Running scenario '" << options.scenario << "' timeout=" << options.timeout_ms
              << "ms poll=" << options.poll_ms << "ms\n";

    std::string scenario_error;
    if (!it->second(options, argv[0], &scenario_error)) {
        std::cerr << "[FAIL] " << options.scenario << " - " << scenario_error << "\n";
        return 1;
    }

    std::cout << "[PASS] " << options.scenario << "\n";
    return 0;
}
