#pragma once

#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::battlecompletion {

#define BATTLE_COMPLETION_KEYS(X) \
    X(OUTPUT_SAVESTATE_PATH,       0x0601, "battle.completion.output_savestate_path") \
    X(OUTCOME,                     0x0602, "battle.completion.outcome") \
    X(PROVIDER_FAILURE,            0x0603, "battle.completion.provider_failure") \
    X(RUNTIME_FAILURE,             0x0604, "battle.completion.runtime_failure") \
    X(MACRO_RESULT,                0x0605, "battle.completion.macro_result") \
    X(MANIFEST_BLOB,               0x0606, "battle.completion.manifest_blob") \
    X(DIAGNOSTIC,                  0x0607, "battle.completion.diagnostic") \
    X(INVARIANT_FLAGS,             0x0608, "battle.completion.invariant_flags") \
    X(LAST_EXPECTED_BP,            0x0609, "battle.completion.last_expected_bp") \
    X(LAST_HIT_BP,                 0x060A, "battle.completion.last_hit_bp") \
    X(LAST_HIT_PC,                 0x060B, "battle.completion.last_hit_pc") \
    X(EXPECTED_LEVEL_PANELS,       0x060C, "battle.completion.expected.level_panels") \
    X(EXPECTED_STAT_WAVES,         0x060D, "battle.completion.expected.stat_waves") \
    X(EXPECTED_MAGIC_RANK_EVENTS,  0x060E, "battle.completion.expected.magic_rank_events") \
    X(EXPECTED_LEARNED_WAVES,      0x060F, "battle.completion.expected.learned_waves") \
    X(EXPECTED_ITEM_POPUPS,        0x0610, "battle.completion.expected.item_popups") \
    X(START_VI_LO,                 0x0611, "battle.completion.start_vi_lo") \
    X(START_VI_HI,                 0x0612, "battle.completion.start_vi_hi") \
    X(END_VI_LO,                   0x0613, "battle.completion.end_vi_lo") \
    X(END_VI_HI,                   0x0614, "battle.completion.end_vi_hi")

#define DECL_KEY(NAME, ID, STR) \
    inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
    static_assert(NAME >= savor::context::key::BATTLE_COMPLETION_MIN \
        && NAME <= savor::context::key::BATTLE_COMPLETION_MAX, \
        "battle-completion key out of range");
    BATTLE_COMPLETION_KEYS(DECL_KEY)
#undef DECL_KEY

inline constexpr savor::context::key::KeyPair kKeys[] = {
#define ROW(NAME, ID, STR) savor::context::key::KeyPair{static_cast<savor::context::key::KeyId>(ID), STR},
    BATTLE_COMPLETION_KEYS(ROW)
#undef ROW
};
inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BATTLE_COMPLETION_KEYS

} // namespace savor::context::key::battlecompletion
