// SimCore/DB/ProgramDB/TasMovieDBCodec.cpp
#include "TasMovieDBCodec.h"
#include "IniKV.h"
#include "../../Utils/Hash.h"
#include "../DBCore/ObjectStore.h"
#include "../../Runner/IPC/Wire.h"
#include "../TasMovieRepo.h"
#include "../SavestateRepo.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../../Phases/Programs/PlayTasMovie/TasMoviePayload.h"
#include <filesystem>

namespace fs = std::filesystem;
static constexpr int PK = simcore::PK_TasMovie;
static constexpr int PV = 1;

static std::string kv_get(const std::vector<std::pair<std::string, std::string>>& kv, const char* k, const std::string& dflt = {}) {
    for (auto& p : kv) if (p.first == k) return p.second;
    return dflt;
}

DbResult<int64_t> TasMovieDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) {
    IniKV kv = IniKV::parse(blueprint_ini);

    const std::string dtm_path = kv.get("dtm_path");
    const std::string save_dir = kv.get("save_dir");
    const int64_t     new_rtc = kv.get_i64("new_rtc", 0);
    const int         priority = static_cast<int>(kv.get_i64("priority", 0));
    const bool        save_on_fail = kv.get_bool("save_on_fail", false);
    const bool        progress_enable = kv.get_bool("progress_enable", true);
    const uint32_t    run_ms = kv.get_u32("run_ms", 0);
    const uint32_t    vi_stall_ms = kv.get_u32("vi_stall_ms", 0);
    const std::string objdir = kv.get("object_dir", ".objects");
    const std::string tmpdir = kv.get("tmp_dir", ".tmp");

    if (dtm_path.empty()) {
        return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "missing dtm_path" });
    }

    auto os_row = ObjectStore::FinalizeFromFile(dtm_path, objdir, simcore::db::Compression::None, fs::path(dtm_path).filename().string());
    if (!os_row.ok) return DbResult<int64_t>::Err(os_row.error);

    auto tm_idr = simcore::db::TasMovieRepo::IdempotentEnqueue(os_row.value.id, new_rtc ? std::optional<int64_t>(new_rtc) : std::nullopt, priority);
    if (!tm_idr.ok) return DbResult<int64_t>::Err(tm_idr.error);
    const int64_t program_ref_id = tm_idr.value;

    IniKV vmkv;
    vmkv.add("dtm_artifact_id", std::to_string(os_row.value.id));
    vmkv.add("save_dir", save_dir);
    vmkv.add("save_on_fail", save_on_fail ? "1" : "0");
    vmkv.add("progress_enable", progress_enable ? "1" : "0");
    vmkv.add("run_ms", std::to_string(run_ms));
    vmkv.add("vi_stall_ms", std::to_string(vi_stall_ms));
    const std::string vm_kv_text = vmkv.to_string_sorted();

    const std::string fp_input = "PK=" + std::to_string(PK) + ";PV=" + std::to_string(PV) + ";REF=" + std::to_string(program_ref_id) + ";VM=" + vm_kv_text;
    const std::string fingerprint = hash::sha256(fp_input.data(), fp_input.size());

    auto jid = simcore::db::JobsRepo::CreateOrGetByFingerprint(job_set_id, PK, PV, program_ref_id, fingerprint, /*priority=*/0, vm_kv_text);
    if (!jid.ok) return DbResult<int64_t>::Err(jid.error);

    auto ev = simcore::db::JobEventsRepo::Append(jid.value, "ENQUEUED", vm_kv_text);
    if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

    return DbResult<int64_t>::Ok(jid.value);
}

DbResult<simcore::PSJob> TasMovieDBCodec::decode_job_from_db(int64_t job_id) {
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    const auto job = jr.value;

    auto tm = simcore::db::TasMovieRepo::Get(job.program_ref_id);
    if (!tm.ok) return DbResult<simcore::PSJob>::Err(tm.error);

    const std::string objdir = ".objects";
    const std::string tmpdir = ".tmp";

    auto dtm_pathr = simcore::db::ObjectStore::MaterializeToTemp(tm.value.base_file_id, objdir, tmpdir);
    if (!dtm_pathr.ok) return DbResult<simcore::PSJob>::Err(dtm_pathr.error);

    IniKV vmkv;
    if (job.vm_kv.has_value() && !job.vm_kv->empty()) vmkv = IniKV::parse(job.vm_kv.value());
    else {
        auto enq = simcore::db::JobEventsRepo::GetFirstPayload(job.job_id, "ENQUEUED");
        if (!enq.ok) return DbResult<simcore::PSJob>::Err(enq.error);
        if (enq.value.has_value()) vmkv = IniKV::parse(enq.value.value());
    }

    simcore::tasmovie::EncodeSpec spec{};
    spec.dtm_path = dtm_pathr.value;
    spec.save_dir = vmkv.get("save_dir");
    spec.save_on_fail = vmkv.get("save_on_fail") == "1";
    spec.progress_enable = vmkv.get("progress_enable") == "1";
    spec.run_ms = vmkv.get_u32("run_ms", 0);
    spec.vi_stall_ms = vmkv.get_u32("vi_stall_ms", 0);

    std::vector<uint8_t> payload;
    if (!simcore::tasmovie::encode_payload(spec, payload))
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::InvalidArgument, 0, "encode_payload failed" });

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

    if (success) {
        IniKV kv = IniKV::parse(results_ini);
        const int64_t object_ref_id = kv.get_i64("savestate_artifact_id", 0);
        if (object_ref_id <= 0) return DbResult<void>::Err({ DbErrorKind::InvalidArgument, 0, "savestate_artifact_id not found" });

        auto plan = simcore::db::SavestateRepo::Plan(/*type=*/0, "TasMovie");
        if (!plan.ok) return DbResult<void>::Err(plan.error);
        auto fin = simcore::db::SavestateRepo::Finalize(plan.value, object_ref_id);
        if (!fin.ok) return DbResult<void>::Err(fin.error);

        auto mark = simcore::db::TasMovieRepo::MarkDone(job.program_ref_id, plan.value);
        if (!mark.ok) return DbResult<void>::Err(mark.error);

        auto st = simcore::db::JobsRepo::SetState(job_id, "SUCCEEDED");
        if (!st.ok) return DbResult<void>::Err(st.error);
    }
    else {
        auto mark = simcore::db::TasMovieRepo::MarkFailed(job.program_ref_id, "failed");
        if (!mark.ok) return DbResult<void>::Err(mark.error);
        auto st = simcore::db::JobsRepo::SetState(job_id, "FAILED");
        if (!st.ok) return DbResult<void>::Err(st.error);
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
    IniKV kv;
    kv.add("ok", r.ps.ok ? "1" : "0");
    kv.add("w_err", std::to_string(static_cast<unsigned>(r.ps.w_err)));
    return DbResult<std::string>::Ok(kv.to_string_sorted());
}
