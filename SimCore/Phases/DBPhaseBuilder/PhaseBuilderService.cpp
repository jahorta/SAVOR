#include "PhaseBuilderService.h"
#include "../../Phases/RNGSeedDeltaMap.h"
#include "../../Core/Input/InputPlan.h"

using namespace simcore;
using namespace simcore::db;
using TasBp = db::codec::tas::BlueprintIni;
using BRBp = db::codec::battle::run::BlueprintIni;
using SPBp = db::codec::seedprobe::BlueprintIni;
using SPGrid = db::codec::seedprobe::GridIni;
using SPUni = db::codec::seedprobe::UniqueIni;
using db::codec::seedprobe::SeedProbePhase;

namespace simcore::db::phasebuilder {

    IniDoc PhaseBuilderService::DefaultsFor(int program_kind) {
        return SchemaComposer::DefaultIniFor(program_kind);
    }

    std::vector<ValidationError> PhaseBuilderService::Validate(int program_kind, const IniDoc& ini) {
        std::vector<ValidationError> errs;

        switch (program_kind) {
        case PK_TasMovie: {
            auto bp = TasBp::from_section(ini);
            if (bp.base_dtm_artifact_id <= 0) errs.push_back({ "TasMovie.Blueprint.base_dtm_artifact_id","required" });
            if (bp.rtc_high < bp.rtc_low) errs.push_back({ "TasMovie.Blueprint.rtc_high","rtc_high must be >= rtc_low" });
            if (bp.base_dtm_artifact_id > 0) {
                auto orow = ObjectStore::Get(bp.base_dtm_artifact_id);
                if (!orow.ok) errs.push_back({ "TasMovie.Blueprint.base_dtm_artifact_id","artifact not found" });
            }
            break;
        }
        case PK_SeedProbe: {
            auto bp = SPBp::from_section(ini);
            auto grid = SPGrid::from_section(ini);
            auto uni = SPUni::from_section(ini);
            if (bp.savestate_id <= 0) errs.push_back({ "SeedProbe.Blueprint.savestate_id","required" });
            if (grid.samples_per_axis <= 0) errs.push_back({ "SeedProbe.Grid.samples_per_axis","must be > 0" });
            if (grid.min_value > grid.max_value) errs.push_back({ "SeedProbe.Grid.min_value","min_value must be <= max_value" });
            if (uni.combo_attempts_per_target <= 0) errs.push_back({ "SeedProbe.Unique.combo_attempts_per_target","must be > 0" });
            if (uni.combo_sampler_tries <= 0) errs.push_back({ "SeedProbe.Unique.combo_sampler_tries","must be > 0" });          
            if (bp.savestate_id > 0) {
                auto pr = SavestateRepo::Get(bp.savestate_id);
                if (!pr.ok) errs.push_back({ "SeedProbe.Blueprint.savestate_id","savestate not found" });
            }
            break;
        }
        case PK_BattleTurnRunner: {
            auto bp = BRBp::from_section(ini);
            if (bp.settings_id <= 0) errs.push_back({ "BattleRun.Blueprint.settings_id","required" });
            if (bp.seed_probe_id <= 0) errs.push_back({ "BattleRun.Blueprint.seed_probe_id","required" });
            if (bp.settings_id > 0) {
                auto s = ExplorerSettingsRepo::Get(bp.settings_id);
                if (!s.ok) errs.push_back({ "BattleRun.Blueprint.settings_id","settings not found" });
            }
            if (bp.seed_probe_id > 0) {
                auto d = SeedProbeRepo::Get(bp.seed_probe_id);
                if (!d.ok || d.value.status != "done") errs.push_back({ "BattleRun.Blueprint.seed_probe_id","seed probe not found or not done" });
            }
            break;
        }
        default: break;
        }

        return errs;
    }

    DbResult<PhasePreview> PhaseBuilderService::Preview(int program_kind, const IniDoc& ini) {
        PhasePreview out{};
        out.program_kind = program_kind;

        switch (program_kind) {
        case PK_TasMovie: {
            auto r = PreviewTas(ini); if (!r.ok) return DbResult<PhasePreview>::Err(r.error);
            out.tasmovie = r.value; break;
        }
        case PK_SeedProbe: {
            auto r = PreviewSeedProbe(ini); if (!r.ok) return DbResult<PhasePreview>::Err(r.error);
            out.seedprobe = r.value; break;
        }
        case PK_BattleTurnRunner: {
            auto r = PreviewExplorer(ini); if (!r.ok) return DbResult<PhasePreview>::Err(r.error);
            out.explorer = r.value; break;
        }
        default: break;
        }
        return DbResult<PhasePreview>::Ok(std::move(out));
    }

    DbResult<TasMoviePreview> PhaseBuilderService::PreviewTas(const IniDoc& ini) {
        TasMoviePreview p{};
        auto bp = TasBp::from_section(ini);
        p.rtc_low = bp.rtc_low;
        p.rtc_high = bp.rtc_high;
        if (bp.rtc_high >= bp.rtc_low) p.jobs = (bp.rtc_high - bp.rtc_low + 1);
        if (bp.base_dtm_artifact_id > 0) {
            auto r = ObjectStore::Get(bp.base_dtm_artifact_id);
            if (r.ok) {
                p.artifact_exists = true;
                p.artifact_size = r.value.size;
            }
        }
        return DbResult<TasMoviePreview>::Ok(std::move(p));
    }

