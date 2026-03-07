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
#include "../Querying/DataService.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Phases/Programs/BattleContext/BattleContextPayload.h"
#include "../../Phases/Programs/BattleContext/BattleContextScript.h"
#include "../../Runner/Script/PhaseScriptVM.h"
#include "../../Runner/Script/PSContext.h"
#include "../../Phases/Programs/ProgramRegistry.h"
#include "../../Runner/Script/KeyRegistry.h"
#include "../../Runner/Parallel/DB/DBTriggerEngine.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"

using simcore::db::DbResult;
using simcore::TriggerCtx;
using simcore::db::battle::ctx::BlueprintIni;
using simcore::db::battle::ctx::ResultsIni;

static constexpr int kPK = PK_BattleContextProbe;
static constexpr int kPV = 1;

DbResult<int64_t> BattleContextDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    IniDoc ini = IniDoc::parse(blueprint_ini);

    BlueprintIni bp = BlueprintIni::from_section(ini);

    const uint32_t run_ms = bp.run_ms;
    const uint32_t vi_stall_ms = bp.vi_stall_ms;
    const int priority = bp.priority;
    const uint64_t savestate_id = bp.savestate_id;

    auto save_found = simcore::db::SavestateRepo::Get(savestate_id);
    if (!save_found.ok) return DbResult<int64_t>::Err(save_found.error);
    if (!save_found.value.has_value()) return DbResult<int64_t>::Err({ DbErrorKind::NotFound, 0, "savestate_id is required to make a battle context" });
    auto savestate_row = save_found.value.value();
    //if (!savestate_row.savestate_type != db::SavestateType::BATTLE) return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "a battle savestate is required" });


    std::string fp = "PK=" + std::to_string(kPK) + ";PV=" + std::to_string(kPV) 
        + ";ssid=" + std::to_string(bp.savestate_id) + ";bcver=" + std::to_string(soa::battle::ctx::codec::ver)
        + ";run_ms=" + std::to_string(run_ms)
        + ";vi=" + std::to_string(vi_stall_ms);

    auto ins = simcore::db::JobsRepo::CreateOrGetByFingerprint(job_set_id, kPK, kPV, /*program_ref_id*/0, fp, priority, blueprint_ini, bp.savestate_id);
    if (!ins.ok) return DbResult<int64_t>::Err(ins.error);

    auto ev = simcore::db::JobEventsRepo::Append(ins.value, "ENQUEUED", blueprint_ini);
    if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

    return DbResult<int64_t>::Ok(ins.value);
}

DbResult<simcore::PSJob> BattleContextDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);

    IniDoc ini;
    if (jr.value.vm_kv) ini = IniDoc::parse(*jr.value.vm_kv);
    else {
        auto enq = simcore::db::JobEventsRepo::GetFirstPayload(job_id, "ENQUEUED");
        if (!enq.ok) return DbResult<simcore::PSJob>::Err(enq.error);
        if (!enq.value) return DbResult<simcore::PSJob>::Err({ simcore::db::DbErrorKind::NotFound, 0, "missing ENQUEUED payload" });
        ini = IniDoc::parse(*enq.value);
    }

    BlueprintIni bp = BlueprintIni::from_section(ini);

    const uint32_t run_ms = bp.run_ms;
    const uint32_t vi_stall_ms = bp.vi_stall_ms;

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
        
    IniDoc ini = IniDoc::parse(results_ini);

    ResultsIni res = ResultsIni::from_section(ini);
    BlueprintIni bp = BlueprintIni::from_section(ini);
        
    auto st = simcore::db::JobsRepo::SetState(job_id, success ? "SUCCEEDED" : "FAILED");
    if (!st.ok) return DbResult<void>::Err(st.error);

    auto ev = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", res.to_string());
    if (!ev.ok) return DbResult<void>::Err(ev.error);

    if (success) {
        auto jb = simcore::db::JobsRepo::Get(job_id);
        if (!jb.ok) return DbResult<void>::Err(jb.error);

        int64_t ver = res.version;
        int64_t obj_id = res.artifact_id;
        int64_t sav_id = bp.savestate_id;

        auto bc = simcore::db::BattleContextRepo::Insert(jb.value.job_set_id, job_id, obj_id, sav_id, ver);
    }

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
    if (jr.value.vm_kv) kv = IniDoc::parse(*jr.value.vm_kv).section_kv(BlueprintIni::SECTION_NAME);

    return simcore::db::DbResult<std::optional<int64_t>>::Ok(std::optional<int64_t>(kv.get_i64("savestate_id", 0)));
}

