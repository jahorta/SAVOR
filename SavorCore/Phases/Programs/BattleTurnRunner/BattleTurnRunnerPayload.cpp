#include "BattleTurnRunnerPayload.h"

#include <cstring>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/CtxRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"
#include "../../../Core/Input/SoaBattle/BattleCommandCodec.h"

using savor::GCInputFrame;

namespace phase::battle::turnrunner {

    static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v));
        b.push_back(uint8_t(v >> 8));
        b.push_back(uint8_t(v >> 16));
        b.push_back(uint8_t(v >> 24));
    }
    static inline bool get_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) {
        if (p + 4 > e) return false;
        v = (uint32_t)p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
        p += 4;
        return true;
    }
    static inline void put_str(std::vector<uint8_t>& b, const std::string& s) {
        put_u32(b, (uint32_t)s.size());
        b.insert(b.end(), s.begin(), s.end());
    }
    static inline bool get_str(const uint8_t*& p, const uint8_t* e, std::string& out) {
        uint32_t n = 0;
        if (!get_u32(p, e, n)) return false;
        if (p + n > e) return false;
        out.assign(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + n));
        p += n;
        return true;
    }

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
    {
        out.clear();
        out.push_back(savor::PK_BattleSingleTurnRunner);
        put_u32(out, PayloadVersion);
        put_u32(out, spec.run_ms);
        put_u32(out, spec.vi_stall_ms);
        put_u32(out, spec.current_turn);
        put_u32(out, spec.max_turn);
        put_u32(out, spec.has_initial_input ? 1u : 0u);

        const auto* f = reinterpret_cast<const uint8_t*>(&spec.initial);
        out.insert(out.end(), f, f + sizeof(GCInputFrame));

        // Encode single resolved command set directly; fake attacks are separate metadata.
        put_u32(out, spec.turn_plan.fake_attack_count);
        put_u32(out, (uint32_t)spec.turn_plan.commands.size());
        for (const auto& command : spec.turn_plan.commands) {
            soa::battle::actions::BattleCommand::to_wire(command, out);
        }

        // bookkeeping metadata
        put_u32(out, spec.fake_attack_budget_max);
        put_u32(out, spec.fake_attacks_used_before_turn);
        put_str(out, spec.output_savestate_path);

        std::vector<savor::pred::PredicateRecord> records;
        std::vector<uint8_t> blob;
        savor::pred::BuildTable(spec.predicates, records, blob);

        const uint32_t np = (uint32_t)records.size();
        put_u32(out, np);
        if (np) {
            const auto* p = reinterpret_cast<const uint8_t*>(records.data());
            out.insert(out.end(), p, p + np * sizeof(savor::pred::PredicateRecord));
        }

        const uint32_t blob_sz = (uint32_t)blob.size();
        put_u32(out, blob_sz);
        if (blob_sz) out.insert(out.end(), blob.begin(), blob.end());

        put_str(out, spec.capture_profile_path);
        put_str(out, spec.capture_output_path);
        put_u32(out, spec.override_start_rng_seed.has_value() ? 1u : 0u);
        put_u32(out, spec.override_start_rng_seed.value_or(0u));

        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, savor::PSContext& out_ctx)
    {
        if (in.size() < 1 + 4 + 4 + 4 + 4 + 4 + sizeof(GCInputFrame) + 4 + 4 + 4 + 4 + 4 + 4) return false;

        const uint8_t* p = in.data();
        const uint8_t* e = p + in.size();

        const uint8_t tag = *p++;
        if (tag != savor::PK_BattleSingleTurnRunner) return false;

        uint32_t version = 0;
        uint32_t run_ms = 0, vi_stall_ms = 0;
        uint32_t current_turn = 1;
        uint32_t max_turn = 1;
        uint32_t has_initial_input = 0;

        if (!get_u32(p, e, version)) return false;
        if (version < 3 || version > PayloadVersion) return false;
        if (!get_u32(p, e, run_ms)) return false;
        if (!get_u32(p, e, vi_stall_ms)) return false;
        if (!get_u32(p, e, current_turn)) return false;
        if (!get_u32(p, e, max_turn)) return false;
        if (!get_u32(p, e, has_initial_input)) return false;

        if (p + sizeof(GCInputFrame) > e) return false;
        GCInputFrame initial{};
        std::memcpy(&initial, p, sizeof(GCInputFrame));
        p += sizeof(GCInputFrame);

        uint32_t fake_attack_count = 0;
        uint32_t action_count = 0;
        if (!get_u32(p, e, fake_attack_count)) return false;
        if (!get_u32(p, e, action_count)) return false;

        soa::battle::actions::TurnPlan turn;
        turn.fake_attack_count = fake_attack_count;
        turn.commands.reserve(action_count);
        for (uint32_t i = 0; i < action_count; ++i) {
            soa::battle::actions::BattleCommand command{};
            if (!soa::battle::actions::BattleCommand::from_wire(p, e, command)) return false;
            turn.commands.push_back(command);
        }

        uint32_t budget_max = 0;
        uint32_t used_before = 0;
        if (!get_u32(p, e, budget_max)) return false;
        if (!get_u32(p, e, used_before)) return false;
        std::string output_savestate_path;
        if (!get_str(p, e, output_savestate_path)) return false;

        uint32_t pred_count = 0;
        if (!get_u32(p, e, pred_count)) return false;

        std::string pred_table;
        std::string pred_bases;
        if (pred_count) {
            const size_t bytes = size_t(pred_count) * sizeof(savor::pred::PredicateRecord);
            if (p + bytes > e) return false;
            pred_table.append(reinterpret_cast<const char*>(p), bytes);
            pred_bases.resize(pred_count * sizeof(uint64_t), 0);
            p += bytes;
        }

        uint32_t blob_sz = 0;
        if (!get_u32(p, e, blob_sz)) return false;
        if (blob_sz) {
            if (p + blob_sz > e) return false;
            pred_table.append(reinterpret_cast<const char*>(p), blob_sz);
            p += blob_sz;
        }

        std::string capture_profile_path;
        std::string capture_output_path;
        if (version >= 4) {
            if (!get_str(p, e, capture_profile_path)) return false;
            if (!get_str(p, e, capture_output_path)) return false;
        }
        uint32_t override_start_rng_enabled = 0;
        uint32_t override_start_rng_seed = 0;
        if (version >= 5) {
            if (!get_u32(p, e, override_start_rng_enabled)) return false;
            if (!get_u32(p, e, override_start_rng_seed)) return false;
        }

        if (p != e) return false;

        soa::battle::actions::BattlePath path;
        path.push_back(std::move(turn));

        out_ctx[savor::context::key::core::RUN_MS] = run_ms;
        out_ctx[savor::context::key::core::VI_STALL_MS] = vi_stall_ms;

        // Single-turn runner payload carries exactly one local turn plan.
        // Materialization indexes TURN_PLANS with ACTIVE_TURN as a 1-based index,
        // so keep ACTIVE_TURN local (=1) while preserving the caller-provided
        // global turn number in TURN_OUTPUT_INDEX for bookkeeping/output.
        out_ctx[savor::context::key::battle::ACTIVE_TURN] = (uint32_t)1;
        out_ctx[savor::context::key::battle::TURN_INPUT_INDEX] = (uint32_t)1;
        out_ctx[savor::context::key::battle::TURN_OUTPUT_INDEX] = current_turn;
        out_ctx[savor::context::key::battle::HAS_INITIAL_INPUT] = has_initial_input ? 1u : 0u;
        out_ctx[savor::context::key::battle::INITIAL_INPUT] = initial;

        out_ctx[savor::context::key::battle::TURN_PLANS] = path;
        out_ctx[savor::context::key::battle::LAST_TURN] = max_turn;

        out_ctx[savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN] = fake_attack_count;
        out_ctx[savor::context::key::battle::FAKE_ATTACK_BUDGET_MAX] = budget_max;
        out_ctx[savor::context::key::battle::FAKE_ATTACK_USED_BEFORE] = used_before;
        out_ctx[savor::context::key::battle::OUTPUT_SAVESTATE_PATH] = output_savestate_path;
        out_ctx[savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES] = (uint32_t)3;
        out_ctx[savor::context::key::battle::MACRO_RESULT] = (uint32_t)1;
        out_ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_STEP_COUNT] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE] = (uint32_t)1;
        out_ctx[savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES] = (uint32_t)7;
        out_ctx[savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS] = (uint32_t)1000;
        out_ctx[savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS] = (uint32_t)1;
        out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE] = (uint32_t)1;
        out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS] = (uint32_t)1000;
        out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = (uint32_t)0;
        out_ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = (uint32_t)0;
        out_ctx[savor::context::key::battle::RNG_OVERRIDE_ENABLED] = override_start_rng_enabled ? 1u : 0u;
        out_ctx[savor::context::key::battle::RNG_OVERRIDE_SEED] = override_start_rng_seed;
        out_ctx[savor::context::key::battle::RNG_ORIGINAL_SEED] = (uint32_t)0;
        out_ctx[savor::context::key::battle::RNG_APPLIED_SEED] = (uint32_t)0;
        out_ctx[savor::context::key::core::RUN_POLL_MS] = (uint32_t)10;

        out_ctx[savor::context::key::core::PRED_COUNT] = pred_count;
        out_ctx[savor::context::key::core::PRED_TABLE] = pred_table;
        out_ctx[savor::context::key::core::PRED_BASELINES] = pred_bases;
        out_ctx[savor::context::key::core::PRED_PASSED] = (uint32_t)0;
        out_ctx[savor::context::key::core::PRED_TOTAL] = (uint32_t)0;
        out_ctx[savor::context::key::core::PRED_ABORT_RUN] = (uint32_t)0;

        if (!capture_profile_path.empty() || !capture_output_path.empty()) {
            out_ctx[savor::context::key::core::CAPTURE_PROFILE_PATH] = capture_profile_path;
            out_ctx[savor::context::key::core::CAPTURE_OUTPUT_PATH] = capture_output_path;
        }

        savor::progress::ProgressDeets progress{ .poll_rate = 5000 };
        progress.set_flag(CoreProgressFlags::BattleProgress);
        progress.set_flag(CoreProgressFlags::PredicateProgress);
        progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);

        out_ctx[savor::context::key::core::PROGRESS_RATE] = progress.poll_rate;
        out_ctx[savor::context::key::core::PROGRESS_CORE_FLAGS] = progress.flags;
        return true;
    }

} // namespace phase::battle::turnrunner