    DbResult<SeedProbePreview> PhaseBuilderService::PreviewSeedProbe(const IniDoc& ini) {
        SeedProbePreview p{};
        auto bp = SPBp::from_section(ini);
        auto grid = SPGrid::from_section(ini);
        auto uni = SPUni::from_section(ini);

        if (grid.samples_per_axis > 0) {
            auto v1 = simcore::build_grid_main(grid.samples_per_axis, grid.min_value, grid.max_value);
            auto v2 = simcore::build_grid_cstick(grid.samples_per_axis, grid.min_value, grid.max_value);
            auto v3 = simcore::build_grid_trig(grid.samples_per_axis,
                grid.ignore_trigger_minmax ? 0 : grid.min_value,
                grid.ignore_trigger_minmax ? 255 : grid.max_value,
                grid.cap_trigger_top);
            p.grid_jobs = static_cast<int64_t>(v1.size() + v2.size() + v3.size());
        }

        if (bp.probe_id > 0) {
            auto dr = DeltaSeedRepo::ListGridForProbe(bp.probe_id);
            if (dr.ok && !dr.value.empty()) {
                uint32_t base_seed = 0;
                auto pr = SeedProbeRepo::Get(bp.probe_id);
                if (pr.ok && pr.value.neutral_seed != 0) base_seed = static_cast<uint32_t>(pr.value.neutral_seed);

                simcore::RandSeedProbeResult grid_res{ .base_seed = base_seed };
                for (auto& ds : dr.value) {
                    auto fam = static_cast<simcore::SeedFamily>(ds.input.get_family());
                    uint8_t x = 0, y = 0;
                    switch (fam) {
                    case simcore::SeedFamily::Main:     x = ds.input.main_x;  y = ds.input.main_y;  break;
                    case simcore::SeedFamily::CStick:   x = ds.input.c_x;     y = ds.input.c_y;     break;
                    case simcore::SeedFamily::Triggers: x = ds.input.trig_l;  y = ds.input.trig_r;  break;
                    default: break;
                    }
                    simcore::RandSeedProbeEntry e{};
                    e.family = fam; e.x = x; e.y = y; e.seed = base_seed + ds.seed_delta; e.delta = ds.seed_delta; e.ok = true;
                    grid_res.entries.push_back(e);
                }
                auto samples = simcore::PlanJCTComboSamples(grid_res, (uint32_t)uni.combo_attempts_per_target, (uint32_t)uni.combo_sampler_tries);
                int64_t count = 0;
                for (auto& s : samples.samples) count += static_cast<int64_t>(s.frames.size());
                p.unique_jobs = count;
            }
            else {
                p.unique_deferred = true;
            }
        }
        return DbResult<SeedProbePreview>::Ok(std::move(p));
    }

    DbResult<ExplorerRunPreview> PhaseBuilderService::PreviewExplorer(const IniDoc& ini) {
        ExplorerRunPreview p{};
        auto bp = BRBp::from_section(ini);

        if (bp.settings_id > 0) {
            auto lr = ExplorerSettingsPredicateRepo::List(bp.settings_id);
            if (lr.ok) p.predicate_count = static_cast<int32_t>(lr.value.size());
        }
        return DbResult<ExplorerRunPreview>::Ok(std::move(p));
    }

    DbResult<SubmitResult> PhaseBuilderService::Submit(
        const std::string& purpose,
        int program_kind,
        const IniDoc& ini,
        std::optional<std::string> meta_text,
        std::optional<int64_t> expected_total_override)
    {
        std::optional<int64_t> expected_total{};

        if (expected_total_override.has_value()) {
            expected_total = expected_total_override;
        }
        else {
            if (program_kind == PK_TasMovie) {
                auto tp = PreviewTas(ini); if (tp.ok) expected_total = tp.value.jobs;
            }
            else if (program_kind == PK_BattleTurnRunner) {
                expected_total = 1;
            }
            else {
                expected_total.reset(); // SeedProbe left for codec to update later
            }
        }

        auto jsr = JobSetsRepo::Create(
            purpose, program_kind,
            std::nullopt, std::nullopt, std::nullopt,
            meta_text, expected_total);
        if (!jsr.ok) return DbResult<SubmitResult>::Err(jsr.error);

        const int64_t job_set_id = jsr.value;

        auto& codec = ProgramDBCodecRegistry::for_kind(program_kind);
        auto enc = codec.encode_job_into_db(job_set_id, ini.to_string_sorted());
        if (!enc.ok) return DbResult<SubmitResult>::Err(enc.error);

        SubmitResult out{ job_set_id, program_kind };
        return DbResult<SubmitResult>::Ok(out);
    }

} // namespace simcore::db::phasebuilder
