#include "DBTriggerEngine.h"
#include "../../DB/Scheduling/TriggersRepo.h"
#include "../../DB/Scheduling/JobSetsRepo.h"
#include "../../DB/Scheduling/JobsRepo.h"
#include "../../Phases/Programs/ProgramRegistry.h"
#include "../../DB/ProgramDB/IProgramDBCodec.h"
#include "../../DB/ProgramDB/ExplorerRunDBCodec.h"
#include "../../DB/Scheduling/JobEventsRepo.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Utils/IniDoc.h"
#include <sqlite3.h>
#include <algorithm>
#include <limits>

using simcore::db::DbResult;
using simcore::db::JobsRepo;
using simcore::db::JobSetsRepo;
using simcore::db::TriggersRepo;

namespace {

    static DbResult<bool> condition_each_job_terminal(const IniKV& cond, const simcore::db::JobRow& jr) {
        const bool success_only = cond.get_i64("success_only", 0) != 0;
        if (success_only) {
            return DbResult<bool>::Ok(jr.state == "SUCCEEDED");
        }
        // This engine is only called after terminal write; treat as satisfied.
        return DbResult<bool>::Ok(true);
    }

    static DbResult<bool> condition_all_succeeded(int64_t job_set_id) {
        return simcore::db::DBService::instance().submit_res<bool>(simcore::db::OpType::Read, simcore::db::Priority::Normal, {},
            [=](simcore::db::DbEnv& env)->DbResult<bool> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st = nullptr;
                const char* sql =
                    "SELECT succeeded,total FROM v_job_set_progress_h WHERE job_set_id=?";
                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                    return DbResult<bool>::Err({ simcore::db::map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare v_job_set_progress_h" });
                }
                sqlite3_bind_int64(st, 1, job_set_id);
                bool ok = false;
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const int64_t succ = sqlite3_column_int64(st, 0);
                    const int64_t total = sqlite3_column_int64(st, 1);
                    ok = (total > 0 && succ == total);
                }
                sqlite3_finalize(st);
                return DbResult<bool>::Ok(ok);
            }).get();
    }

    // Returns true when the job set has finished processing (all jobs in any terminal state)
    static DbResult<bool> condition_all_finished(int64_t job_set_id) {
        return simcore::db::DBService::instance().submit_res<bool>(simcore::db::OpType::Read, simcore::db::Priority::Normal, {},
            [=](simcore::db::DbEnv& env)->DbResult<bool> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st = nullptr;

                const char* sql =
                    "SELECT terminal,total FROM v_job_set_progress_h WHERE job_set_id=?";

                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                    return DbResult<bool>::Err({
                        simcore::db::map_sqlite_err(sqlite3_errcode(db)),
                        sqlite3_errcode(db),
                        "prepare v_job_set_progress_h (ALL_FINISHED)"
                        });
                }

                if (sqlite3_bind_int64(st, 1, job_set_id) != SQLITE_OK) {
                    sqlite3_finalize(st);
                    return DbResult<bool>::Err({
                        simcore::db::map_sqlite_err(sqlite3_errcode(db)),
                        sqlite3_errcode(db),
                        "bind job_set_id (ALL_FINISHED)"
                        });
                }

                bool ok = false;
                const int rc = sqlite3_step(st);
                if (rc == SQLITE_ROW) {
                    const auto terminal = sqlite3_column_int64(st, 0);
                    const auto total = sqlite3_column_int64(st, 1);
                    ok = (total > 0 && terminal == total);
                }
                else if (rc == SQLITE_DONE) {
                    ok = false; // no row for this job_set_id
                }
                else {
                    auto err = DbResult<bool>::Err({
                        simcore::db::map_sqlite_err(sqlite3_errcode(db)),
                        sqlite3_errcode(db),
                        "step v_job_set_progress (ALL_FINISHED)"
                        });
                    sqlite3_finalize(st);
                    return err;
                }

                sqlite3_finalize(st);
                return DbResult<bool>::Ok(ok);
            }
        ).get();
    }

    static DbResult<bool> condition_winners_complete(int64_t job_set_id, const IniKV& cond) {
        return simcore::db::DBService::instance().submit_res<bool>(simcore::db::OpType::Read, simcore::db::Priority::Normal, {},
            [=](simcore::db::DbEnv& env)->DbResult<bool> {
                sqlite3* db = env.handle();
                sqlite3_stmt* st = nullptr;
                const char* sql =
                    "SELECT winners_found,expected_total FROM v_winners_progress_h WHERE job_set_id=?";
                if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) {
                    return DbResult<bool>::Err({ simcore::db::map_sqlite_err(sqlite3_errcode(db)), sqlite3_errcode(db), "prepare v_winners_progress_h" });
                }
                sqlite3_bind_int64(st, 1, job_set_id);
                bool ok = false;
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const int64_t found = sqlite3_column_int64(st, 0);
                    const int64_t expected = sqlite3_column_int64(st, 1);
                    const int64_t min_req = (int64_t)cond.get_i64("min_winners", expected);
                    ok = (expected > 0 && found >= min_req && found == expected);
                }
                sqlite3_finalize(st);
                return DbResult<bool>::Ok(ok);
            }).get();
    }

    static DbResult<void> handle_trigger(const simcore::db::TriggerRow& t, const simcore::db::JobRow& jr) {
        IniKV cond = IniDoc::parse(t.condition).section_kv(IniDoc::GLOBAL);
        bool satisfied = false;

        std::string type = cond.get("type");
        if (type == "EACH_JOB_TERMINAL") {
            auto r = condition_each_job_terminal(cond, jr);
            if (!r.ok) return DbResult<void>::Err(r.error);
            satisfied = r.value;
        }
        else if (type == "ALL_SUCCEEDED") {
            auto r = condition_all_succeeded(jr.job_set_id);
            if (!r.ok) return DbResult<void>::Err(r.error);
            satisfied = r.value;
        }
        else if (type == "WINNERS_COMPLETE") {
            auto r = condition_winners_complete(jr.job_set_id, cond);
            if (!r.ok) return DbResult<void>::Err(r.error);
            satisfied = r.value;
        }
        else if (type == "ALL_FINISHED") {
            auto r = condition_all_finished(jr.job_set_id);
            if (!r.ok) return DbResult<void>::Err(r.error);
            satisfied = r.value;
        }
        else {
            // Unknown condition => ignore.
            return DbResult<void>::Ok();
        }

        if (!satisfied) return DbResult<void>::Ok();

        auto deact = TriggersRepo::TryDeactivate(t.trigger_id);
        if (!deact.ok) return DbResult<void>::Err(deact.error);
        if (!deact.value) return DbResult<void>::Ok(); // lost the race


        auto& codec = ProgramDBCodecRegistry::for_kind(t.action_kind);

        simcore::TriggerCtx ctx{};
        ctx.prev_job_id = jr.job_id;
        ctx.prev_job_set_id = jr.job_set_id;
        ctx.prev_program_kind = jr.program_kind;
        ctx.prev_success = (jr.state == "SUCCEEDED");
        ctx.scope = t.scope.c_str();

        auto r = codec.phase_setup_on_trigger(ctx, t.action_args);
        if (!r.ok) return DbResult<void>::Err(r.error);

        return DbResult<void>::Ok();
    }

} // anon

