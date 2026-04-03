#include "TasFrameDetectorDBCodec.h"
#include "ResultErrorFormatting.h"

#include "../DBCore/ObjectStore.h"
#include "../Scheduling/JobsRepo.h"
#include "../Scheduling/JobEventsRepo.h"
#include "../Scheduling/JobSetsRepo.h"
#include "../TasFrameDetectRepo.h"
#include "../Querying/DataService.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Phases/Programs/TasFrameDetector/TasFrameDetectorPayload.h"
#include "../../Runner/Script/KeyRegistry.h"
#include "../../Utils/Hash.h"

using simcore::db::codec::tasframedetector::BlueprintIni;
using simcore::db::codec::tasframedetector::ResultsIni;

static constexpr int kPK = simcore::PK_TasInputStreamDetector;
static constexpr int kPV = 1;

DbResult<int64_t> TasFrameDetectorDBCodec::encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini)
{
    const IniDoc ini = IniDoc::parse(blueprint_ini);
    const BlueprintIni bp = BlueprintIni::from_section(ini);
    if (bp.base_dtm_artifact_id <= 0) {
        return DbResult<int64_t>::Err({ DbErrorKind::InvalidArgument, 0, "base_dtm_artifact_id is required" });
    }

    auto dtm = simcore::db::ObjectStore::Get(bp.base_dtm_artifact_id);
    if (!dtm.ok) return DbResult<int64_t>::Err(dtm.error);

    const std::string fp_input =
        "PK=" + std::to_string(kPK) +
        ";PV=" + std::to_string(kPV) +
        ";BASE_DTM=" + std::to_string(bp.base_dtm_artifact_id) +
        ";VM=" + ini.to_string_sorted();
    const std::string fingerprint = hash::sha256(fp_input.data(), fp_input.size());

    auto jid = simcore::db::JobsRepo::CreateOrGetByFingerprint(job_set_id, kPK, kPV, 0, fingerprint, bp.priority, ini.to_string_sorted());
    if (!jid.ok) return DbResult<int64_t>::Err(jid.error);

    auto ev = simcore::db::JobEventsRepo::Append(jid.value, "ENQUEUED", ini.to_string_sorted());
    if (!ev.ok) return DbResult<int64_t>::Err(ev.error);

    return DbResult<int64_t>::Ok(1);
}

DbResult<simcore::PSJob> TasFrameDetectorDBCodec::decode_job_from_db(int64_t job_id)
{
    auto jr = simcore::db::JobsRepo::Get(job_id);
    if (!jr.ok) return DbResult<simcore::PSJob>::Err(jr.error);
    if (!jr.value.vm_kv.has_value() || jr.value.vm_kv->empty()) {
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::NotFound, 0, "no ini was passed with job" });
    }

    IniDoc ini = IniDoc::parse(*jr.value.vm_kv);
    BlueprintIni bp = BlueprintIni::from_section(ini);

    auto dtm = simcore::db::ObjectStore::Get(bp.base_dtm_artifact_id);
    if (!dtm.ok) return DbResult<simcore::PSJob>::Err(dtm.error);

    auto dtm_path = simcore::db::ObjectStore::MaterializeToTemp(dtm.value.id);
    if (!dtm_path.ok) return DbResult<simcore::PSJob>::Err(dtm_path.error);

    simcore::tasframedetector::EncodeSpec spec{};
    spec.dtm_path = dtm_path.value;
    spec.vi_stall_ms = bp.vi_stall_ms;

    std::vector<uint8_t> payload;
    if (!simcore::tasframedetector::encode_payload(spec, payload)) {
        return DbResult<simcore::PSJob>::Err({ DbErrorKind::InvalidArgument, 0, "encode_payload failed" });
    }

    simcore::PSJob out{};
    out.payload = std::move(payload);
    return DbResult<simcore::PSJob>::Ok(std::move(out));
}

DbResult<void> TasFrameDetectorDBCodec::encode_progress_into_db(int64_t job_id, const std::string& progress_line)
{
    auto r = simcore::db::JobEventsRepo::Append(job_id, "PROGRESS", progress_line);
    if (!r.ok) return DbResult<void>::Err(r.error);
    return DbResult<void>::Ok();
}

DbResult<void> TasFrameDetectorDBCodec::encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success)
{
    auto ev = simcore::db::JobEventsRepo::Append(job_id, "RESULTS", results_ini);
    if (!ev.ok) return DbResult<void>::Err(ev.error);

    if (success) {
        auto jr = simcore::db::JobsRepo::Get(job_id);
        if (!jr.ok) return DbResult<void>::Err(jr.error);
        if (!jr.value.vm_kv.has_value() || jr.value.vm_kv->empty())
            return DbResult<void>::Err({ DbErrorKind::NotFound, 0, "vm_kv missing for TasFrameDetector job" });

        const IniDoc bp_doc = IniDoc::parse(*jr.value.vm_kv);
        const BlueprintIni bp = BlueprintIni::from_section(bp_doc);
        const IniDoc results_doc = IniDoc::parse(results_ini);
        const ResultsIni results = ResultsIni::from_section(results_doc);
        if (results.artifact_id <= 0 || bp.base_dtm_artifact_id <= 0) {
            return DbResult<void>::Err({ DbErrorKind::InvalidState, 0, "missing artifact ids for tas_frame_detect insert" });
        }

        auto ins = simcore::db::TasFrameDetectRepo::Insert(results.artifact_id, bp.base_dtm_artifact_id);
        if (!ins.ok) return DbResult<void>::Err(ins.error);
    }

    auto st = simcore::db::JobsRepo::SetState(job_id, success ? "SUCCEEDED" : "FAILED");
    if (!st.ok) return DbResult<void>::Err(st.error);

    return DbResult<void>::Ok();
}

