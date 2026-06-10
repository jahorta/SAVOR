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
    using namespace savor::e2e;
    using namespace savor::db::core;
    using namespace savor::db::migrations;

    CliOptions options{};
    std::string parse_error;
    if (!ParseArgs(argc, argv, &options, &parse_error)) {
        std::cerr << parse_error << "\n\n";
        PrintUsage();
        return 2;
    }

    const std::map<std::string, bool (*)(const CliOptions&, const char*, DBService*, std::string*)> scenarios{
        { "seedprobe", &RunSeedProbeWorkflowGraphRealWorkerSmoke },
        { "tasmovie", &RunTasMovieRealWorkerSmoke },
        { "tasmovie_seedprobe", &RunTasMovieSeedProbeRealWorkerSmoke },
        { "battle", &RunBattleSingleTurnRealWorkerScenario },
        { "tasmovie_seedprobe_battle", &RunTasMovieSeedProbeBattleWorkflowGraphRealWorkerScenario },
        { "tasmovie_seedprobe_battle_override", &RunTasMovieSeedProbeBattleOverrideWorkflowGraphRealWorkerScenario },
        { "tasmovie_battle", &RunTasMovieBattleWorkflowGraphRealWorkerScenario },
    };

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
    for (const auto& scenario_name : options.scenarios) {
        const auto it = scenarios.find(scenario_name);
        if (it == scenarios.end()) {
            std::cerr << "unknown --scenario: " << scenario_name << "\n";
            service.Stop();
            return 2;
        }

        options.scenario = scenario_name;
        std::cout << "Running scenario '" << scenario_name << "' timeout=" << options.timeout_ms
                  << "ms poll=" << options.poll_ms << "ms\n";

        if (!it->second(options, argv[0], &service, &scenario_error)) {
            service.Stop();
            std::cerr << "[FAIL] " << scenario_name << " - " << scenario_error << "\n";
            return 1;
        }
        std::cout << "[PASS] " << scenario_name << "\n";
    }

    service.Stop();
    return 0;
}
