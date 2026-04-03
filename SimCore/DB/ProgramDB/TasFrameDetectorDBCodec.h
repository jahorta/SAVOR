#pragma once
#include "IProgramDBCodec.h"
#include "../../Utils/IniDoc.h"

namespace simcore::db::codec::tasframedetector {

struct BlueprintIni {
    static constexpr const char* SECTION_NAME = "TasFrameDetector.Blueprint";
    int64_t base_dtm_artifact_id{ -1 };
    int priority{ 0 };
    uint32_t vi_stall_ms{ 0 };

    static inline BlueprintIni from_section(const IniDoc& doc) {
        BlueprintIni bp{};
        if (!doc.has_section(SECTION_NAME)) return bp;
        IniKV section = doc.section_kv(SECTION_NAME);
        bp.base_dtm_artifact_id = section.get_i64("base_dtm_artifact_id", -1);
        bp.priority = section.get_i64("priority", 0);
        bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
        return bp;
    }
    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "base_dtm_artifact_id", std::to_string(base_dtm_artifact_id));
        doc.set(SECTION_NAME, "priority", std::to_string(priority));
        doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
    }
};

struct ResultsIni {
    static constexpr const char* SECTION_NAME = "TasFrameDetector.Results";
    uint32_t w_err{ 0 };
    uint32_t dw_err{ 0 };
    uint32_t sample_count{ 0 };
    int64_t artifact_id{ -1 };

    inline void set_section(IniDoc& doc) const {
        doc.ensure_section(SECTION_NAME);
        doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
        doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
        doc.set(SECTION_NAME, "sample_count", std::to_string(sample_count));
        doc.set(SECTION_NAME, "artifact_id", std::to_string(artifact_id));
    }

    static inline ResultsIni from_section(const IniDoc& doc) {
        ResultsIni r{};
        if (!doc.has_section(SECTION_NAME)) return r;
        IniKV section = doc.section_kv(SECTION_NAME);
        r.w_err = section.get_u32("w_err", 0);
        r.dw_err = section.get_u32("dw_err", 0);
        r.sample_count = section.get_u32("sample_count", 0);
        r.artifact_id = section.get_i64("artifact_id", -1);
        return r;
    }
};

} // namespace simcore::db::codec::tasframedetector

struct TasFrameDetectorDBCodec final : IProgramDBCodec {
    DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    DbResult<simcore::PSJob> decode_job_from_db(int64_t job_id) override;
    DbResult<void> encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    DbResult<void> encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    DbResult<simcore::PSInit> build_psinit_for_job(int64_t job_id) override;
    DbResult<std::string> build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
    DbResult<void> phase_setup_on_trigger(const simcore::TriggerCtx& ctx, const std::string& action_args_ini) override;
    DbResult<std::string> build_artifact_ini_from_db(int64_t job_id) override;
};