simcore::db::DbResult<simcore::PSInit> BattleContextDBCodec::build_psinit_for_job(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSInit>::Err(jr.error);

    IniKV kv;
    if (jr.value.vm_kv) kv = IniDoc::parse(*jr.value.vm_kv).section_kv(BlueprintIni::SECTION_NAME);

    auto ss = SavestateRepo::Get(kv.get_i64("savestate_id"));
    if (!ss.ok) return DbResult<simcore::PSInit>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<simcore::PSInit>::Err({ simcore::db::DbErrorKind::NotFound, 0,
        "save state not found" });

    auto temp_savestate_path = ObjectStore::MaterializeToTemp(ss.value.value().object_ref_id);
    if (!temp_savestate_path.ok) return simcore::db::DbResult<simcore::PSInit>::Err(temp_savestate_path.error);

    simcore::PSInit init{};
    init.savestate_path = temp_savestate_path.value;
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return simcore::db::DbResult<simcore::PSInit>::Ok(init);
}

simcore::db::DbResult<std::string> BattleContextDBCodec::build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) {
    ResultsIni res{};
        
    res.w_err = r.ps.w_err;
    r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, res.dw_err);

    auto jb = JobsRepo::Get(job_id);
    if (!jb.ok) return DbResult<std::string>::Err(jb.error);
    if (!jb.value.vm_kv.has_value()) return DbResult<std::string>::Err({ simcore::db::DbErrorKind::NotFound, 0,
        "vm_kv not found" });

    IniDoc ini = IniDoc::parse(jb.value.vm_kv.value());
    BlueprintIni bp = BlueprintIni::from_section(ini);

    std::string blob;
    if (r.ps.ok && r.ps.ctx.get<std::string>(simcore::keys::battle::CTX_BLOB, blob)) {
        auto ss = SavestateRepo::Get(bp.savestate_id);
        if (!ss.ok) return DbResult<std::string>::Err(ss.error);
        if (!ss.value.has_value()) return DbResult<std::string>::Err({ simcore::db::DbErrorKind::NotFound, 0,
            "save state not found" });

        auto sso = ObjectStore::Get(ss.value.value().object_ref_id);
        if (!sso.ok) return simcore::db::DbResult<std::string>::Err(sso.error);

        std::filesystem::path ss_fn = sso.value.filename;
        std::string filename = ss_fn.stem().string() +
            "_v" + std::to_string(soa::battle::ctx::codec::ver) +
            soa::battle::ctx::codec::ext;

        auto put = simcore::db::ObjectStore::PutText(blob, filename);
        if (!put.ok) return simcore::db::DbResult<std::string>::Err(put.error);

        res.artifact_id = put.value.id;
        res.version = soa::battle::ctx::codec::ver;
        res.size = put.value.size;
        r.ps.ctx.get(simcore::keys::core::VI_FIRST, res.vi_start);
        r.ps.ctx.get(simcore::keys::core::VI_LAST, res.vi_end);
    }

    res.set_section(ini);

    return simcore::db::DbResult<std::string>::Ok(ini.to_string_sorted());
}

DbResult<void> BattleContextDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {
    return DbResult<void>::Err({DbErrorKind::InvalidState, 0, "Nothing should trigger a battle context"});
}

DbResult<std::string> BattleContextDBCodec::build_artifact_ini_from_db(int64_t job_id)
{
    ArtifactIniBuilder artifacts{};

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::string>::Err(jr.error);

    if (!jr.value.vm_kv) return DbResult<std::string>::Err(jr.error);

    IniKV kv = IniDoc::parse(*jr.value.vm_kv).section_kv(IniDoc::GLOBAL);
    const uint64_t savestate_id = kv.get_i64("savestate_id", 0);

    auto ss = SavestateRepo::Get(savestate_id);
    if (!ss.ok) return DbResult<std::string>::Err(ss.error);
    if (!ss.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
        "No Savestate found..." });

    artifacts.add_artifact("Savestate", ss.value.value().object_ref_id);

    return DbResult<std::string>::Ok(artifacts.to_string());
}