#pragma once
#include <cstddef>
#include "KeyIds.h"

namespace savor::context::key::core {

	// One-line per key via X-macro: NAME, ID, "string"
#define CORE_KEYS(X) \
  X(RUN_HIT_PC,              0x0000, "core.run.hit_pc")        \
  X(RUN_HIT_BP_KEY,          0x0001, "core.run.hit_bp")        \
  X(DW_RUN_OUTCOME_CODE,     0x0002, "core.run.outcome_code")  \
  X(ELAPSED_MS,              0x0003, "core.run.elapsed_ms")    \
  X(LAST_SAVESTATE_PATH,     0x0004, "core.run.last_savestate_path")    \
  X(GAME_ISO_PATH,           0x0005, "core.run.game_iso_path")    \
  X(RUN_EXPECTED_MATCH,      0x0006, "core.run.expected_match")    \
\
  X(VI_FIRST,                0x0020, "core.metrics.vi_first")  \
  X(VI_LAST,                 0x0021, "core.metrics.vi_last")   \
  X(POLL_MS,                 0x0022, "core.metrics.poll_ms")   \
  X(VI_DELTA,                0x0023, "core.metrics.vi_delta")   \
\
  X(RUN_MS,                  0x0040, "core.input.run_ms")      \
  X(VI_STALL_MS,             0x0041, "core.input.vi_stall_ms") \
  X(PROGRESS_RATE,           0x0042, "core.input.progress_rate") \
  X(PROGRESS_CORE_FLAGS,     0x0043, "core.input.progress_core_flags") \
  X(RUN_POLL_MS,             0x0044, "core.input.run_poll_ms") \
  X(CAPTURE_PROFILE_PATH,    0x0045, "core.capture.profile_path") \
  X(CAPTURE_OUTPUT_PATH,     0x0046, "core.capture.output_path") \
\
  X(PLAN_FRAME_IDX,          0x0060, "core.plan.frame_idx")    \
  X(PLAN_DONE,               0x0061, "core.plan.done")         \
\
  X(PRED_COUNT,              0x0080, "core.pred.count")        \
  X(PRED_TABLE,              0x0081, "core.pred.table")        \
  X(PRED_BASELINES,          0x0082, "core.pred.baselines")    \
  X(PRED_TOTAL,              0x0083, "core.pred.total_passed")  \
  X(PRED_PASSED,             0x0084, "core.pred.count_passed_at_bp")   \
  X(PRED_ABORT_RUN,          0x0085, "core.pred.abort_run")   \
\
  X(WORKER_ERROR,            0x00A0, "core.output.worker_err")

// Emit KeyId constants + per-module range guards
#define DECL_KEY(NAME, ID, STR) \
	inline constexpr savor::context::key::KeyId NAME = static_cast<savor::context::key::KeyId>(ID); \
	static_assert(NAME >= savor::context::key::CORE_MIN && NAME <= savor::context::key::CORE_MAX, "core key out of range");
	CORE_KEYS(DECL_KEY)
#undef DECL_KEY

// Emit the module table used by the aggregator (names + ids for logging/inspection)
inline constexpr savor::context::key::KeyPair kKeys[] = {
	#define ROW(NAME, ID, STR) savor::context::key::KeyPair{ static_cast<savor::context::key::KeyId>(ID), STR },
	CORE_KEYS(ROW)
	#undef ROW
};
inline constexpr std::size_t kCount = sizeof(kKeys) / sizeof(kKeys[0]);

#undef CORE_KEYS

} // namespace savor::context::key::core
