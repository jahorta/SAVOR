#include "ProgramRegistry.h"
#include "SeedProbe/SeedProbePayload.h"
#include "SeedProbe/SeedProbeScript.h"
#include "PlayTasMovie/TasMoviePayload.h"
#include "PlayTasMovie/TasMovieScript.h"
#include "BattleRunner/BattleRunnerPayload.h"
#include "BattleRunner/BattleRunnerScript.h"
#include "BattleContext/BattleContextScript.h"
#include "BattleContext/BattleContextPayload.h"
#include "BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "BattleTurnRunner/BattleTurnRunnerScript.h"
#include "../../DB/ProgramDB/SeedProbeDBCodec.h"
#include "../../DB/ProgramDB/TasMovieDBCodec.h"
#include "../../DB/ProgramDB/ExplorerRunDBCodec.h"
#include "../../DB/ProgramDB/BattleContextDBCodec.h"
#include "../../DB/ProgramDB/BattleSingleTurnRunDBCodec.h"
#include "../../Runner/IPC/Wire.h"

namespace simcore::programs {

    PhaseScript build_main_program(uint8_t program_kind)
    {
        switch (program_kind) {
        case PK_SeedProbe:
            // SeedProbe fixed program should use APPLY_INPUT_FROM("seed.gc.input") etc.
            return seedprobe::MakeSeedProbeProgram();
        case PK_TasMovie:
            // TAS fixed program should use *_FROM("tas.*") keys (id6, dtm_path, run_ms, save_path)
            return tasmovie::MakeTasMovieProgram();
        case PK_BattleTurnRunner:
            return phase::battle::runner::MakeBattleRunnerProgram();
        case PK_BattleContextProbe:
            return phase::battle::ctx::MakeBattleContextProbeProgram();
        case PK_BattleSingleTurnRunner:
            return phase::battle::turnrunner::MakeBattleTurnRunnerProgram();
        default:
            return PhaseScript{};
        }
    }

    bool decode_payload_for(uint8_t active_program_kind,
        const std::vector<uint8_t>& payload,
        PSContext& out_ctx)
    {
        if (payload.empty()) return false;
        const uint8_t tag = payload[0];

        // NOTE: We deliberately keep the worker ignorant of tags:
        // the registry checks tag vs active kind here and delegates to the right decoder.
        if (tag != active_program_kind) return false;

        switch (active_program_kind) {
        case PK_SeedProbe:
            return seedprobe::decode_payload(payload, out_ctx);
        case PK_TasMovie:
            return tasmovie::decode_payload(payload, out_ctx);
        case PK_BattleTurnRunner:          
            return phase::battle::runner::decode_payload(payload, out_ctx);
        case PK_BattleContextProbe:
            return phase::battle::ctx::decode_payload(payload, out_ctx);
        case PK_BattleSingleTurnRunner:
            return phase::battle::turnrunner::decode_payload(payload, out_ctx);
        default:
            return false;
        }
    }

    const RetryTuningInfo* get_retry_tuning_info(uint8_t program_kind)
    {
        static const RetryTuningInfo seedprobe_info{
            simcore::db::codec::seedprobe::BlueprintIni::SECTION_NAME,
            simcore::db::codec::seedprobe::ResultsIni::SECTION_NAME
        };
        static const RetryTuningInfo tasmovie_info{
            simcore::db::codec::tas::BlueprintIni::SECTION_NAME,
            simcore::db::codec::tas::ResultsIni::SECTION_NAME
        };
        static const RetryTuningInfo battleturn_info{
            simcore::db::codec::battle::run::BlueprintIni::SECTION_NAME,
            simcore::db::codec::battle::run::ResultsIni::SECTION_NAME
        };
        static const RetryTuningInfo battlecontext_info{
            simcore::db::battle::ctx::BlueprintIni::SECTION_NAME,
            simcore::db::battle::ctx::ResultsIni::SECTION_NAME
        };
        static const RetryTuningInfo battlest_info{
            simcore::db::codec::battle::run::BlueprintIni::SECTION_NAME,
            simcore::db::codec::battle::singleturn::ResultsIni::SECTION_NAME
        };

        switch (program_kind) {
        case PK_SeedProbe: return &seedprobe_info;
        case PK_TasMovie: return &tasmovie_info;
        case PK_BattleTurnRunner: return &battleturn_info;
        case PK_BattleContextProbe: return &battlecontext_info;
        case PK_BattleSingleTurnRunner: return &battlest_info;
        default: return nullptr;
        }
    }

} // namespace simcore::programs
