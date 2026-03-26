// SimCore/DB/ProgramDB/SeedProbeDBCodec.h
#pragma once
#include <optional>
#include <string>
#include "IProgramDBCodec.h"
#include "../DBCore/DbResult.h"
#include "../../Runner/Script/PhaseScriptVM.h"
#include "../../Utils/IniDoc.h"
#include "../../Runner/IPC/Wire.h"

using simcore::TriggerCtx;

namespace simcore::db::codec::seedprobe {
    struct GridIni {
        static constexpr const char* SECTION_NAME = "SeedProbe.Grid";

        int samples_per_axis = 5;
        uint8_t min_value = 0;
        uint8_t max_value = 255;
        bool cap_trigger_top = true;
        bool ignore_trigger_minmax = false;

        static inline GridIni from_section(const IniDoc& doc) {
            GridIni grid{};
            if (!doc.has_section(SECTION_NAME)) return grid;
            IniKV section = doc.section_kv(SECTION_NAME);
            grid.samples_per_axis = section.get_i64("samples_per_axis", grid.samples_per_axis);
            grid.min_value = section.get_u8("min_value", grid.min_value);
            grid.max_value = section.get_u8("max_value", grid.max_value);
            grid.cap_trigger_top = section.get_bool("cap_trigger_top", grid.cap_trigger_top);
            grid.ignore_trigger_minmax = section.get_bool("ignore_trigger_minmax", grid.ignore_trigger_minmax);
            return grid;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "samples_per_axis", std::to_string(samples_per_axis));
            doc.set(SECTION_NAME, "min_value", std::to_string(min_value));
            doc.set(SECTION_NAME, "max_value", std::to_string(max_value));
            doc.set(SECTION_NAME, "cap_trigger_top", cap_trigger_top ? "1" : "0");
            doc.set(SECTION_NAME, "ignore_trigger_minmax", ignore_trigger_minmax ? "1" : "0");
        }
    };

    struct UniqueIni {
        static constexpr const char* SECTION_NAME = "SeedProbe.Unique";

        int combo_attempts_per_target = 128;
        int combo_sampler_tries = 8;

        static inline UniqueIni from_section(const IniDoc& doc) {
            UniqueIni unique{};
            if (!doc.has_section(SECTION_NAME)) return unique;
            IniKV section = doc.section_kv(SECTION_NAME);
            unique.combo_attempts_per_target = section.get_i64("combo_attempts_per_target", unique.combo_attempts_per_target);
            unique.combo_sampler_tries = section.get_i64("combo_sampler_tries", unique.combo_sampler_tries);
            return unique;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "combo_attempts_per_target", std::to_string(combo_attempts_per_target));
            doc.set(SECTION_NAME, "combo_sampler_tries", std::to_string(combo_sampler_tries));
        }
    };

    enum SeedProbePhase : uint32_t {
        None,
        Neutral,
        Grid,
        Unique,
        Done
    };

    struct BlueprintIni {
        static constexpr const char* SECTION_NAME = "SeedProbe.Blueprint";

        int64_t root_jobset_id{ -1 };
        int64_t savestate_id{ -1 };
        int64_t probe_id{ -1 };
        uint32_t run_ms;
        uint32_t vi_stall_ms;
        SeedProbePhase cur_phase{ SeedProbePhase::None };
        bool clear_result_winners = true;
        bool auto_schedule_battle_run = false;
        int priority = 0;

        static inline BlueprintIni from_section(const IniDoc& doc) {
            BlueprintIni bp{};
            if (!doc.has_section(SECTION_NAME)) return bp;
            IniKV section = doc.section_kv(SECTION_NAME);
            bp.root_jobset_id = section.get_i64("root_jobset_id", -1);
            bp.savestate_id = section.get_i64("savestate_id", -1);
            bp.probe_id = section.get_i64("probe_id", -1);
            bp.run_ms = section.get_u32("run_ms", 0);
            bp.vi_stall_ms = section.get_u32("vi_stall_ms", 0);
            bp.cur_phase = (SeedProbePhase)section.get_u32("cur_phase", 0);
            bp.clear_result_winners = section.get_bool("clear_result_winners", true);
            bp.auto_schedule_battle_run = section.get_bool("auto_schedule_battle_run", true);
            bp.priority = section.get_i64("priority", 0);
            return bp;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "root_jobset_id", std::to_string(root_jobset_id));
            doc.set(SECTION_NAME, "savestate_id", std::to_string(savestate_id));
            doc.set(SECTION_NAME, "probe_id", std::to_string(probe_id));
            doc.set(SECTION_NAME, "run_ms", std::to_string(run_ms));
            doc.set(SECTION_NAME, "vi_stall_ms", std::to_string(vi_stall_ms));
            doc.set(SECTION_NAME, "cur_phase", std::to_string((uint32_t)cur_phase));
            doc.set(SECTION_NAME, "clear_result_winners", clear_result_winners ? "1" : "0");
            doc.set(SECTION_NAME, "auto_schedule_battle_run", auto_schedule_battle_run ? "1" : "0");
            doc.set(SECTION_NAME, "priority", std::to_string(priority));
        }
    };

    struct JobIni {
        static constexpr const char* SECTION_NAME = "SeedProbe.Job";

        std::string frame_hex{ "" };
        int32_t     expected_delta{ 0 };
        int64_t     delta_set_id{ -1 };
        std::string unique_tag{ "" };

        static inline JobIni from_section(const IniDoc& doc) {
            JobIni job{};
            if (!doc.has_section(SECTION_NAME)) return job;
            IniKV section = doc.section_kv(SECTION_NAME);
            job.frame_hex = section.get("frame_hex", "");
            job.expected_delta = section.get_u32("expected_delta", 0);
            job.delta_set_id = section.get_i64("delta_set_id", -1);
            job.unique_tag = section.get("unique_tag", "");
            return job;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "frame_hex", frame_hex);
            doc.set(SECTION_NAME, "expected_delta", std::to_string(expected_delta));
            doc.set(SECTION_NAME, "delta_set_id", std::to_string(delta_set_id));
            doc.set(SECTION_NAME, "unique_tag", unique_tag);
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc newDoc{ doc };
            set_section(newDoc);
            return newDoc;
        }
    };

    struct ResultsIni {
        static constexpr const char* SECTION_NAME = "SeedProbe.Results";

        uint32_t     w_err{ 0 };
        uint32_t     dw_err{ 0 };

        uint32_t     vi_start{ 0 };
        uint32_t     vi_end{ 0 };

        uint32_t     rng_seed{ 0 };

        static inline ResultsIni from_section(const IniDoc& doc) {
            ResultsIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.rng_seed = section.get_u32("rng_seed", 0);
            results.w_err = section.get_u32("w_err", (uint32_t)simcore::WERR_UnknownError);
            results.dw_err = section.get_u32("dw_err", (uint32_t)simcore::RunToBpOutcome::Unknown);
            results.vi_start = section.get_u32("vi_start", -1);
            results.vi_end = section.get_u32("vi_end", -1);
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "rng_seed", std::to_string(rng_seed));
            doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
            doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
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
        static constexpr const char* SECTION_NAME = "SeedProbe.Cleanup";

        bool clear_winners{ true };

        static inline CleanupIni from_section(const IniDoc& doc) {
            CleanupIni results{};
            if (!doc.has_section(SECTION_NAME)) return results;
            IniKV section = doc.section_kv(SECTION_NAME);
            results.clear_winners = section.get_bool("clear_winners", true);
            return results;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "clear_winners", clear_winners ? "1" : "0");
        }
    };
}

struct SeedProbeDBCodec final : IProgramDBCodec {
    simcore::db::DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    simcore::db::DbResult<simcore::PSJob> decode_job_from_db(int64_t job_id) override;
    simcore::db::DbResult<void> encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    simcore::db::DbResult<void> encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    simcore::db::DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    simcore::db::DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    simcore::db::DbResult<simcore::PSInit>        build_psinit_for_job(int64_t job_id) override;
    simcore::db::DbResult<std::string>            build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
    DbResult<void> phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) override;
    
    DbResult<std::string>            build_artifact_ini_from_db(int64_t job_id) override;
};
