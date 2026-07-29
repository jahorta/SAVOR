#pragma once

#include <cstddef>

#include "KeyIds.h"

namespace savor::context::key::battleend {

#define BATTLE_END_RESULTS_KEYS(X) \
    X(ACCELERATION_POLICY,       0x0501, "battle.end_results.acceleration_policy") \
    X(OUTPUT_SAVESTATE_PATH,     0x0502, "battle.end_results.output_savestate_path") \
    X(OUTCOME,                   0x0503, "battle.end_results.outcome") \
    X(PROVIDER_FAILURE,          0x0504, "battle.end_results.provider_failure") \
    X(RUNTIME_FAILURE,           0x0505, "battle.end_results.runtime_failure") \
    X(MACRO_RESULT,              0x0506, "battle.end_results.macro_result") \
    X(ACTION_COUNT,              0x0507, "battle.end_results.action_count") \
    X(INPUT_REQUEST_COUNT,       0x0508, "battle.end_results.input.request_count") \
    X(INPUT_OBSERVED_COUNT,      0x0509, "battle.end_results.input.observed_count") \
    X(RELEASE_REQUEST_COUNT,     0x050A, "battle.end_results.release.request_count") \
    X(RELEASE_OBSERVED_COUNT,    0x050B, "battle.end_results.release.observed_count") \
    X(LAST_EXPECTED_BP,          0x050C, "battle.end_results.last_expected_bp") \
    X(LAST_HIT_BP,               0x050D, "battle.end_results.last_hit_bp") \
    X(LAST_HIT_PC,               0x050E, "battle.end_results.last_hit_pc") \
    X(LAST_STATE,                0x050F, "battle.end_results.last_state") \
    X(LAST_SUBSTATE,             0x0510, "battle.end_results.last_substate") \
    X(LAST_TOKEN_LO,             0x0511, "battle.end_results.last_token_lo") \
    X(LAST_TOKEN_HI,             0x0512, "battle.end_results.last_token_hi") \
    X(EXPECTED_STAT_WAVES,       0x0513, "battle.end_results.expected.stat_waves") \
    X(OBSERVED_STAT_WAVES,       0x0514, "battle.end_results.observed.stat_waves") \
    X(EXPECTED_LEARNED_WAVES,    0x0515, "battle.end_results.expected.learned_waves") \
    X(OBSERVED_LEARNED_WAVES,    0x0516, "battle.end_results.observed.learned_waves") \
    X(EXPECTED_ITEM_POPUP,       0x0517, "battle.end_results.expected.item_popup") \
    X(OBSERVED_ITEM_POPUP,       0x0518, "battle.end_results.observed.item_popup") \
    X(MISMATCH_FLAGS,            0x0519, "battle.end_results.mismatch_flags") \
    X(INVARIANT_FLAGS,           0x051A, "battle.end_results.invariant_flags") \
    X(SOURCE_INVARIANT_FLAGS,    0x051B, "battle.end_results.source_invariant_flags") \
    X(REWARD_INVARIANT_FLAGS,    0x051C, "battle.end_results.reward_invariant_flags") \
    X(LIFECYCLE_INVARIANT_FLAGS, 0x051D, "battle.end_results.lifecycle_invariant_flags") \
    X(COMPLETION_INVARIANT_FLAGS,0x051E, "battle.end_results.completion_invariant_flags") \
    X(REPORT_BLOB,               0x051F, "battle.end_results.report_blob") \
    X(DIAGNOSTIC,                0x0520, "battle.end_results.diagnostic") \
    X(COMPLETION_MANIFEST_BLOB,  0x0521, "battle.end_results.completion_manifest_blob") \
    X(ENTRY_RNG_SEED,            0x0522, "battle.end_results.entry_rng_seed") \
    X(FINAL_RNG_SEED,            0x0523, "battle.end_results.final_rng_seed") \
    X(RNG_EFFECT_KIND,           0x0524, "battle.end_results.rng_effect_kind") \
    X(RNG_ADVANCE_COUNT,         0x0525, "battle.end_results.rng_advance_count")

#define DECL_KEY(NAME, ID, STR) \
    inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
    static_assert(NAME >= savor::context::key::BATTLE_END_RESULTS_MIN \
        && NAME <= savor::context::key::BATTLE_END_RESULTS_MAX, \
        "battle-end-results key out of range");
    BATTLE_END_RESULTS_KEYS(DECL_KEY)
#undef DECL_KEY

inline constexpr savor::context::key::KeyPair kKeys[] = {
#define ROW(NAME, ID, STR) savor::context::key::KeyPair{static_cast<savor::context::key::KeyId>(ID), STR},
    BATTLE_END_RESULTS_KEYS(ROW)
#undef ROW
};

inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BATTLE_END_RESULTS_KEYS

} // namespace savor::context::key::battleend
