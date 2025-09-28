// SimCore/DB/ProgramDB/ExplorerRunDBCodec.cpp
#include "ExplorerRunDBCodec.h"
#include "IniKV.h"
#include "../DBCore/CoordinatorClock.h"
#include "../DBCore/DbResult.h"
#include "../DBCore/DbEnv.h"
#include "../DBCore/ObjectStore.h"

#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../ExplorerSettingsPredicateRepo.h"
#include "../PredicateSpecRepo.h"
#include "../BattlePlanRepo.h"
#include "../BattlePlanAtomRepo.h"
#include "../BattlePlanTurnRepo.h"
#include "../AddressProgramRepo.h"
#include "../ExplorerRunRepo.h"

#include "../../Phases/Programs/ProgramRegistry.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"
#include "../../Runner/Script/PSContext.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Utils/Hash.h"

#include <sqlite3.h>
#include <optional>
#include <string>
#include <vector>
#include <cstdint>

using simcore::db::BattlePlanAtomRepo;
using simcore::db::BattlePlanTurnRepo;
using simcore::db::PredicateSpecRepo;
using simcore::db::PredicateSpecRow;
using simcore::db::AddressProgramRepo;
using simcore::db::ExplorerRunRepo;
using soa::battle::actions::BattlePath;
using soa::battle::actions::TurnPlan;
using soa::battle::actions::ActionPlan;
using soa::battle::actions::BattleAction;

static constexpr int kPK = PK_BattleTurnRunner; // from Wire.h
static constexpr int kProgramVersion = phase::battle::runner::PayloadVersion;       // from BattleRunnerPayload.h

static inline DbResult<void> insert_job_event(DbEnv& env, int64_t job_id, const char* kind, const std::string& payload) {
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO job_events(job_id, event_kind, payload) VALUES(?,?,?);",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc),rc,"prepare event" });
    sqlite3_bind_int64(st, 1, job_id);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
    if (!payload.empty()) sqlite3_bind_text(st, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(st, 3);
    rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc),rc,"insert event" });
    return DbResult<void>::Ok();
}

static inline DbResult<int64_t> insert_job(DbEnv& env, int64_t job_set_id, int64_t program_ref_id, int priority,
    const std::string& fingerprint) {
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO jobs(job_set_id, program_kind, program_version, program_ref_id, fingerprint, priority, state, attempts, max_attempts, queued_at) "
        "VALUES(?,?,?,?,?,?, 'QUEUED', 0, 1, strftime('%s','now'));",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc),rc,"prepare jobs" });
    sqlite3_bind_int64(st, 1, job_set_id);
    sqlite3_bind_int(st, 2, kPK);
    sqlite3_bind_int(st, 3, kProgramVersion);
    sqlite3_bind_int64(st, 4, program_ref_id);
    sqlite3_bind_text(st, 5, fingerprint.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, priority);
    rc = sqlite3_step(st);
    if (rc != SQLITE_DONE) { int ec = rc; sqlite3_finalize(st); return DbResult<int64_t>::Err({ map_sqlite_err(ec),ec,"insert jobs" }); }
    int64_t job_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(st);
    return DbResult<int64_t>::Ok(job_id);
}

