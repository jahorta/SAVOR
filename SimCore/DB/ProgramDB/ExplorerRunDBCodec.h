// SimCore/DB/ProgramDB/ExplorerRunDBCodec.h
#pragma once
#include "IProgramDBCodec.h"
#include "../../Runner/Script/PhaseScriptVM.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"
#include <sstream>
#include <vector>

using simcore::TriggerCtx;

namespace simcore::db::codec::battle::run {
    struct BlueprintIni {
        static constexpr const char* SECTION_NAME = "BattleRun.Blueprint";

        int64_t     settings_id{ -1 };
        int64_t     seed_probe_id{ -1 };
        std::string delta_seed_ids_csv{"0"};
        int         priority{ 0 };
        uint32_t    run_ms{};
        uint32_t    vi_stall_ms{};
        bool        progress_enable{ true };
        bool        use_single_turn_runner{ false };
        bool        auto_wave_trigger_enable{ false };
        uint32_t    min_fake_attacks{ 0 };
        uint32_t    max_fake_attacks{ 0 };

        static inline BlueprintIni from_section(const IniDoc& doc) {
            BlueprintIni bp{};
            if (!doc.has_section(SECTION_NAME)) return bp;
            IniKV section = doc.section_kv(SECTION_NAME);
            bp.settings_id = section.get_i64("settings_id", -1);
            bp.seed_probe_id = section.get_i64("seed_probe_id", -1);
            bp.delta_seed_ids_csv = section.get("delta_seed_ids_csv", "0");
            bp.priority = section.get_i64("priority", 0);
            bp.run_ms = section.get_u32("run_ms", 0);
            bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
            bp.progress_enable = section.get_bool("progress_enable", true);
            bp.use_single_turn_runner = section.get_bool("use_single_turn_runner", false);
            bp.auto_wave_trigger_enable = section.get_bool("auto_wave_trigger_enable", false);
            bp.min_fake_attacks = section.get_u32("min_fake_attacks", 0);
            bp.max_fake_attacks = section.get_u32("max_fake_attacks", 0);
            return bp;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "settings_id", std::to_string(settings_id));
            doc.set(SECTION_NAME, "seed_probe_id", std::to_string(seed_probe_id));
            doc.set(SECTION_NAME, "delta_seed_ids_csv", delta_seed_ids_csv.empty() ? "0" : delta_seed_ids_csv);
            doc.set(SECTION_NAME, "priority", std::to_string(priority));
            doc.set(SECTION_NAME, "run_ms", std::to_string(run_ms));
            doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
            doc.set(SECTION_NAME, "progress_enable", progress_enable ? "1" : "0");
            doc.set(SECTION_NAME, "use_single_turn_runner", use_single_turn_runner ? "1" : "0");
            doc.set(SECTION_NAME, "auto_wave_trigger_enable", auto_wave_trigger_enable ? "1" : "0");
            doc.set(SECTION_NAME, "min_fake_attacks", std::to_string(min_fake_attacks));
            doc.set(SECTION_NAME, "max_fake_attacks", std::to_string(max_fake_attacks));
        }
        inline std::string to_string() const {
            IniDoc doc{};
            set_section(doc);
            return doc.to_string_preserve_order();
        }
        static inline std::vector<int64_t> parse_ids_csv(const std::string& csv) {
            std::vector<int64_t> out;
            if (csv.empty()) return out;
            std::stringstream ss(csv);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                if (tok.empty()) continue;
                try {
                    out.push_back(std::stoll(tok));
                }
                catch (...) {
                }
            }
            return out;
        }
        static inline std::string to_ids_csv(const std::vector<int64_t>& ids) {
            std::string out;
            for (size_t i = 0; i < ids.size(); ++i) {
                if (i) out.push_back(',');
                out += std::to_string(ids[i]);
            }
            return out;
        }
    };

    struct JobIni {
        static constexpr const char* SECTION_NAME = "BattleRun.Job";

        int64_t     plan_id;
        int64_t     delta_seed_id;
        int64_t     savestate_id;
        std::string fake_attacks_by_turn_csv;

        static inline JobIni from_section(const IniDoc& doc) {
            JobIni job{};
            if (!doc.has_section(SECTION_NAME)) return job;
            IniKV section = doc.section_kv(SECTION_NAME);
            job.plan_id = section.get_i64("plan_id", -1);
            job.delta_seed_id = section.get_i64("delta_seed_id", -1);
            job.savestate_id = section.get_i64("savestate_id", -1);
            job.fake_attacks_by_turn_csv = section.get("fake_attacks_by_turn_csv", "");
            return job;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "plan_id", std::to_string(plan_id));
            doc.set(SECTION_NAME, "savestate_id", std::to_string(savestate_id));
            doc.set(SECTION_NAME, "delta_seed_id", std::to_string(delta_seed_id));
            doc.set(SECTION_NAME, "fake_attacks_by_turn_csv", fake_attacks_by_turn_csv);
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
    };

    struct ResultsIni {
        static constexpr const char* SECTION_NAME = "BattleRun.Results";

        uint32_t     w_err{ 0 };
        uint32_t     dw_err{ 0 };

        uint32_t     vi_start{ 0 };
        uint32_t     vi_end{ 0 };
        std::string  applied_input_tape_text{};
        int64_t      applied_input_artifact_id{ -1 };
        uint32_t     battle_outcome{};

        static inline ResultsIni from_section(const IniDoc& doc) {
            ResultsIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.w_err = section.get_u32("w_err", (uint32_t)simcore::WERR_UnknownError);
            results.dw_err = section.get_u32("dw_err", (uint32_t)simcore::RunToBpOutcome::Unknown);
            results.vi_start = section.get_u32("vi_start", -1);
            results.vi_end = section.get_u32("vi_end", -1);
            results.applied_input_tape_text = section.get("applied_input_tape_text", "");
            results.applied_input_artifact_id = section.get_i64("applied_input_artifact_id", -1);
            results.battle_outcome = section.get_u32("battle_outcome", -1);
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
            doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
            doc.set(SECTION_NAME, "vi_start", std::to_string(vi_start));
            doc.set(SECTION_NAME, "vi_end", std::to_string(vi_end));
            doc.set(SECTION_NAME, "applied_input_tape_text", applied_input_tape_text);
            doc.set(SECTION_NAME, "applied_input_artifact_id", std::to_string(applied_input_artifact_id));
            doc.set(SECTION_NAME, "battle_outcome", std::to_string(battle_outcome));
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
    };

    struct CleanupIni {
        static constexpr const char* SECTION_NAME = "BattleRun.Cleanup";

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

struct ExplorerRunDBCodec final : IProgramDBCodec {
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