namespace simcore {

    static uint32_t double_u32_limit(uint32_t v) {
        if (v == 0) return 0;
        const uint64_t doubled = static_cast<uint64_t>(v) * 2ull;
        return static_cast<uint32_t>((std::min)(doubled, static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())));
    }

    static DbResult<void> maybe_expand_limits_for_timeout_retry(const simcore::db::JobRow& jr, std::string& detail) {
        if (!jr.vm_kv.has_value() || jr.vm_kv->empty()) return DbResult<void>::Ok();

        auto latest = simcore::db::JobEventsRepo::GetLatestPayload(jr.job_id, "RESULTS");
        if (!latest.ok || !latest.value.has_value() || latest.value->empty()) return DbResult<void>::Ok();

        IniDoc result_doc = IniDoc::parse(*latest.value);
        uint32_t dw_err = static_cast<uint32_t>(simcore::RunToBpOutcome::Unknown);
        if (result_doc.has_section("BattleSingleTurn.Results")) {
            dw_err = result_doc.section_kv("BattleSingleTurn.Results").get_u32("dw_err", dw_err);
        }
        else if (result_doc.has_section(simcore::db::codec::battle::run::ResultsIni::SECTION_NAME)) {
            dw_err = result_doc.section_kv(simcore::db::codec::battle::run::ResultsIni::SECTION_NAME).get_u32("dw_err", dw_err);
        }
        else {
            return DbResult<void>::Ok();
        }

        const bool hit_timeout = dw_err == static_cast<uint32_t>(simcore::RunToBpOutcome::Timeout);
        const bool hit_vi_stall = dw_err == static_cast<uint32_t>(simcore::RunToBpOutcome::ViStalled);
        if (!hit_timeout && !hit_vi_stall) return DbResult<void>::Ok();

        IniDoc vm_doc = IniDoc::parse(*jr.vm_kv);
        if (!vm_doc.has_section(simcore::db::codec::battle::run::BlueprintIni::SECTION_NAME)) return DbResult<void>::Ok();

        auto bp = simcore::db::codec::battle::run::BlueprintIni::from_section(vm_doc);
        const uint32_t old_run = bp.run_ms;
        const uint32_t old_vi = bp.vi_stall_ms;

        if (hit_timeout) bp.run_ms = double_u32_limit(bp.run_ms);
        if (hit_vi_stall) bp.vi_stall_ms = double_u32_limit(bp.vi_stall_ms);

        if (bp.run_ms == old_run && bp.vi_stall_ms == old_vi) return DbResult<void>::Ok();

        bp.set_section(vm_doc);
        auto set = simcore::db::JobsRepo::SetVmKv(jr.job_id, vm_doc.to_string_sorted());
        if (!set.ok) return DbResult<void>::Err(set.error);

        if (hit_timeout) detail += "run_ms:" + std::to_string(old_run) + "->" + std::to_string(bp.run_ms) + " ";
        if (hit_vi_stall) detail += "vi_stall_ms:" + std::to_string(old_vi) + "->" + std::to_string(bp.vi_stall_ms);
        return DbResult<void>::Ok();
    }

    static DbResult<bool> maybe_auto_retry_failed_explorer(const simcore::db::JobRow& jr) {
        if (jr.program_kind != simcore::PK_BattleSingleTurnRunner) return DbResult<bool>::Ok(false);
        if (jr.state != "FAILED") return DbResult<bool>::Ok(false);
        if (jr.attempts >= jr.max_attempts) return DbResult<bool>::Ok(false);

        std::string detail;
        auto tune = maybe_expand_limits_for_timeout_retry(jr, detail);
        if (!tune.ok) return DbResult<bool>::Err(tune.error);

        auto rr = JobsRepo::Requeue(jr.job_id);
        if (!rr.ok) return DbResult<bool>::Err(rr.error);

        std::string payload = "attempt " + std::to_string(jr.attempts) + "/" + std::to_string(jr.max_attempts);
        if (!detail.empty()) payload += " | adjusted " + detail;
        (void)simcore::db::JobEventsRepo::Append(jr.job_id, "AUTO_RETRY", payload);
        return DbResult<bool>::Ok(true);
    }

    DbResult<void> TriggerEngine::after_terminal(int64_t job_id) {
        auto jres = JobsRepo::Get(job_id);
        if (!jres.ok) return DbResult<void>::Err(jres.error);
        const auto jr = jres.value;

        auto retry = maybe_auto_retry_failed_explorer(jr);
        if (!retry.ok) return DbResult<void>::Err(retry.error);
        if (retry.value) return DbResult<void>::Ok();

        // Job-scoped triggers
        {
            auto list = TriggersRepo::ListActiveByJob(job_id);
            if (!list.ok) return DbResult<void>::Err(list.error);
            for (auto& t : list.value) {
                auto r = handle_trigger(t, jr);
                if (!r.ok) return r;
            }
        }
        // Job-set-scoped triggers
        {
            auto list = TriggersRepo::ListActiveByJobSet(jr.job_set_id);
            if (!list.ok) return DbResult<void>::Err(list.error);
            for (auto& t : list.value) {
                auto r = handle_trigger(t, jr);
                if (!r.ok) return r;
            }
        }
        // Evaluate triggers attached to ancestor job_sets (parent -> ... -> root)
        {
            auto pid = JobSetsRepo::GetParent(jr.job_set_id);
            if (!pid.ok) return DbResult<void>::Err(pid.error);
            std::optional<int64_t> cur = pid.value;
            while (cur.has_value()) {
                auto list = TriggersRepo::ListActiveByJobSet(*cur);
                if (!list.ok) return DbResult<void>::Err(list.error);
                for (auto& t : list.value) {
                    auto r = handle_trigger(t, jr); // handle_trigger uses t.scope='job_set' and will read progress for t.scope_id
                    if (!r.ok) return r;
                }
                auto next = JobSetsRepo::GetParent(*cur);
                if (!next.ok) return DbResult<void>::Err(next.error);
                cur = next.value;
            }
        }
        return DbResult<void>::Ok();
    }

} // namespace simcore