static inline DbResult<void> set_job_state(DbEnv& env, int64_t job_id, const char* state) {
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    int rc = sqlite3_prepare_v2(db, "UPDATE jobs SET state=? WHERE job_id=?;", -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<void>::Err({ map_sqlite_err(rc),rc,"prepare state" });
    sqlite3_bind_text(st, 1, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, job_id);
    rc = sqlite3_step(st); sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return DbResult<void>::Err({ map_sqlite_err(rc),rc,"update state" });
    return DbResult<void>::Ok();
}

static inline DbResult<std::string> get_enqueued_blueprint(DbEnv& env, int64_t job_id) {
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    int rc = sqlite3_prepare_v2(db,
        "SELECT payload FROM job_events WHERE job_id=? AND event_kind='ENQUEUED' ORDER BY event_id ASC LIMIT 1;",
        -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<std::string>::Err({ map_sqlite_err(rc),rc,"prepare get ENQUEUED" });
    sqlite3_bind_int64(st, 1, job_id);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<std::string>::Err({ DbErrorKind::NotFound, rc, "no ENQUEUED" }); }
    const unsigned char* txt = sqlite3_column_text(st, 0);
    std::string out = txt ? reinterpret_cast<const char*>(txt) : std::string();
    sqlite3_finalize(st);
    return DbResult<std::string>::Ok(std::move(out));
}

static inline DbResult<int64_t> get_job_program_ref(DbEnv& env, int64_t job_id) {
    sqlite3* db = env.handle();
    sqlite3_stmt* st{};
    int rc = sqlite3_prepare_v2(db, "SELECT program_ref_id FROM jobs WHERE job_id=?;", -1, &st, nullptr);
    if (rc != SQLITE_OK) return DbResult<int64_t>::Err({ map_sqlite_err(rc),rc,"prepare ref" });
    sqlite3_bind_int64(st, 1, job_id);
    rc = sqlite3_step(st);
    if (rc != SQLITE_ROW) { sqlite3_finalize(st); return DbResult<int64_t>::Err({ DbErrorKind::NotFound, rc, "job missing" }); }
    int64_t ref = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return DbResult<int64_t>::Ok(ref);
}

static inline std::optional<std::string> kv_get(const std::vector<std::pair<std::string, std::string>>& kv, const char* key) {
    for (auto& p : kv) if (p.first == key) return p.second;
    return std::nullopt;
}

DbResult<int64_t> ExplorerRunDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& controls_ini)
{
    auto js = JobSetsRepo::Get(job_set_id);
    if (!js.ok) return DbResult<int64_t>::Err(js.error);
    if (!js.value.domain_ref_id) return DbResult<int64_t>::Err({ .kind = DbErrorKind::NotFound, .message="job_set.domain_ref_id missing" });
    const int64_t run_id = *js.value.domain_ref_id;

    auto run = simcore::db::ExplorerRunRepo::Get(run_id);
    if (!run.ok) return DbResult<int64_t>::Err(run.error);
    const int64_t settings_id = run.value.settings_id;

    IniKV kv = IniKV::parse(controls_ini);
    const int64_t plan_id = kv.get_i64("plan_id", 0);
    const int64_t delta_seed_id = kv.get_i64("delta_seed_id", 0);
    const int priority = (int)kv.get_i64("priority", 0);
    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    if (!plan_id || !delta_seed_id) return DbResult<int64_t>::Err({ .kind=DbErrorKind::NotFound, .message="plan_id and delta_seed_id required" });

    auto planRow = BattlePlanRepo::Get(plan_id);
    if (!planRow.ok) return DbResult<int64_t>::Err(planRow.error);
    if (planRow.value.settings_id != settings_id) return DbResult<int64_t>::Err({ .kind = DbErrorKind::InvalidArgument, .message = "plan_id does not belong to explorer_settings" });

    IniKV norm;
    norm.add("delta_seed_id", std::to_string(delta_seed_id));
    norm.add("plan_id", std::to_string(plan_id));
    norm.add("run_ms", std::to_string(run_ms));
    norm.add("vi_stall_ms", std::to_string(vi_stall_ms));
    if (priority) norm.add("priority", std::to_string(priority));
    const std::string norm_txt = norm.to_string_sorted();

    std::string to_hash = "ExplorerRun|" + std::to_string(kProgramVersion) + "|" +
        std::to_string(run_id) + "|" + std::to_string(plan_id) + "|" + std::to_string(delta_seed_id) + "|" +
        std::to_string(run_ms) + "|" + std::to_string(vi_stall_ms);
    const std::string fingerprint = hash::sha256(to_hash.data(), to_hash.size());

    auto cj = JobsRepo::CreateOrGetByFingerprint(job_set_id, kPK, kProgramVersion, run_id, fingerprint, priority, norm_txt);
    if (!cj.ok) return DbResult<int64_t>::Err(cj.error);

    auto ev = JobEventsRepo::Append(cj.value, "ENQUEUED", norm_txt);
    if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

    return DbResult<int64_t>::Ok(cj.value);
}

