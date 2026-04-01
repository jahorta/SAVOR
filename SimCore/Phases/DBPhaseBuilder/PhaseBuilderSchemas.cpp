#include "PhaseBuilderSchemas.h"

using namespace simcore;
using namespace simcore::db;
using TasBp = simcore::db::codec::tas::BlueprintIni;
using BRBp = simcore::db::codec::battle::run::BlueprintIni;
using SPBp = simcore::db::codec::seedprobe::BlueprintIni;
using SPGrid = simcore::db::codec::seedprobe::GridIni;
using SPUni = simcore::db::codec::seedprobe::UniqueIni;
using simcore::db::codec::seedprobe::SeedProbePhase;

namespace simcore::db::phasebuilder {

    IniDoc SchemaComposer::DefaultTasMovie() {
        IniDoc doc{};
        TasBp bp{};
        bp.base_dtm_artifact_id = -1;
        bp.rtc_low = 0;
        bp.rtc_high = 0;
        bp.priority = 0;
        bp.run_ms = 0u;
        bp.vi_stall_ms = 0u;
        bp.progress_enable = true;
        bp.auto_queue_seeds = false;
        bp.set_section(doc);

        SPBp sp = SPBp::from_section(DefaultSeedProbe());
        sp.cur_phase = SeedProbePhase::None;
        sp.probe_id = -1;
        sp.savestate_id = -1;
        sp.set_section(doc);

        SPGrid grid = SPGrid::from_section(DefaultSeedProbe());
        grid.set_section(doc);

        SPUni uni = SPUni::from_section(DefaultSeedProbe());
        uni.set_section(doc);

        BRBp br = BRBp::from_section(DefaultExplorerRun());
        br.set_section(doc);
        return doc;
    }

    IniDoc SchemaComposer::DefaultSeedProbe() {
        IniDoc doc{};

        SPBp bp{};
        bp.probe_id = -1;
        bp.run_ms = 30000u;
        bp.vi_stall_ms = 4000u;
        bp.cur_phase = SeedProbePhase::Neutral;
        bp.clear_result_winners = true;
        bp.auto_schedule_battle_run = false;
        bp.set_section(doc);

        SPGrid grid{};
        grid.samples_per_axis = 20;
        grid.min_value = 48;
        grid.max_value = 207;
        grid.cap_trigger_top = true;
        grid.ignore_trigger_minmax = true;
        grid.set_section(doc);

        SPUni uni{};
        uni.combo_attempts_per_target = 128;
        uni.combo_sampler_tries = 8;
        uni.set_section(doc);

        return doc;
    }

    IniDoc SchemaComposer::DefaultExplorerRun() {
        IniDoc doc{};
        BRBp bp{};
        bp.settings_id = -1;
        bp.seed_probe_id = -1;
        bp.priority = 0;
        bp.run_ms = 200000u;
        bp.vi_stall_ms = 5000u;
        bp.progress_enable = true;
        bp.use_single_turn_runner = true;
        bp.auto_wave_trigger_enable = false;
        bp.min_fake_attacks = 0;
        bp.max_fake_attacks = 0;
        bp.set_section(doc);
        return doc;
    }

    IniDoc SchemaComposer::DefaultIniFor(int program_kind) {
        switch (program_kind) {
        case PK_SeedProbe:        return DefaultSeedProbe();
        case PK_TasMovie:         return DefaultTasMovie();
        case PK_BattleTurnRunner: return DefaultExplorerRun();
        case PK_BattleSingleTurnRunner: return DefaultExplorerRun();
        default:                  return IniDoc{};
        }
    }

} // namespace simcore::db::phasebuilder
