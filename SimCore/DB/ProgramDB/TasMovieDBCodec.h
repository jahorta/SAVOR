// SimCore/DB/ProgramDB/TasMovieDBCodec.h
#pragma once
#include "IProgramDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"
#include "../../Runner/Script/PhaseScriptVM.h"  // for RunBpOutcomeCode
#include <string>

using simcore::TriggerCtx;

namespace simcore::db::codec::tas {
    struct BlueprintIni {
        static constexpr const char* SECTION_NAME = "TasMovie.Blueprint";

        int64_t     base_dtm_artifact_id;
        int64_t     rtc_low;
        int64_t     rtc_high;
        int         priority{ 0 };
        uint32_t    run_ms{};
        uint32_t    vi_stall_ms{};
        bool        progress_enable{ true };
        bool        auto_queue_seeds{ false };

        static inline BlueprintIni from_section(const IniDoc& doc) {
            BlueprintIni bp{};
            if (!doc.has_section(SECTION_NAME)) return bp;
            IniKV section = doc.section_kv(SECTION_NAME);
            bp.base_dtm_artifact_id = section.get_i64("base_dtm_artifact_id", -1);
            bp.rtc_low = section.get_i64("rtc_low", 0);
            bp.rtc_high = section.get_i64("rtc_high", 0);
            bp.priority = section.get_i64("priority", 0);
            bp.run_ms = section.get_u32("run_ms", 0);
            bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
            bp.progress_enable = section.get_bool("progress_enable", true);
            bp.auto_queue_seeds = section.get_bool("auto_queue_seeds", true);
            return bp;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "base_dtm_artifact_id", std::to_string(base_dtm_artifact_id));
            doc.set(SECTION_NAME, "rtc_low", std::to_string(rtc_low));
            doc.set(SECTION_NAME, "rtc_high", std::to_string(rtc_high));
            doc.set(SECTION_NAME, "priority", std::to_string(priority));
            doc.set(SECTION_NAME, "run_ms", std::to_string(run_ms));
            doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
            doc.set(SECTION_NAME, "progress_enable", progress_enable ? "1" : "0");
            doc.set(SECTION_NAME, "auto_queue_seeds", auto_queue_seeds ? "1" : "0");
        }
        inline std::string to_string() const {
            IniDoc doc;
            set_section(doc);
            return doc.to_string_sorted();
        }
    };

    struct JobIni {
        static constexpr const char* SECTION_NAME = "TasMovie.Job";

        int64_t     new_rtc;

        static inline JobIni from_section(const IniDoc& doc) {
            JobIni job{};
            if (!doc.has_section(SECTION_NAME)) return job;
            IniKV section = doc.section_kv(SECTION_NAME);
            job.new_rtc = section.get_i64("new_rtc", -1);
            return job;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "new_rtc", std::to_string(new_rtc));
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
    };

    struct ResultsIni {
        static constexpr const char* SECTION_NAME = "TasMovie.Results";

        uint32_t     w_err{ 0 };
        uint32_t     dw_err{ 0 };

        uint32_t     vi_start{ 0 };
        uint32_t     vi_end{ 0 };

        std::string  savestate_path;

        static inline ResultsIni from_section(const IniDoc& doc) {
            ResultsIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.w_err = section.get_u32("w_err", (uint32_t)simcore::WERR_UnknownError);
            results.dw_err = section.get_u32("dw_err", (uint32_t)simcore::RunToBpOutcome::Unknown);
            results.savestate_path = section.get("savestate_path");
            results.vi_start = section.get_u32("vi_start", -1);
            results.vi_end = section.get_u32("vi_end", -1);
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
            doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
            doc.set(SECTION_NAME, "savestate_path", savestate_path);
            doc.set(SECTION_NAME, "vi_start", std::to_string(vi_start));
            doc.set(SECTION_NAME, "vi_end", std::to_string(vi_end));
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
    };

    struct CleanupIni {
        static constexpr const char* SECTION_NAME = "TasMovie.Cleanup";

        std::string     temp_dtm_path;

        static inline CleanupIni from_section(const IniDoc& doc) {
            CleanupIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.temp_dtm_path = section.get("temp_dtm_path");
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "temp_dtm_path", temp_dtm_path);
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
        inline std::string to_string() {
            IniDoc doc;
            set_section(doc);
            return doc.to_string_sorted();
        }
    };
}

struct TasMovieDBCodec final : IProgramDBCodec {
    DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    DbResult<simcore::PSJob> decode_job_from_db(int64_t job_id) override;
    DbResult<void> encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    DbResult<void> encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    simcore::db::DbResult<simcore::PSInit>        build_psinit_for_job(int64_t job_id) override;
    simcore::db::DbResult<std::string>            build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
    DbResult<void> phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) override;

    DbResult<std::string>            build_artifact_ini_from_db(int64_t job_id) override;
};
