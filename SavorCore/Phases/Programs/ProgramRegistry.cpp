#include "ProgramRegistry.h"
#include "SeedProbe/SeedProbePayload.h"
#include "SeedProbe/SeedProbeScript.h"
#include "PlayTasMovie/TasMoviePayload.h"
#include "PlayTasMovie/TasMovieScript.h"
#include "TasFrameDetector/TasFrameDetectorPayload.h"
#include "TasFrameDetector/TasFrameDetectorScript.h"
#include "BattleRunner/BattleRunnerPayload.h"
#include "BattleRunner/BattleRunnerScript.h"
#include "BattleContext/BattleContextScript.h"
#include "BattleContext/BattleContextPayload.h"
#include "BattleTurnRunner/BattleTurnRunnerPayload.h"
#include "BattleTurnRunner/BattleTurnRunnerScript.h"
#include "BattleMacroProbe/BattleMacroProbePayload.h"
#include "BattleMacroProbe/BattleMacroProbeScript.h"
#include "BattleEndResults/BattleEndResultsPayload.h"
#include "BattleEndResults/BattleEndResultsScript.h"
#include "BattleCompletion/BattleCompletionPayload.h"
#include "BattleCompletion/BattleCompletionScript.h"
#include "NavigationContext/NavigationContextPayload.h"
#include "NavigationContext/NavigationContextScript.h"
#include "../../Runner/IPC/Wire.h"

namespace savor::programs {

    PhaseScript build_main_program(uint8_t program_kind)
    {
        switch (program_kind) {
        case PK_SeedProbe:
            // SeedProbe fixed program should use APPLY_INPUT_FROM("seed.gc.input") etc.
            return seedprobe::MakeSeedProbeProgram();
        case PK_TasMovie:
            // TAS fixed program uses context keys for the disc, movie, and output state.
            return tasmovie::MakeTasMovieProgram();
        case PK_TasInputStreamDetector:
            return tasframedetector::MakeTasFrameDetectorProgram();
        case PK_BattleTurnRunner:
            return phase::battle::runner::MakeBattleRunnerProgram();
        case PK_BattleContextProbe:
            return phase::battle::ctx::MakeBattleContextProbeProgram();
        case PK_BattleSingleTurnRunner:
            return phase::battle::turnrunner::MakeBattleTurnRunnerProgram();
        case PK_BattleMacroProbe:
            return phase::battle::macroprobe::MakeBattleMacroProbeProgram();
        case PK_BattleResultsScreenRunner:
            return phase::battle::endresults::MakeBattleResultsScreenProgram();
        case PK_BattleCompletionRunner:
            return phase::battle::completion::MakeBattleCompletionProgram();
        case PK_NavigationContextRunner:
            return phase::navigation::ctx::MakeNavigationContextProgram();
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
        case PK_TasInputStreamDetector:
            return tasframedetector::decode_payload(payload, out_ctx);
        case PK_BattleTurnRunner:          
            return phase::battle::runner::decode_payload(payload, out_ctx);
        case PK_BattleContextProbe:
            return phase::battle::ctx::decode_payload(payload, out_ctx);
        case PK_BattleSingleTurnRunner:
            return phase::battle::turnrunner::decode_payload(payload, out_ctx);
        case PK_BattleMacroProbe:
            return phase::battle::macroprobe::decode_payload(payload, out_ctx);
        case PK_BattleResultsScreenRunner:
            return phase::battle::endresults::decode_payload(payload, out_ctx);
        case PK_BattleCompletionRunner:
            return phase::battle::completion::decode_payload(payload, out_ctx);
        case PK_NavigationContextRunner:
            return phase::navigation::ctx::decode_payload(payload, out_ctx);
        default:
            return false;
        }
    }

} // namespace savor::programs
