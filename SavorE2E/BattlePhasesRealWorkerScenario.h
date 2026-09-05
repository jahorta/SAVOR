#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "Cli.h"
#include "ScenarioEntry.h"

namespace savor::db::core {
class DBService;
}

namespace savor::e2e {

struct BattleScenarioAuthoringIds {
    std::int64_t seed_probe_spec_id = 0;
    std::int64_t battle_plan_id = 0;
};

bool PrepareBattleScenarioAuthoring(
    const CliOptions& options,
    std::string_view run_identity,
    bool cutscene_mode,
    savor::db::core::DBService* db_service,
    BattleScenarioAuthoringIds* ids_out,
    std::string* error_out);

bool SeedFreshBattleWorkflowForScenario(
    const CliOptions& options,
    std::int64_t dtm_artifact_id,
    std::int64_t rtc,
    bool cutscene_delay,
    std::string_view run_identity,
    const BattleScenarioAuthoringIds& authoring,
    savor::db::core::DBService* db_service,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool SeedEstablishedBattleWorkflowForScenario(
    const CliOptions& options,
    std::int64_t annotation_attempt_id,
    std::int64_t root_establishment_attempt_id,
    std::int64_t rtc,
    std::string_view run_identity,
    const BattleScenarioAuthoringIds& authoring,
    savor::db::core::DBService* db_service,
    std::int64_t* workflow_instance_id_out,
    std::string* error_out);

bool RunBattleWorkflowGraphRealWorkerScenario(
    const CliOptions& options,
    const ResolvedE2eScenarioEntry& entry,
    const char* argv0,
    savor::db::core::DBService* db_service,
    std::string* error_out,
    std::int64_t* workflow_instance_id_out = nullptr);

} // namespace savor::e2e
