// SimCore/DB/ProgramDB/TasMovieDBCodec.cpp
#include "TasMovieDBCodec.h"

#include <filesystem>

#include "../../Utils/Hash.h"
#include "../DBCore/ObjectStore.h"
#include "../DBCore/Common.h"
#include "../../Runner/IPC/Wire.h"
#include "../TasMovieRepo.h"
#include "../SavestateRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/TriggersRepo.h"
#include "../../Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include "../../Runner/Parallel/DB/DBTriggerEngine.h"
#include "../../Tas/DtmFile.h"
#include "../../Runner/Script/KeyRegistry.h"
#include "../Querying/DataService.h"

namespace fs = std::filesystem;
static constexpr int PK = simcore::PK_TasMovie;
static constexpr int PV = 1;

using simcore::TriggerCtx;
using simcore::db::codec::tas::BlueprintIni;
using simcore::db::codec::tas::JobIni;
using simcore::db::codec::tas::ResultsIni;
using simcore::db::codec::tas::CleanupIni;

static std::string kv_get(const std::vector<std::pair<std::string, std::string>>& kv, const char* k, const std::string& dflt = {}) {
    for (auto& p : kv) if (p.first == k) return p.second;
    return dflt;
}

DbResult<int64_t> TasMovieDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    IniDoc ini = IniDoc::parse(blueprint_ini);
    
    BlueprintIni bp = BlueprintIni::from_section(ini);

    if (bp.rtc_low > bp.rtc_high) return DbResult<int64_t>::Err({DbErrorKind::InvalidArgument, 0, "rtc_low is higher than rtc_high"});

    auto base_dtm = ObjectStore::MaterializeToTemp(bp.base_dtm_artifact_id);
    if (!base_dtm.ok) return DbResult<int64_t>::Err(base_dtm.error);

    int enqueued = 0;
    for (int rtc = bp.rtc_low; rtc < bp.rtc_high; rtc++) {
        auto tm_idr = simcore::db::TasMovieRepo::IdempotentEnqueue(bp.base_dtm_artifact_id, rtc, bp.priority);
        if (!tm_idr.ok) return DbResult<int64_t>::Err(tm_idr.error);
        const int64_t program_ref_id = tm_idr.value;

        JobIni job{};
        job.new_rtc = rtc;
        const std::string vm_kv_text = job.append_section(ini).to_string_sorted();

        const std::string fp_input = "PK=" + std::to_string(PK) + ";PV=" + std::to_string(PV) + ";REF=" + std::to_string(program_ref_id) + ";VM=" + vm_kv_text;
        const std::string fingerprint = hash::sha256(fp_input.data(), fp_input.size());

        auto jid = simcore::db::JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, program_ref_id, fingerprint, /*priority=*/0, vm_kv_text);
        if (!jid.ok) return DbResult<int64_t>::Err(jid.error);

        auto ev = simcore::db::JobEventsRepo::Append(jid.value, "ENQUEUED", vm_kv_text);
        if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

        ++enqueued;
    }

    if (bp.auto_queue_seeds) {
        IniKV cond;
        cond.add("type", "EACH_JOB_TERMINAL");
        cond.add("success_only", std::to_string(1));
        auto tr = simcore::db::TriggersRepo::AddForJobSet(job_set_id, simcore::PK_SeedProbe,
            cond.to_string_sorted(), blueprint_ini);
    }

    return DbResult<int64_t>::Ok(enqueued);
}

