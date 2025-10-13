#pragma once
#include "IProgramDBCodec.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"

using simcore::TriggerCtx;

namespace simcore::db::battle::ctx {

    struct BlueprintIni {
        static constexpr const char* SECTION_NAME = "BattleContext.Blueprint";

        int64_t     savestate_id;
        int         priority{ 0 };
        uint32_t    run_ms{};
        uint32_t    vi_stall_ms{};

        static inline BlueprintIni from_section(const IniDoc& doc) {
            BlueprintIni bp{};
            if (!doc.has_section(SECTION_NAME)) return bp;
            IniKV section = doc.section_kv(SECTION_NAME);
            bp.savestate_id = section.get_i64("savestate_id", 0);
            bp.priority = (int) section.get_i64("priority", 0);
            bp.run_ms = section.get_u32("run_ms", 0);
            bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
            return bp;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "savestate_id", std::to_string(savestate_id));
            doc.set(SECTION_NAME, "priority", std::to_string(priority));
            doc.set(SECTION_NAME, "run_ms", std::to_string(run_ms));
            doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
        }
        inline std::string to_string() const {
            IniDoc doc;
            set_section(doc);
            return doc.to_string_sorted();
        }
    };

    struct ResultsIni {
        static constexpr const char* SECTION_NAME = "BattleContext.Results";

        uint8_t     w_err{ 0 };
        uint32_t     dw_err{ 0 };

        uint32_t     vi_start{ 0 };
        uint32_t     vi_end{ 0 };

        int64_t      version{ 0 };
        int64_t      artifact_id{ 0 };
        uint64_t     size{ 0 };

        static inline ResultsIni from_section(const IniDoc& doc) {
            ResultsIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.w_err = section.get_u32("w_err", (uint32_t)simcore::WERR_UnknownError);
            results.dw_err = section.get_u32("dw_err", (uint32_t)simcore::RunToBpOutcome::Unknown);
            results.vi_start = section.get_u32("vi_start", -1);
            results.vi_end = section.get_u32("vi_end", -1);
            results.version = section.get_i64("version", 0);
            results.artifact_id = section.get_i64("artifact_id", 0);
            results.size = section.get_u64("size", 0);
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
            doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
            doc.set(SECTION_NAME, "vi_start", std::to_string(vi_start));
            doc.set(SECTION_NAME, "vi_end", std::to_string(vi_end));
            doc.set(SECTION_NAME, "version", std::to_string(version));
            doc.set(SECTION_NAME, "artifact_id", std::to_string(artifact_id));
            doc.set(SECTION_NAME, "size", std::to_string(size));
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
        std::string to_string() const {
            IniDoc doc{};
            set_section(doc);
            return doc.to_string_sorted();
        }
    };

}

struct BattleContextDBCodec final : IProgramDBCodec {
    DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    DbResult<simcore::PSJob>   decode_job_from_db(int64_t job_id) override;
    DbResult<void>    encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    DbResult<void>    encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    simcore::db::DbResult<simcore::PSInit>        build_psinit_for_job(int64_t job_id) override;
    simcore::db::DbResult<std::string>            build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
    DbResult<void> phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) override;

    DbResult<std::string>            build_artifact_ini_from_db(int64_t job_id) override;
};
