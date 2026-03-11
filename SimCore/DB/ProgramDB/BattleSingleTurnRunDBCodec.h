#pragma once
#include "IProgramDBCodec.h"
#include "ExplorerRunDBCodec.h"
#include "../../Utils/IniDoc.h"

using simcore::TriggerCtx;

namespace simcore::db::codec::battle::singleturn {

    struct JobIni {
        static constexpr const char* SECTION_NAME = "BattleSingleTurn.Job";

        int64_t plan_id{-1};
        int64_t delta_seed_id{-1};
        int64_t savestate_id{-1};
        uint32_t turn_index{1};
        uint32_t fake_attacks_used_before{0};
        std::string action_key;

        static inline JobIni from_section(const IniDoc& doc) {
            JobIni job{};
            if (!doc.has_section(SECTION_NAME)) return job;
            IniKV section = doc.section_kv(SECTION_NAME);
            job.plan_id = section.get_i64("plan_id", -1);
            job.delta_seed_id = section.get_i64("delta_seed_id", -1);
            job.savestate_id = section.get_i64("savestate_id", -1);
            job.turn_index = section.get_u32("turn_index", 1);
            job.fake_attacks_used_before = section.get_u32("fake_attacks_used_before", 0);
            job.action_key = section.get("action_key", "");
            return job;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "plan_id", std::to_string(plan_id));
            doc.set(SECTION_NAME, "delta_seed_id", std::to_string(delta_seed_id));
            doc.set(SECTION_NAME, "savestate_id", std::to_string(savestate_id));
            doc.set(SECTION_NAME, "turn_index", std::to_string(turn_index));
            doc.set(SECTION_NAME, "fake_attacks_used_before", std::to_string(fake_attacks_used_before));
            doc.set(SECTION_NAME, "action_key", action_key);
        }
        IniDoc append_section(const IniDoc& doc) const {
            IniDoc d{ doc };
            set_section(d);
            return d;
        }
    };

    struct ResultsIni {
        static constexpr const char* SECTION_NAME = "BattleSingleTurn.Results";
        uint32_t w_err{0};
        uint32_t dw_err{0};
        uint32_t vi_start{0};
        uint32_t vi_end{0};
        uint32_t rng_seed{0};
        uint32_t battle_outcome{0};
        uint32_t fake_attacks_used{0};
        uint32_t pred_passed{0};
        uint32_t pred_total{0};
        uint32_t pred_abort_run{0};
        std::string savestate_path;
        int64_t output_savestate_id{-1};

        static inline ResultsIni from_section(const IniDoc& doc) {
            ResultsIni r{};
            if (!doc.has_section(SECTION_NAME)) return r;
            IniKV section = doc.section_kv(SECTION_NAME);
            r.w_err = section.get_u32("w_err", 0);
            r.dw_err = section.get_u32("dw_err", 0);
            r.vi_start = section.get_u32("vi_start", 0);
            r.vi_end = section.get_u32("vi_end", 0);
            r.rng_seed = section.get_u32("rng_seed", 0);
            r.battle_outcome = section.get_u32("battle_outcome", 0);
            r.fake_attacks_used = section.get_u32("fake_attacks_used", 0);
            r.pred_passed = section.get_u32("pred_passed", 0);
            r.pred_total = section.get_u32("pred_total", 0);
            r.pred_abort_run = section.get_u32("pred_abort_run", 0);
            r.savestate_path = section.get("savestate_path", "");
            r.output_savestate_id = section.get_i64("output_savestate_id", -1);
            return r;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "w_err", std::to_string(w_err));
            doc.set(SECTION_NAME, "dw_err", std::to_string(dw_err));
            doc.set(SECTION_NAME, "vi_start", std::to_string(vi_start));
            doc.set(SECTION_NAME, "vi_end", std::to_string(vi_end));
            doc.set(SECTION_NAME, "rng_seed", std::to_string(rng_seed));
            doc.set(SECTION_NAME, "battle_outcome", std::to_string(battle_outcome));
            doc.set(SECTION_NAME, "fake_attacks_used", std::to_string(fake_attacks_used));
            doc.set(SECTION_NAME, "pred_passed", std::to_string(pred_passed));
            doc.set(SECTION_NAME, "pred_total", std::to_string(pred_total));
            doc.set(SECTION_NAME, "pred_abort_run", std::to_string(pred_abort_run));
            doc.set(SECTION_NAME, "savestate_path", savestate_path);
            doc.set(SECTION_NAME, "output_savestate_id", std::to_string(output_savestate_id));
        }
    };

    struct WaveIni {
        static constexpr const char* SECTION_NAME = "BattleSingleTurn.Wave";
        uint32_t cur_turn{1};

        static inline WaveIni from_section(const IniDoc& doc) {
            WaveIni w{};
            if (!doc.has_section(SECTION_NAME)) return w;
            IniKV section = doc.section_kv(SECTION_NAME);
            w.cur_turn = section.get_u32("cur_turn", 1);
            return w;
        }
        inline void set_section(IniDoc& doc) const {
            doc.ensure_section(SECTION_NAME);
            doc.set(SECTION_NAME, "cur_turn", std::to_string(cur_turn));
        }
    };
}

struct BattleSingleTurnRunDBCodec final : IProgramDBCodec {
    DbResult<int64_t> encode_job_into_db(int64_t job_set_id, const std::string& blueprint_ini) override;
    DbResult<simcore::PSJob> decode_job_from_db(int64_t job_id) override;
    DbResult<void> encode_progress_into_db(int64_t job_id, const std::string& progress_line) override;
    DbResult<void> encode_results_into_db(int64_t job_id, const std::string& results_ini, bool success) override;
    DbResult<std::string> decode_progress_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::string> decode_results_from_db(std::optional<int64_t> job_id, std::optional<int64_t> job_set_id) override;
    DbResult<std::optional<int64_t>> get_required_savestate_id(int64_t job_id) override;
    DbResult<simcore::PSInit> build_psinit_for_job(int64_t job_id) override;
    DbResult<std::string> build_results_ini_from_prresult(int64_t job_id, const simcore::PRResult& r) override;
    DbResult<std::string> build_artifact_ini_from_db(int64_t job_id) override;
    DbResult<void> phase_setup_on_trigger(const TriggerCtx& ctx, const std::string& action_args_ini) override;

    static DbResult<int64_t> enqueue_next_wave_from_job(int64_t source_job_id, bool auto_wave_trigger_enable = false);
};
