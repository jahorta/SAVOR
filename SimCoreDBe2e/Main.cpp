#include <iostream>
#include <map>
#include <string>

#include "Cli.h"
#include "BattleSingleTurnScenario.h"
#include "Common/DbService.h"
#include "DbSetup.h"
#include "SeedProbeRealWorkerScenario.h"
#include "TasMovieRealWorkerScenario.h"

int main(int argc, char** argv) {
    using namespace simcore::e2e;
    using namespace simcore::db::core;
    using namespace simcore::db::migrations;

    CliOptions options{};
    std::string parse_error;
    if (!ParseArgs(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << "\n\n";
        PrintUsage();
        return 2;
    }

    const std::map<std::string, bool (*)(const CliOptions&, const char*, DBService*, std::string*)> scenarios{
        { "seedprobe_real_worker_smoke", &RunSeedProbeRealWorkerSmoke },
        { "tasmovie_real_worker_smoke", &RunTasMovieRealWorkerSmoke },
        { "tasmovie_seedprobe_real_worker_smoke", &RunTasMovieSeedProbeRealWorkerSmoke },
        { "battle_single_turn_real_worker_smoke", &RunBattleSingleTurnRealWorkerScenario },
    };

    const auto it = scenarios.find(options.scenario);
    if (it == scenarios.end()) {
        std::cerr << "unknown --scenario: " << options.scenario << "\n";
        return 2;
    }

    std::cout << "Running scenario '" << options.scenario << "' timeout=" << options.timeout_ms
              << "ms poll=" << options.poll_ms << "ms\n";

    const auto migration_root = ResolveMigrationRoot(options.migration_root);
    const auto db_paths = BuildDbPaths(options);
    DBService service(
        db_paths,
        MigrationSourceOptions{
            .source_kind = MigrationSourceKind::Filesystem,
            .filesystem_root = migration_root,
        });

    std::string db_error;
    if (!service.Start(&db_error)) {
        std::cerr << "[FAIL] starting DBService - " << db_error << "\n";
        return 1;
    }

    std::string scenario_error;
    if (!it->second(options, argv[0], &service, &scenario_error)) {
        service.Stop();
        std::cerr << "[FAIL] " << options.scenario << " - " << scenario_error << "\n";
        return 1;
    }

    service.Stop();
    std::cout << "[PASS] " << options.scenario << "\n";
    return 0;
}