DbResult<simcore::PSJob> TasMovieDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    const auto job = jr.value;

    auto tm = simcore::db::TasMovieRepo::Get(job.program_ref_id);
    if (!tm.ok) return DbResult<simcore::PSJob>::Err(tm.error);

    auto dtm_deets = ObjectStore::Get(tm.value.base_file_id);
    if (!dtm_deets.ok) return DbResult<simcore::PSJob>::Err(dtm_deets.error);

    auto dtm_pathr = simcore::db::ObjectStore::MaterializeToTemp(dtm_deets.value.id);
    if (!dtm_pathr.ok) return DbResult<simcore::PSJob>::Err(dtm_pathr.error);

    if (!job.vm_kv.has_value() || job.vm_kv->empty()) 
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "no ini was passed with job"});
    
    IniDoc ini = IniDoc::parse(job.vm_kv.value());

    if (!ini.has_section(BlueprintIni::SECTION_NAME))
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "no BlueprintIni was specified" });
    if (!ini.has_section(JobIni::SECTION_NAME))
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "no JobIni was specified" });

    JobIni job_ini = JobIni::from_section(ini);
    BlueprintIni bp = BlueprintIni::from_section(ini);

    simcore::tas::DtmFile base_dtm;
    base_dtm.load(dtm_pathr.value);
    base_dtm.set_recording_start_time(job_ini.new_rtc);

    std::string base_dtm_filename = dtm_deets.value.filename.empty() ? "temp.dtm" : dtm_deets.value.filename;
    std::string temp_filename = std::filesystem::path(base_dtm_filename).stem().string()+ "_rtc" + std::to_string(job_ini.new_rtc) + ".dtm";

    std::filesystem::path temp_filepath = std::filesystem::path(ObjectStore::TmpDir()) / "zzTasMovieDerived" / temp_filename;
    if (!std::filesystem::exists(temp_filepath.parent_path()))
        std::filesystem::create_directories(temp_filepath);

    std::string temp_filepath_str = temp_filepath.string();

    bool saved = base_dtm.save(temp_filepath_str);

    IniKV cfg;
    cfg.add("type", "EACH_JOB_TERMINAL");
    CleanupIni cleanup{};
    cleanup.temp_dtm_path = temp_filepath_str;

    TriggersRepo::AddForJob(job_id, simcore::PK_TasMovie, cfg.to_string_sorted(), cleanup.to_string());

    simcore::tasmovie::EncodeSpec spec{};
    spec.dtm_path = temp_filepath.string();
    spec.progress_enable = bp.progress_enable;
    spec.run_ms = bp.run_ms;
    spec.vi_stall_ms = bp.vi_stall_ms;

    std::vector<uint8_t> payload;
    if (!simcore::tasmovie::encode_payload(spec, payload))
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::InvalidArgument, 0, "encode_payload failed" });

    auto tas_running = TasMovieRepo::MarkRunning(tm.value.id);
    if (!tas_running.ok) return DbResult<simcore::PSJob>::Err(tas_running.error);

    simcore::PSJob out{};
    out.payload = std::move(payload);
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> TasMovieDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line) {
    auto r = simcore::db::JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!r.ok) return DbResult<void>::Err(r.error);
    return DbResult<void>::Ok();
}

DbResult<void> TasMovieDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) {
    auto r = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!r.ok) return DbResult<void>::Err(r.error);

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<void>::Err(jr.error);
    const auto job = jr.value;

    IniDoc ini = IniDoc::parse(results_ini);

    ResultsIni results = ResultsIni::from_section(ini);

    if (results.savestate_path.empty() || !std::filesystem::exists(results.savestate_path)) 
        success = false;

    if (results.dw_err != 0 || results.w_err != 0)
        success = false;

    if (success) {
        std::string filename = std::filesystem::path(results.savestate_path).filename().string();

        auto object_ref_id = ObjectStore::FinalizeFromFile(results.savestate_path, Compression::None, filename);

        std::filesystem::remove(results.savestate_path);
        if (!object_ref_id.ok) return DbResult<void>::Err(object_ref_id.error);

        
        auto plan = simcore::db::SavestateRepo::Plan(SavestateType::BATTLE, "TasMovie");
        if (!plan.ok) return DbResult<void>::Err(plan.error);

        auto fin = simcore::db::SavestateRepo::Finalize(plan.value, object_ref_id.value.id);
        if (!fin.ok) return DbResult<void>::Err(fin.error);

        auto mark = simcore::db::TasMovieRepo::MarkDone(job.program_ref_id, plan.value);
        if (!mark.ok) return DbResult<void>::Err(mark.error);

        auto st = simcore::db::JobsRepo::SetState(job_id, "SUCCEEDED");
        if (!st.ok) return DbResult<void>::Err(st.error);
    }
    else {
        if (!results.savestate_path.empty() && std::filesystem::exists(results.savestate_path))
            std::filesystem::remove(results.savestate_path);

        auto mark = simcore::db::TasMovieRepo::MarkFailed(job.program_ref_id, "failed");
        if (!mark.ok) return DbResult<void>::Err(mark.error);

        auto st = simcore::db::JobsRepo::SetState(job_id, "FAILED");
        if (!st.ok) return DbResult<void>::Err(st.error);

        if (!results.savestate_path.empty()) return DbResult<void>::Err({DbErrorKind::InvalidArgument, 0, 
            "savestate_path present in failed run: " + results.savestate_path});
    }

    return DbResult<void>::Ok();
}

