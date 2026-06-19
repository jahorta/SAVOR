#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "SavorDbRuntime.h"
#include "DB/SavorDbServiceResult.h"
#include "Analysis/IAnalysisDb.h"
#include "Execution/IExecutionDb.h"
#include "State/IStateDb.h"
#include "UIRead/IUiReadDb.h"
#include "Core/Input/SoaBattle/BattleCommandCodec.h"

namespace savorqt::db {

struct ExplorerRunCursor {
    std::int64_t primary = 0;
    std::int64_t secondary = 0;
};

struct ExplorerRunGroupQuery {
    std::optional<ExplorerRunCursor> before;
    std::optional<ExplorerRunCursor> after;
    int limit = 50;
};

struct ExplorerRunGroupPage {
    std::vector<savor::db::UiJobSetSummary> groups;
    std::optional<ExplorerRunCursor> next;
    std::optional<ExplorerRunCursor> prev;
};

struct ExplorerRunJobDetail {
    savor::db::UiJobDetail job;
    std::vector<savor::db::UiJobArtifact> artifacts;
    std::vector<savor::db::ExecutionJobEventRecord> events;
};

using BattleRunCursor = ExplorerRunCursor;

struct BattleRunGroupQuery {
    std::optional<BattleRunCursor> before;
    std::optional<BattleRunCursor> after;
    int limit = 50;
    bool child_selected_only = false;
    bool final_victory_only = false;
};

struct BattleRunGroupPage {
    std::vector<savor::db::UiBattleGroupSummary> groups;
    std::optional<BattleRunCursor> next;
    std::optional<BattleRunCursor> prev;
};

struct BattleRunJobDetail {
    savor::db::UiBattleTurnJobDetail battle;
    std::optional<savor::db::UiJobDetail> job;
    std::vector<savor::db::ExecutionJobEventRecord> events;
};

struct BattleReplicationOrigin {
    std::optional<std::int64_t> entry_savestate_id;
    std::optional<std::int64_t> entry_savestate_artifact_id;
    std::optional<std::int64_t> seed_candidate_id;
    std::string seed_source_kind;
    std::optional<std::int64_t> source_unique_seed_id;
    std::optional<std::int64_t> source_input_frame_id;
    std::optional<savor::GCInputFrame> initial_input;
};

struct BattleReplicationTurn {
    savor::db::UiBattleTurnJobReplicationRow row;
    std::vector<soa::battle::actions::BattleCommand> commands;
    bool command_decode_ok = false;
};

struct BattleReplicationDetails {
    BattleReplicationOrigin origin;
    std::vector<BattleReplicationTurn> turns;
};

class SavorDbExplorerRunService {
public:
    static constexpr int kBattleSingleTurnProgramKind = 5;

    static ServiceResult<ExplorerRunGroupPage> ListGroups(const ExplorerRunGroupQuery& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<ExplorerRunGroupPage>("SavorDb UIRead is unavailable");
        }

        savor::db::UiReadJobSetListQuery query{};
        query.program_kind = kBattleSingleTurnProgramKind;
        query.limit = (std::max)(1, request.limit);
        if (request.before.has_value()) {
            query.before = savor::db::UiReadListCursor{ request.before->primary, request.before->secondary };
        }
        if (request.after.has_value()) {
            query.after = savor::db::UiReadListCursor{ request.after->primary, request.after->secondary };
        }

        const auto page = db->ListJobSets(query);
        ExplorerRunGroupPage out{};
        out.groups = page.items;
        if (page.next.has_value()) {
            out.next = ExplorerRunCursor{ page.next->primary, page.next->secondary };
        }
        if (page.prev.has_value()) {
            out.prev = ExplorerRunCursor{ page.prev->primary, page.prev->secondary };
        }
        return ServiceResult<ExplorerRunGroupPage>::Ok(std::move(out));
    }

