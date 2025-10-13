#include "IProgramDBCodec.h"
#include <unordered_map>

#include "SeedProbeDBCodec.h"
#include "TasMovieDBCodec.h"
#include "ExplorerRunDBCodec.h"
#include "BattleContextDBCodec.h"

#include "../../Runner/IPC/Wire.h"

namespace simcore::db::codec {
    void ensure_codecs_registered() {
        static std::once_flag once;
        std::call_once(once, [] {
            static SeedProbeDBCodec  seed_codec;
            static TasMovieDBCodec   tas_codec;
            static ExplorerRunDBCodec battle_runner_codec;
            static BattleContextDBCodec battle_context_codec;

            // Use your existing ProgramKind ids here:
            ProgramDBCodecRegistry::register_codec(simcore::PK_SeedProbe, &seed_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_TasMovie, &tas_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_BattleTurnRunner, &battle_runner_codec);
            ProgramDBCodecRegistry::register_codec(simcore::PK_BattleContextProbe, &battle_context_codec);
            });
    }
}

static std::unordered_map<int, IProgramDBCodec*>* g_map;

void ProgramDBCodecRegistry::register_codec(int program_kind, IProgramDBCodec* impl) {
    if (!g_map) g_map = new std::unordered_map<int, IProgramDBCodec*>();
    (*g_map)[program_kind] = impl;
}
IProgramDBCodec& ProgramDBCodecRegistry::for_kind(int program_kind) {
    return *(*g_map)[program_kind];
}
