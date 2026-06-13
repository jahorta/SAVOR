#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::battle {

#define BATTLE_KEYS(X) \
  X(ACTIVE_TURN,              0x0300, "battle.active_turn")   \
  X(INITIAL_INPUT,            0x0301, "battle.initial_input") \
  X(BATTLE_OUTCOME,           0x0302, "battle.outcome_code") \
  X(INPUTPLAN_FRAME_COUNT,    0x0311, "battle.inputplan.frame_count") \
  X(INPUTPLAN,                0x0312, "battle.inputplan.frames") \
  X(CTX_BLOB,                 0x0320, "battle.CTX_BLOB")      \
  X(NUM_TURN_PLANS,           0x0330, "battle.turnplan.count")     \
  X(TURN_PLANS,               0x0331, "battle.turnplan.plans") \
  X(LAST_TURN,                0x0332, "battle.turnplan.last_idx") \
  X(PLAN_MATERIALIZE_ERR,     0x0333, "battle.turnplan.materialize_err") \
  X(TURN_INPUT_INDEX,         0x0334, "battle.turn.input_index") \
  X(TURN_OUTPUT_INDEX,        0x0335, "battle.turn.output_index") \
  X(HAS_INITIAL_INPUT,        0x0336, "battle.turn.has_initial_input") \
  X(FAKE_ATTACK_COUNT_THIS_TURN, 0x0337, "battle.turn.fake_attacks_this_turn") \
  X(FAKE_ATTACK_BUDGET_MAX,   0x0338, "battle.turn.fake_attacks_budget_max") \
  X(FAKE_ATTACK_USED_BEFORE,  0x0339, "battle.turn.fake_attacks_used_before") \
  X(OUTPUT_SAVESTATE_PATH,    0x033A, "battle.turn.output_savestate_path") \
  X(APPLIED_INPUTPLAN_COUNT,  0x033B, "battle.inputplan.applied_frame_count") \
  X(APPLIED_INPUTPLAN,        0x033C, "battle.inputplan.applied_frames") \
  X(APPLIED_INPUTPLAN_VI_DURATIONS,0x033E, "battle.inputplan.applied_vi_durations") \
  X(APPLIED_INPUTPLAN_TURN_BLOB,0x033F, "battle.inputplan.applied_turn_blob") \
  X(INPUT_PLAYBACK_ERR,       0x0340, "battle.input.playback_err") \
  X(INPUT_RETRY_COUNT,        0x0341, "battle.input.retry_count") \
  X(INPUT_PLAYBACK_UNACKED,   0x0342, "battle.input.unacked_count") \
  X(MACRO_MODE,               0x0343, "battle.macro.mode") \
  X(MACRO_TARGET_SLOT,        0x0344, "battle.macro.target_slot") \
  X(MACRO_RESULT,             0x0345, "battle.macro.result") \
  X(MACRO_FAILURE_CODE,       0x0346, "battle.macro.failure_code") \
  X(MACRO_STEP_COUNT,         0x0347, "battle.macro.step_count") \
  X(MACRO_LAST_STEP_INDEX,    0x0348, "battle.macro.last_step_index") \
  X(MACRO_LAST_EXPECTED_BP,   0x0349, "battle.macro.last_expected_bp") \
  X(MACRO_LAST_HIT_BP,        0x034A, "battle.macro.last_hit_bp") \
  X(MACRO_LAST_HIT_PC,        0x034B, "battle.macro.last_hit_pc") \
  X(MACRO_PLAN_BLOB,          0x034C, "battle.macro.plan_blob") \
  X(MACRO_TRANSITION_NEUTRAL_FRAMES,0x034D, "battle.macro.transition_neutral_frames")

#define DECL_KEY(NAME, ID, STR) inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
static_assert(NAME >= savor::context::key::BATTLE_MIN && NAME <= savor::context::key::BATTLE_MAX, "battle key out of range");
	BATTLE_KEYS(DECL_KEY)
#undef DECL_KEY

		inline constexpr savor::context::key::KeyPair kKeys[] = {
		#define ROW(NAME, ID, STR) savor::context::key::KeyPair{ static_cast<savor::context::key::KeyId>(ID), STR },
		BATTLE_KEYS(ROW)
		#undef ROW
	};
	inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef BATTLE_KEYS

} // namespace savor::context::key::battle