DbResult<std::string> TasFrameDetectorDBCodec::decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    std::string out;
    if (job_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobAndKind(*job_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (auto& e : rows.value) {
            if (e.payload) { out.append(*e.payload); out.push_back('\n'); }
        }
    } else if (job_set_id) {
        auto rows = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "PROGRESS");
        if (!rows.ok) return DbResult<std::string>::Err(rows.error);
        for (auto& e : rows.value) {
            if (e.payload) { out.append(*e.payload); out.push_back('\n'); }
        }
    } else {
        return DbResult<std::string>::Err({ DbErrorKind::InvalidArgument, 0, "must supply job_id or job_set_id" });
    }
    return DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::string> TasFrameDetectorDBCodec::decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id)
{
    std::string out;
    if (job_id) {
        auto p = simcore::db::JobEventsRepo::GetLatestPayload(*job_id, "RESULTS");
        if (!p.ok) return DbResult<std::string>::Err(p.error);
        if (p.value.has_value()) out = simcore::db::codec::HumanizeResultIniErrors(p.value.value());
    } else if (job_set_id) {
        auto v = simcore::db::JobEventsRepo::ListByJobSetAndKind(*job_set_id, "RESULTS");
        if (!v.ok) return DbResult<std::string>::Err(v.error);
        for (auto& e : v.value) {
            if (e.payload) { out.append(simcore::db::codec::HumanizeResultIniErrors(*e.payload)); out.push_back('\n'); }
        }
    } else {
        return DbResult<std::string>::Err({ DbErrorKind::InvalidArgument, 0, "must supply job_id or job_set_id" });
    }
    return DbResult<std::string>::Ok(std::move(out));
}

DbResult<std::optional<int64_t>> TasFrameDetectorDBCodec::get_required_savestate_id(int64_t)
{
    return DbResult<std::optional<int64_t>>::Ok(std::nullopt);
}

DbResult<simcore::PSInit> TasFrameDetectorDBCodec::build_psinit_for_job(int64_t)
{
    simcore::PSInit init{};
    init.savestate_path.clear();
    init.default_timeout_ms = 10000;
    init.derived_buffer_type = simcore::DBuf::DK_None;
    return DbResult<simcore::PSInit>::Ok(init);
}

DbResult<std::string> TasFrameDetectorDBCodec::build_results_ini_from_prresult(int64_t, const simcore::PRResult& r)
{
    ResultsIni results{};
    results.w_err = r.ps.w_err;
    r.ps.ctx.get(simcore::keys::core::DW_RUN_OUTCOME_CODE, results.dw_err);
    r.ps.ctx.get(simcore::keys::tasframedetector::SAMPLE_COUNT, results.sample_count);

    std::string stream_ini;
    r.ps.ctx.get(simcore::keys::tasframedetector::STREAM_INI, stream_ini);
    std::string header_identity;
    r.ps.ctx.get(simcore::keys::tasframedetector::HEADER_IDENTITY, header_identity);
    std::string emu_version;
    r.ps.ctx.get(simcore::keys::tasframedetector::EMU_VERSION, emu_version);

    IniDoc artifact_doc = stream_ini.empty() ? IniDoc{} : IniDoc::parse(stream_ini);
    artifact_doc.ensure_section("Metadata");
    artifact_doc.set("Metadata", "header_identity", header_identity);
    artifact_doc.set("Metadata", "emulator_version", emu_version);

    auto put = simcore::db::ObjectStore::PutText(artifact_doc.to_string_sorted(), "tas_input_stream.ini");
    if (!put.ok) return DbResult<std::string>::Err(put.error);
    results.artifact_id = put.value.id;

    IniDoc out;
    results.set_section(out);
    return DbResult<std::string>::Ok(out.to_string_sorted());
}

DbResult<void> TasFrameDetectorDBCodec::phase_setup_on_trigger(const simcore::TriggerCtx&, const std::string&)
{
    return DbResult<void>::Err({ DbErrorKind::InvalidState, 0, "TasFrameDetector does not support trigger follow-up actions" });
}

DbResult<std::string> TasFrameDetectorDBCodec::build_artifact_ini_from_db(int64_t job_id)
{
    ArtifactIniBuilder artifacts{};

    auto latest = simcore::db::JobEventsRepo::GetLatestPayload(job_id, "RESULTS");
    if (!latest.ok) return DbResult<std::string>::Err(latest.error);
    if (!latest.value.has_value()) return DbResult<std::string>::Err({ DbErrorKind::NotFound, 0, "No RESULTS payload found" });

    IniDoc results_doc = IniDoc::parse(*latest.value);
    auto res = ResultsIni::from_section(results_doc);
    if (res.artifact_id > 0) artifacts.add_artifact("Tas Input Stream", res.artifact_id);
    return DbResult<std::string>::Ok(artifacts.to_string());
}