DbResult<std::string> TasMovieDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    std::string out;
    if (job_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (auto& e : rows.value) {
            if (e.payload) { out.append(*e.payload); out.push_back('\n'); }
        }
    }
    else if (job_set_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (auto& e : rows.value) {
            if (e.payload) { out.append(*e.payload); out.push_back('\n'); }
        }
    }
    else {
        return DbResult<std::string>::Err({ DbErrorKind::InvalidArgument, 0, "must supply job_id or job_set_id" });
    }
    return DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::string> TasMovieDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) {
    // Once a results column is added, take results from TasMovieDB (only?, in addition?)
    
    std::string out;
    if (job_id) {
        auto p = simcore::db::JobEventsRepo::GetLatestPayload(*job_id, "RESULTS");
        if (!p.ok) return DbResult<std::string>::Err(p.error);
        if (p.value.has_value()) out = p.value.value();
    }
    else if (job_set_id) {
        auto v = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!v.ok) return DbResult<std::string>::Err(v.error);
        for (auto& e : v.value) { if (e.payload) { out.append(*e.payload); out.push_back('\n'); } }
    }
    else {
        return DbResult<std::string>::Err({ DbErrorKind::InvalidArgument, 0, "must supply job_id or job_set_id" });
    }
    return DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::optional<int64_t>> TasMovieDBCodec::get_required_savestate_id(int64_t /*job_id*/) {
    return DbResult<std::optional<int64_t>>::Ok(std::nullopt);
}

DbResult<simcore::PSInit> TasMovieDBCodec::build_psinit_for_job(int64_t /*job_id*/) {
    simcore::PSInit init{};
    init.savestate_path.clear();
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> TasMovieDBCodec::build_results_ini_from_prresult(int64_t /*job_id*/, const simcore::PRResult& r) {
    
    ResultsIni results{};
    bool success = r.ps.ok ? true : false;
    results.w_err = r.ps.w_err;

    if (results.w_err == 0) r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, results.dw_err);

    if (success) {
        r.ps.ctx.get(simcore::keys::tas::SAVE_PATH, results.savestate_path);
        r.ps.ctx.get(simcore::keys::core::VI_FIRST, results.vi_start);
        r.ps.ctx.get(simcore::keys::core::VI_LAST, results.vi_end);
    }

    IniDoc ini;
    return DbResult<std::string>::Ok(results.append_section(ini).to_string_sorted());
}

DbResult<void> TasMovieDBCodec::phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) {
    if (ctx.prev_program_kind != simcore::PK_TasMovie)
        return DbResult<void>::Err({DbErrorKind::InvalidState, 0, 
            "there should be nothing that triggers a TasMovie other than itself"});

    IniDoc ini = IniDoc::parse(action_args_ini);

    if (ini.has_section(CleanupIni::SECTION_NAME)) {
        CleanupIni cleanup = CleanupIni::from_section(ini);

        std::string temp_dtm_path = cleanup.temp_dtm_path;

        if (!std::filesystem::exists(temp_dtm_path))
            return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "no file to cleanup, was the correct filename sent? " + temp_dtm_path});

        std::filesystem::remove(temp_dtm_path);

        return DbResult<void>::Ok();
    }
    
    return DbResult<void>::Err({ DbErrorKind::InvalidState, 0,
            "cleanup is the only implemented TasMovie phase trigger, however this was sent:\n" + action_args_ini });
}

DbResult<std::string> TasMovieDBCodec::build_artifact_ini_from_db(int64_t job_id)
{
    ArtifactIniBuilder artifacts{};

    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<std::string>::Err(jr.error);

    if (!jr.value.vm_kv) return DbResult<std::string>::Err(jr.error);

    IniDoc ini = IniDoc::parse(*jr.value.vm_kv);
    IniKV bp = ini.section_kv(BlueprintIni::SECTION_NAME);

    const auto base_dtm_artifact_id = bp.get_i64("base_dtm_artifact_id", -1);
    if (base_dtm_artifact_id < 0) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
            "Base DTM file not found..." });

    artifacts.add_artifact("Base DTM File", base_dtm_artifact_id);

    auto tas = simcore::db::TasMovieRepo::Get(jr.value.program_ref_id);
    if (!tas.ok) return DbResult<std::string>::Err(tas.error);

    if (tas.value.status == "done" && tas.value.output_savestate_id.has_value()) {

        auto ss = SavestateRepo::Get(tas.value.output_savestate_id.value());
        if (!ss.ok) return DbResult<std::string>::Err(ss.error);
        if (!ss.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0,
            "TasMovie was marked done but no Savestate found..." });

        artifacts.add_artifact("savestate", ss.value.value().object_ref_id);
    }

    return DbResult<std::string>::Ok(artifacts.to_string());
}