DbResult<simcore::PSJob> ExplorerRunDBCodec::decode_job_from_db(int64_t job_id)
{
    auto jr = JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    const int64_t run_id = jr.value.program_ref_id;

    auto run = simcore::db::ExplorerRunRepo::Get(run_id);
    if (!run.ok) return DbResult<simcore::PSJob>::Err(run.error);
    const int64_t settings_id = run.value.settings_id;

    IniKV kv;
    if (jr.value.vm_kv) kv = IniKV::parse(*jr.value.vm_kv);
    else {
        auto enq = JobEventsRepo::GetFirstPayload(job_id, "ENQUEUED");
        if (!enq.ok) return DbResult<simcore::PSJob>::Err(enq.error);
        if (!enq.value) return DbResult<simcore::PSJob>::Err({ .kind = DbErrorKind::NotFound, .message = "missing ENQUEUED payload" });
        kv = IniKV::parse(*enq.value);
    }

    const int64_t plan_id = kv.get_i64("plan_id", 0);
    const int64_t delta_seed_id = kv.get_i64("delta_seed_id", 0);
    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    if (!plan_id || !delta_seed_id) return DbResult<simcore::PSJob>::Err({ .kind = DbErrorKind::NotFound, .message = "plan_id and delta_seed_id required" });

    auto turnsR = simcore::db::BattlePlanTurnRepo::LoadTurnsByPlan(plan_id);
    if (!turnsR.ok) return DbResult<simcore::PSJob>::Err(turnsR.error);

    BattlePath path;
    for (auto& t : turnsR.value) {
        TurnPlan plan{ .fake_attack_count = t.fake_atk_count };
        auto actorsR = simcore::db::BattlePlanTurnRepo::ListActorsByPlan(plan_id, t.turn_index);
        if (!actorsR.ok) return DbResult<simcore::PSJob>::Err(actorsR.error);
        for (auto& a : actorsR.value) {
            auto atom = simcore::db::BattlePlanAtomRepo::Get(a.atom_id);
            if (!atom.ok) return DbResult<simcore::PSJob>::Err(atom.error);
            ActionPlan ap{ .actor_slot = atom.value.actor_slot, .macro = (BattleAction)atom.value.action_type };
            // add target if valid
            if (atom.value.target_slot > -1 && atom.value.target_slot < 12) ap.params.target_slot = atom.value.target_slot;
            // add item id if valid
            if (atom.value.param_item_id >= 0) ap.params.item_id = atom.value.param_item_id;
            plan.spec.push_back(std::move(ap));
        }
        path.push_back(std::move(plan));
    }

    std::vector<simcore::pred::Spec> preds;
    auto plist = simcore::db::ExplorerSettingsPredicateRepo::List(settings_id);
    if (!plist.ok) return DbResult<simcore::PSJob>::Err(plist.error);
    preds.reserve(plist.value.size());
    for (auto& r : plist.value) {
        auto p = simcore::db::PredicateSpecRepo::Get(r.predicate_id);
        if (!p.ok) return DbResult<simcore::PSJob>::Err(p.error);
        std::vector<uint8_t> lhs_prog, rhs_prog;
        if (p.value.lhs_prog_id.has_value()) {
            auto lhs_prog_row = simcore::db::AddressProgramRepo::Get(p.value.lhs_prog_id.value());
            if (!lhs_prog_row.ok) return DbResult<simcore::PSJob>::Err(lhs_prog_row.error);
            lhs_prog = std::move(lhs_prog_row.value.prog_bytes);
        }
        if (p.value.rhs_prog_id.has_value()) {
            auto rhs_prog_row = simcore::db::AddressProgramRepo::Get(p.value.rhs_prog_id.value());
            if (!rhs_prog_row.ok) return DbResult<simcore::PSJob>::Err(rhs_prog_row.error);
            rhs_prog = std::move(rhs_prog_row.value.prog_bytes);
        }
        simcore::pred::Spec spec{
            .id = r.ordinal,
            .required_bp = p.value.required_bp,
            .kind = (simcore::pred::PredKind)p.value.kind,
            .width = p.value.width,
            .cmp = (simcore::pred::CmpOp)p.value.cmp_op,
            .flags = p.value.flags & 0xFF,
            .lhs_addr = p.value.lhs_addr,
            .lhs_key = (addr::AddrKey)p.value.lhs_key.value(),
            .rhs_value = p.value.rhs_value,
            .rhs_key = (addr::AddrKey)p.value.rhs_key.value(),
            .turn_mask = p.value.turn_mask,
            .lhs_prog = lhs_prog,
            .rhs_prog = rhs_prog,
            .desc = p.value.description
        };
        preds.push_back(std::move(spec));
    }

    phase::battle::runner::EncodeSpec spec;
    spec.run_ms = run_ms;
    spec.vi_stall_ms = vi_stall_ms;
    spec.initial = {}; // TODO: integrate delta_seed into initial RNG if/when exposed
    spec.path = std::move(path);
    spec.predicates = std::move(preds);

    simcore::PSJob out{};
    phase::battle::runner::encode_payload(spec, out.payload);
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> ExplorerRunDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line)
{
    auto r = JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!r.ok) return DbResult<void>::Err(r.error);
    return DbResult<void>::Ok();
}

