#include "BattleContextDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../DBCore/DbResult.h"
#include "../DBCore/ObjectStore.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../BattleContextRepo.h"
#include "../ExplorerRunRepo.h"
#include "../DeltaSeedRepo.h"
#include "../SavestateRepo.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Phases/Programs/BattleContext/BattleContextPayload.h"
#include "../../Phases/Programs/BattleContext/BattleContextScript.h"
#include "../../Runner/Script/PhaseScriptVM.h"
#include "../../Runner/Script/PSContext.h"
#include "../../Phases/Programs/ProgramRegistry.h"
#include "../../Runner/Script/KeyRegistry.h"
#include "../../Runner/Parallel/DB/DBTriggerEngine.h"

using simcore::db::DbResult;
using simcore::TriggerCtx;

static constexpr int kPK = PK_BattleContextProbe;
static constexpr int kPV = 1;

DbResult<int64_t> BattleContextDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    IniKV kv = IniDoc::parse(blueprint_ini).section_kv(IniDoc::GLOBAL);

    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    const int priority = (int)kv.get_i64("priority", 0);
    const uint64_t savestate_id = kv.get_i64("savestate_id", 0);

    auto save_found = simcore::db::SavestateRepo::Get(savestate_id);
    if (!save_found.ok) return DbResult<int64_t>::Err(save_found.error);
    if (!save_found.value.has_value()) return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "savestate_id is required to make a battle context" });
    auto savestate_row = save_found.value.value();
    if (!savestate_row.complete || savestate_row.savestate_type != db::SavestateType::BATTLE) return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "a complete battle savestate is required" });

    IniKV norm;
    norm.add("run_ms", std::to_string(run_ms));
    norm.add("vi_stall_ms", std::to_string(vi_stall_ms));
    if (priority) norm.add("priority", std::to_string(priority));
    const std::string vmkv = norm.to_string_sorted();

    std::string fp = "PK=" + std::to_string(kPK) + ";PV=" + std::to_string(kPV)
        + ";run_ms=" + std::to_string(run_ms)
        + ";vi=" + std::to_string(vi_stall_ms);

    auto ins = simcore::db::JobsRepo::CreateOrGetByFingerprint(job_set_id, kPK, kPV, /*program_ref_id*/0, fp, priority, vmkv);
    if (!ins.ok) return DbResult<int64_t>::Err(ins.error);

    auto ev = simcore::db::JobEventsRepo::Append(ins.value, "ENQUEUED", vmkv);
    if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

    return DbResult<int64_t>::Ok(ins.value);
}

DbResult<simcore::PSJob> BattleContextDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);

    IniKV kv;
    if (jr.value.vm_kv) kv = IniDoc::parse(*jr.value.vm_kv).section_kv(IniDoc::GLOBAL);
    else {
        auto enq = simcore::db::JobEventsRepo::GetFirstPayload(job_id, "ENQUEUED");
        if (!enq.ok) return DbResult<simcore::PSJob>::Err(enq.error);
        if (!enq.value) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::NotFound, 0, "missing ENQUEUED payload" });
        kv = IniDoc::parse(*enq.value).section_kv(IniDoc::GLOBAL);
    }

    const uint32_t run_ms = kv.get_u32("run_ms", 0);
    const uint32_t vi_stall_ms = kv.get_u32("vi_stall_ms", 0);

    phase::battle::ctx::EncodeSpec es{};
    es.run_ms = run_ms;
    es.vi_stall_ms = vi_stall_ms;

    std::vector<uint8_t> payload;
    phase::battle::ctx::encode_payload(es, payload);

    simcore::PSJob job{};
    job.payload = std::move(payload);
    return DbResult<simcore::PSJob>::Ok(job);
}

DbResult<void> BattleContextDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line) {
    auto ev = simcore::db::JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!ev.ok) return DbResult<void>::Err(ev.error);
    return DbResult<void>::Ok();
}

DbResult<void> BattleContextDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) {
    auto st = simcore::db::JobsRepo::SetState(job_id, success ? "SUCCEEDED" : "FAILED");
    if (!st.ok) return DbResult<void>::Err(st.error);
    auto ev = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!ev.ok) return DbResult<void>::Err(ev.error);
    return DbResult<void>::Ok();
}

DbResult<std::string> BattleContextDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    std::string out;
    if (job_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) { if (i) out.push_back('\n'); if (rows.value[i].payload) out.append(*rows.value[i].payload); }
        return DbResult<std::string>::Ok(out);
    }
    if (job_set_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) { if (i) out.push_back('\n'); if (rows.value[i].payload) out.append(*rows.value[i].payload); }
        return DbResult<std::string>::Ok(out);
    }
    return DbResult<std::string>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "need job_id or job_set_id" });
}

