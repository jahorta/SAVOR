#pragma once

#include "SavorDbRuntime.h"
#include "Analysis/IAnalysisDb.h"
#include "Phases/Programs/BattleCompletion/BattleCompletionContracts.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace savorqt::db {

struct TasRouteSnapshot {
    std::vector<savor::db::TasRouteNodeSnapshot> nodes;
    std::vector<savor::db::VictoryRouteBranchSnapshot> branches;
};

struct VictoryResultSummary {
    std::int64_t route_node_id = 0;
    std::int64_t battle_set_id = 0;
    std::int64_t wave_id = 0;
    int turn_index = 0;
    std::int64_t turn_job_id = 0;
    std::int64_t execution_job_id = 0;
    std::int64_t ending_rng = 0;
    int fake_attacks = 0;
    int delta_vi = 0;
    bool completion_representative = false;
    bool completion_ready = false;
    std::optional<std::int64_t> workflow_instance_id;
    std::optional<std::int64_t> manifest_artifact_id;
    std::string completion_status;
    std::string route_kind;
    std::uint32_t normal_experience = 0;
    std::uint32_t magic_experience = 0;
    std::uint32_t gold = 0;
    std::array<savor::runtime::battlecompletion::BattleRewardItemV1,
        savor::runtime::battlecompletion::RewardItemCount> items{};
    savor::runtime::battlecompletion::BattleResultsPresentationV1 presentation{};
    std::uint32_t entry_rng = 0;
    std::uint32_t completion_rng = 0;
    std::string transition_filename;
};

class SavorDbTasRouteService final {
public:
    static TasRouteSnapshot FetchRoutes() {
        TasRouteSnapshot out{};
        if (auto* db = SavorDbRuntime::instance().analysisDb()) {
            out.nodes = db->ListTasRouteNodes();
            out.branches = db->ListVictoryRouteBranches();
        }
        return out;
    }

    static bool RenameNode(std::int64_t route_node_id, const std::string& label,
                           std::string* error_out) {
        auto* db = SavorDbRuntime::instance().analysisDb();
        if (!db) {
            if (error_out) *error_out = "SavorDb runtime is unavailable";
            return false;
        }
        return db->RenameTasRouteNode(
            route_node_id, label, savor::db::types::UtcNow(), error_out);
    }

    static std::vector<VictoryResultSummary> FetchVictories(
        std::int64_t route_node_id) {
        std::vector<VictoryResultSummary> out;
        auto* db = SavorDbRuntime::instance().analysisDb();
        if (!db || route_node_id <= 0) return out;
        for (const auto battle_set_id : db->ListBattleSetIdsForRouteNode(route_node_id)) {
            const auto workflow = db->GetBattleWorkflowInstanceId(battle_set_id);
            for (const auto& wave : db->ListBattleTurnWaves(battle_set_id)) {
                for (const auto& job : db->ListBattleTurnJobsForWave(wave.wave_id)) {
                    if (!job.exec_job_id || job.battle_outcome !=
                            std::optional<savor::db::BattleTurnOutcome>(savor::db::BattleTurnOutcome::Victory))
                        continue;
                    const auto result = db->GetBattleSingleTurnResultForExecJob(*job.exec_job_id);
                    if (!result || result->terminal_kind != "SUCCEEDED" ||
                        result->domain_outcome != std::optional<std::string>("Victory") ||
                        !result->ending_rng)
                        continue;
                    VictoryResultSummary row{};
                    row.route_node_id = route_node_id;
                    row.battle_set_id = battle_set_id;
                    row.wave_id = wave.wave_id;
                    row.turn_index = wave.turn_index;
                    row.turn_job_id = job.turn_job_id;
                    row.execution_job_id = *job.exec_job_id;
                    row.ending_rng = *result->ending_rng;
                    row.fake_attacks = job.fake_attacks_this_turn + job.fake_attacks_used_before;
                    row.delta_vi = job.delta_vi.value_or(0);
                    row.workflow_instance_id = workflow;
                    const auto completion = db->GetBattleCompletionForSelectedTurnJob(job.turn_job_id);
                    if (completion) {
                        row.completion_representative = true;
                        row.completion_ready = completion->status == "COMPLETED";
                        row.completion_status = completion->status;
                        row.workflow_instance_id = completion->workflow_instance_id;
                        row.manifest_artifact_id = completion->manifest_artifact_id;
                        row.route_kind = completion->route_kind.value_or("");
                        row.transition_filename = completion->transition_filename.value_or("");
                        if (completion->manifest_blob) {
                            savor::runtime::battlecompletion::BattleCompletionManifestV1 manifest{};
                            const auto bytes = std::span<const std::uint8_t>(
                                reinterpret_cast<const std::uint8_t*>(completion->manifest_blob->data()),
                                completion->manifest_blob->size());
                            if (savor::runtime::battlecompletion::DecodeBattleCompletionManifestV1(bytes, manifest)) {
                                row.normal_experience = manifest.normal_experience_reward;
                                row.magic_experience = manifest.magic_experience_reward;
                                row.gold = manifest.gold_reward;
                                row.items = manifest.reward_items;
                                row.presentation = manifest.presentation;
                                row.entry_rng = manifest.entry_rng_seed;
                                row.completion_rng = manifest.completion_rng_seed;
                            }
                        }
                    } else {
                        row.completion_status = "NOT_SELECTED";
                    }
                    out.push_back(std::move(row));
                }
            }
        }
        return out;
    }
};

} // namespace savorqt::db