DbResult<void> ExplorerRunDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success)
{
    auto st = JobsRepo::SetState(job_id, success ? "SUCCEEDED" : "FAILED");
    if (!st.ok) return DbResult<void>::Err(st.error);

    auto ev = JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!ev.ok) return DbResult<void>::Err(ev.error);

    if (success) {
        auto jr = JobsRepo::Get(job_id);
        if (!jr.ok) return DbResult<void>::Err(jr.error);
        const int64_t run_id = jr.value.program_ref_id;

        auto s1 = simcore::db::ExplorerRunRepo::SetResultsIni(run_id, results_ini);
        if (!s1.ok) return DbResult<void>::Err(s1.error);

        auto lines = JobEventsRepo::ListByJobAndKind(job_id, "PROGRESS");
        if (!lines.ok) return DbResult<void>::Err(lines.error);
        std::string transcript;
        for (size_t i = 0; i < lines.value.size(); ++i) {
            if (i) transcript.push_back('\n');
            if (lines.value[i].payload) transcript.append(*lines.value[i].payload);
        }
        auto art = simcore::db::ObjectStore::PutText("text/plain", transcript, R"({"kind":"ExplorerRunProgress"})");
        if (!art.ok) return DbResult<void>::Err(art.error);

        auto s2 = simcore::db::ExplorerRunRepo::SetProgressLogArtifactId(run_id, art.value.id);
        if (!s2.ok) return DbResult<void>::Err(s2.error);

        auto md = simcore::db::ExplorerRunRepo::MarkDone(run_id);
        if (!md.ok) return DbResult<void>::Err(md.error);
    }
    return DbResult<void>::Ok();
}

DbResult<std::string> ExplorerRunDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    std::string out;

    if (job_id) {
        auto jr = JobsRepo::Get(*job_id);
        if (!jr.ok) return DbResult<std::string>::Err(jr.error);
        auto run = simcore::db::ExplorerRunRepo::Get(jr.value.program_ref_id);
        if (!run.ok) return DbResult<std::string>::Err(run.error);

        auto rows = JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) {
            if (i) out.push_back('\n');
            if (rows.value[i].payload) out.append(*rows.value[i].payload);
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    if (job_set_id) {
        auto rows = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        int64_t cur_job{ -1 };
        for (auto& e : rows.value) {
            if (e.job_id != cur_job) {
                if (!out.empty()) out.push_back('\n');
                out.append("[job_id ");
                out.append(std::to_string(e.job_id));
                out.append("]");
                cur_job = e.job_id;
            }
            out.push_back('\n');
            if (e.payload) out.append(*e.payload);
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    return DbResult<std::string>::Err({ .kind = DbErrorKind::NotFound, .message = "must provide job_id or job_set_id" });
}

DbResult<std::string> ExplorerRunDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    if (job_id) {
        auto s = JobEventsRepo::GetLatestPayload(*job_id, "RESULTS");
        if (!s.ok) return DbResult<std::string>::Err(s.error);
        return DbResult<std::string>::Ok(s.value.value_or(std::string{}));
    }

    if (job_set_id) {
        auto rows = JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        std::string out;
        int64_t cur_job{ -1 };
        for (auto& e : rows.value) {
            if (e.job_id != cur_job) {
                if (!out.empty()) out.push_back('\n');
                out.append("[job_id ");
                out.append(std::to_string(e.job_id));
                out.append("]");
                cur_job = e.job_id;
            }
            out.push_back('\n');
            if (e.payload) out.append(*e.payload);
        }
        return DbResult<std::string>::Ok(std::move(out));
    }

    return DbResult<std::string>::Err({ .kind = DbErrorKind::NotFound, .message = "must provide job_id or job_set_id" });
}

DbResult<std::optional<int64_t>> ExplorerRunDBCodec::get_required_savestate_id(int64_t /*job_id*/) {
    return DbResult<std::optional<int64_t>>::Ok(std::nullopt);
}

DbResult<simcore::PSInit> ExplorerRunDBCodec::build_psinit_for_job(int64_t /*job_id*/) {
    simcore::PSInit init{};
    init.savestate_path.clear();
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> ExplorerRunDBCodec::build_results_ini_from_prresult(int64_t /*job_id*/, const simcore::PRResult& r) {
    IniKV kv;
    kv.add("ok", r.ps.ok ? "1" : "0");
    kv.add("w_err", std::to_string(static_cast<unsigned>(r.ps.w_err)));
    return DbResult<std::string>::Ok(kv.to_string_sorted());
}