    static ServiceResult<savor::db::UiJobSetDetail> GetGroupDetail(std::int64_t job_set_id, int jobs_limit) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<savor::db::UiJobSetDetail>("SavorDb UIRead is unavailable");
        }
        const auto detail = db->GetJobSetDetail(job_set_id, (std::max)(1, jobs_limit));
        if (!detail.has_value()) {
            return NotFound<savor::db::UiJobSetDetail>("explorer run job set not found");
        }
        return ServiceResult<savor::db::UiJobSetDetail>::Ok(*detail);
    }

    static ServiceResult<ExplorerRunJobDetail> GetJobDetail(std::int64_t job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<ExplorerRunJobDetail>("SavorDb UIRead is unavailable");
        }

        const auto job = ui_read->GetJobDetail(job_id);
        if (!job.has_value()) {
            return NotFound<ExplorerRunJobDetail>("job not found");
        }

        ExplorerRunJobDetail out{};
        out.job = *job;
        out.artifacts = ui_read->ListJobArtifacts(job_id);

        if (auto* execution = ExecutionDb(); execution != nullptr) {
            out.events = execution->ListJobEvents(job_id, 128);
        }

        return ServiceResult<ExplorerRunJobDetail>::Ok(std::move(out));
    }

    static ServiceResult<BattleRunGroupPage> ListBattleGroups(const BattleRunGroupQuery& request) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<BattleRunGroupPage>("SavorDb UIRead is unavailable");
        }

        savor::db::UiBattleGroupListQuery query{};
        query.limit = (std::max)(1, request.limit);
        query.child_selected_only = request.child_selected_only;
        query.final_victory_only = request.final_victory_only;
        if (request.before.has_value()) {
            query.before = savor::db::UiReadListCursor{ request.before->primary, request.before->secondary };
        }
        if (request.after.has_value()) {
            query.after = savor::db::UiReadListCursor{ request.after->primary, request.after->secondary };
        }

        const auto page = db->ListBattleGroups(query);
        BattleRunGroupPage out{};
        out.groups = page.items;
        if (page.next.has_value()) {
            out.next = BattleRunCursor{ page.next->primary, page.next->secondary };
        }
        if (page.prev.has_value()) {
            out.prev = BattleRunCursor{ page.prev->primary, page.prev->secondary };
        }
        return ServiceResult<BattleRunGroupPage>::Ok(std::move(out));
    }

    static ServiceResult<std::vector<savor::db::UiBattleWaveSummary>> ListBattleWaves(
        std::int64_t battle_set_id) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiBattleWaveSummary>>("SavorDb UIRead is unavailable");
        }
        return ServiceResult<std::vector<savor::db::UiBattleWaveSummary>>::Ok(db->ListBattleWaves(battle_set_id));
    }

    static ServiceResult<std::vector<savor::db::UiBattleTurnJobSummary>> ListBattleTurnJobsForWaves(
        const std::vector<std::int64_t>& wave_ids,
        bool final_victory_only = false) {
        auto* db = UiReadDb();
        if (db == nullptr) {
            return Unavailable<std::vector<savor::db::UiBattleTurnJobSummary>>("SavorDb UIRead is unavailable");
        }
        return ServiceResult<std::vector<savor::db::UiBattleTurnJobSummary>>::Ok(
            db->ListBattleTurnJobsForWaves(wave_ids, final_victory_only));
    }

    static ServiceResult<BattleRunJobDetail> GetBattleTurnJobDetail(std::int64_t turn_job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<BattleRunJobDetail>("SavorDb UIRead is unavailable");
        }

        const auto battle = ui_read->GetBattleTurnJobDetail(turn_job_id);
        if (!battle.has_value()) {
            return NotFound<BattleRunJobDetail>("battle job not found");
        }

        BattleRunJobDetail out{};
        out.battle = *battle;
        const std::int64_t exec_job_id = battle->summary.exec_job_id.value_or(0);
        if (exec_job_id > 0) {
            out.job = ui_read->GetJobDetail(exec_job_id);
        }
        if (auto* execution = ExecutionDb(); execution != nullptr) {
            if (exec_job_id > 0) {
                out.events = execution->ListJobEvents(exec_job_id, 128);
            }
        }
        return ServiceResult<BattleRunJobDetail>::Ok(std::move(out));
    }

    static ServiceResult<BattleReplicationDetails> GetBattleReplicationDetails(std::int64_t turn_job_id) {
        auto* ui_read = UiReadDb();
        if (ui_read == nullptr) {
            return Unavailable<BattleReplicationDetails>("SavorDb UIRead is unavailable");
        }

        auto chain = ui_read->ListBattleTurnJobReplicationChain(turn_job_id);
        if (chain.empty()) {
            return NotFound<BattleReplicationDetails>("battle replication chain not found");
        }

        BattleReplicationDetails out{};
        out.origin.entry_savestate_id = chain.front().source_savestate_id;
        if (out.origin.entry_savestate_id.has_value()) {
            if (auto* state = StateDb(); state != nullptr) {
                if (const auto payload = state->ResolveArtifactPayload(1, "savestate", *out.origin.entry_savestate_id);
                    payload.has_value() && payload->artifact_id > 0) {
                    out.origin.entry_savestate_artifact_id = payload->artifact_id;
                }
            }
        }
        out.origin.seed_candidate_id = chain.front().seed_candidate_id;
        if (auto* analysis = AnalysisDb(); analysis != nullptr && out.origin.seed_candidate_id.has_value()) {
            if (const auto candidate = analysis->GetBattleSeedCandidate(*out.origin.seed_candidate_id); candidate.has_value()) {
                out.origin.seed_source_kind = std::string(savor::db::ToDbString(candidate->source_kind));
                out.origin.source_unique_seed_id = candidate->source_unique_seed_id;
                out.origin.source_input_frame_id = candidate->source_input_frame_id;
                if (candidate->source_unique_seed_id.has_value()) {
                    if (const auto unique = analysis->GetSeedProbeUniqueSeed(*candidate->source_unique_seed_id); unique.has_value()) {
                        savor::GCInputFrame frame{};
                        frame.main_x = static_cast<std::uint8_t>(std::clamp(unique->main_x, 0, 255));
                        frame.main_y = static_cast<std::uint8_t>(std::clamp(unique->main_y, 0, 255));
                        frame.c_x = static_cast<std::uint8_t>(std::clamp(unique->cstick_x, 0, 255));
                        frame.c_y = static_cast<std::uint8_t>(std::clamp(unique->cstick_y, 0, 255));
                        frame.trig_l = static_cast<std::uint8_t>(std::clamp(unique->trigger_x, 0, 255));
                        frame.trig_r = static_cast<std::uint8_t>(std::clamp(unique->trigger_y, 0, 255));
                        out.origin.initial_input = frame;
                    }
                } else if (candidate->source_input_frame_id.has_value()) {
                    if (const auto input = analysis->GetAnalysisInputFrame(*candidate->source_input_frame_id); input.has_value()) {
                        savor::GCInputFrame frame{};
                        frame.main_x = static_cast<std::uint8_t>(std::clamp(input->main_x, 0, 255));
                        frame.main_y = static_cast<std::uint8_t>(std::clamp(input->main_y, 0, 255));
                        frame.c_x = static_cast<std::uint8_t>(std::clamp(input->cstick_x, 0, 255));
                        frame.c_y = static_cast<std::uint8_t>(std::clamp(input->cstick_y, 0, 255));
                        frame.trig_l = static_cast<std::uint8_t>(std::clamp(input->trigger_x, 0, 255));
                        frame.trig_r = static_cast<std::uint8_t>(std::clamp(input->trigger_y, 0, 255));
                        out.origin.initial_input = frame;
                    }
                }
            }
        }

        out.turns.reserve(chain.size());
        for (auto& row : chain) {
            BattleReplicationTurn turn{};
            turn.row = std::move(row);
            if (turn.row.resolved_turn_commands_blob.has_value() && !turn.row.resolved_turn_commands_blob->empty()) {
                if (auto commands = soa::battle::actions::decode_battle_turn_commands_hex(*turn.row.resolved_turn_commands_blob); commands.has_value()) {
                    turn.commands = std::move(*commands);
                    turn.command_decode_ok = true;
                }
            }
            out.turns.push_back(std::move(turn));
        }
        return ServiceResult<BattleReplicationDetails>::Ok(std::move(out));
    }

private:
    static savor::db::IUiReadDb* UiReadDb() {
        return savorqt::SavorDbRuntime::instance().uiReadDb();
    }

    static savor::db::IExecutionDb* ExecutionDb() {
        return savorqt::SavorDbRuntime::instance().executionDb();
    }

    static savor::db::IStateDb* StateDb() {
        return savorqt::SavorDbRuntime::instance().stateDb();
    }

    static savor::db::IAnalysisDb* AnalysisDb() {
        return savorqt::SavorDbRuntime::instance().analysisDb();
    }

    template <typename T>
    static ServiceResult<T> Unavailable(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::Unavailable, std::move(message) });
    }

    template <typename T>
    static ServiceResult<T> NotFound(std::string message) {
        return ServiceResult<T>::Err({ ServiceErrorKind::NotFound, std::move(message) });
    }
};

} // namespace savorqt::db