DbResult<std::string> BattleContextDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    std::string out;
    if (job_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobAndKind(*job_id, "RESULTS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) { if (i) out.push_back('\n'); if (rows.value[i].payload) out.append(*rows.value[i].payload); }
        return DbResult<std::string>::Ok(out);
    }
    if (job_set_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (size_t i = 0; i < rows.value.size(); ++i) { if (i) out.push_back('\n'); if (rows.value[i].payload) out.append(*rows.value[i].payload); }
        return DbResult<std::string>::Ok(out);
    }
    return DbResult<std::string>::Err({ simcore::db::DbErrorKind::InvalidArgument, 0, "need job_id or job_set_id" });
}

simcore::db::DbResult<std::optional<int64_t>> BattleContextDBCodec::get_required_savestate_id(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::optional<int64_t>>::Err(jr.error);

    IniKV kv;
    if (jr.value.vm_kv) kv = IniDoc::parse(*jr.value.vm_kv).section_kv(IniDoc::GLOBAL);

    return simcore::db::DbResult<std::optional<int64_t>>::Ok(std::optional<int64_t>(kv.get_i64("savestate_id", 0)));
}

simcore::db::DbResult<simcore::PSInit> BattleContextDBCodec::build_psinit_for_job(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSInit>::Err(jr.error);

    IniKV kv;
    if (jr.value.vm_kv) kv = IniDoc::parse(*jr.value.vm_kv).section_kv(IniDoc::GLOBAL);

    auto temp_savestate_path = ObjectStore::MaterializeToTemp(kv.get_i64("savestate_id"));
    if (!temp_savestate_path.ok) return simcore::db::DbResult<simcore::PSInit>::Err(temp_savestate_path.error);

    simcore::PSInit init{};
    init.savestate_path = temp_savestate_path.value;
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return simcore::db::DbResult<simcore::PSInit>::Ok(init);
}

simcore::db::DbResult<std::string> BattleContextDBCodec::build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) {
    IniKV kv;
    kv.add("ok", r.ps.ok ? "1" : "0");
    kv.add("w_err", std::to_string(static_cast<unsigned>(r.ps.w_err)));

    std::string blob;
    if (r.ps.ctx.get<std::string>(simcore::keys::battle::CTX_BLOB, blob)) {
        auto put = simcore::db::ObjectStore::PutText(blob);
        if (!put.ok) return simcore::db::DbResult<std::string>::Err(put.error);

        auto jj = simcore::db::JobsRepo::Get(job_id);
        if (!jj.ok) return simcore::db::DbResult<std::string>::Err(jj.error);
        auto bc = simcore::db::BattleContextRepo::Insert(jj.value.job_set_id, job_id, put.value.id);
        if (!bc.ok) return simcore::db::DbResult<std::string>::Err(bc.error);

        kv.add("artifact_id", std::to_string(put.value.id));
        kv.add("context_id", std::to_string(bc.value));
        kv.add("size", std::to_string(put.value.size));
    }

    return simcore::db::DbResult<std::string>::Ok(kv.to_string_sorted());
}

DbResult<void> BattleContextDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {
    IniKV args = IniDoc::parse(action_args_ini).section_kv(IniDoc::GLOBAL);
    int64_t target_js = args.get_i64("target_job_set_id", 0);
    if (!target_js) {
        std::optional<std::string> purpose = args.get("purpose");
        std::optional<std::string> drk = args.get("domain_ref_kind");
        std::optional<int64_t> drid; if (args.has("domain_ref_id")) drid = args.get_i64("domain_ref_id", 0);
        std::optional<std::string> meta = args.get("meta_text");
        std::optional<int64_t> expected; if (args.has("expected_total")) expected = args.get_i64("expected_total", 0);
        auto crt = simcore::db::JobSetsRepo::Create(purpose, PK_BattleContextProbe, std::nullopt, drk, drid, meta, expected);
        if (!crt.ok) return DbResult<void>::Err(crt.error);
        target_js = crt.value;
    }
    IniKV bp = args;
    bp.erase("target_job_set_id"); bp.erase("purpose"); bp.erase("domain_ref_kind"); bp.erase("domain_ref_id");
    bp.erase("meta_text"); bp.erase("expected_total");
    auto enq = encode_job_into_db(target_js, bp.to_string_sorted());
    if (!enq.ok) return DbResult<void>::Err(enq.error);
    return DbResult<void>::Ok();
}