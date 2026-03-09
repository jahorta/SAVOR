#include "BattleTurnRunnerPayload.h"

#include <cstring>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/KeyRegistry.h"
#include "../../../Runner/Script/ScriptProgress.h"

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
        out.push_back(PK_BattleSingleTurnRunner);
        put_u32(out, PayloadVersion);
        put_u32(out, spec.run_ms);
        put_u32(out, spec.vi_stall_ms);
        put_u32(out, spec.current_turn);
        put_u32(out, spec.has_initial_input ? 1u : 0u);

        const auto* f = reinterpret_cast<const uint8_t*>(&spec.initial);
        out.insert(out.end(), f, f + sizeof(GCInputFrame));

        // Encode single TurnPlan directly
        put_u32(out, spec.turn_plan.fake_attack_count);
        put_u32(out, (uint32_t)spec.turn_plan.spec.size());
        for (const auto& ap : spec.turn_plan.spec) {
            soa::battle::actions::ActionPlan::to_wire(ap, out);
        }

        // bookkeeping metadata
        put_u32(out, spec.fake_attack_budget_max);
        put_u32(out, spec.fake_attacks_used_before_turn);
        put_str(out, spec.output_savestate_path);

        std::vector<simcore::pred::PredicateRecord> records;
        std::vector<uint8_t> blob;
        simcore::pred::BuildTable(spec.predicates, records, blob);

        const uint32_t np = (uint32_t)records.size();
        put_u32(out, np);
        if (np) {
            const auto* p = reinterpret_cast<const uint8_t*>(records.data());
            out.insert(out.end(), p, p + np * sizeof(simcore::pred::PredicateRecord));
        }

        const uint32_t blob_sz = (uint32_t)blob.size();
        put_u32(out, blob_sz);
        if (blob_sz) out.insert(out.end(), blob.begin(), blob.end());

        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, simcore::PSContext& out_ctx)
    {
        if (in.size() < 1 + 4 + 4 + 4 + 4 + 4 + sizeof(GCInputFrame) + 4 + 4 + 4 + 4 + 4 + 4) return false;

        const uint8_t* p = in.data();
        const uint8_t* e = p + in.size();

        const uint8_t tag = *p++;
        if (tag != PK_BattleSingleTurnRunner) return false;

        uint32_t version = 0;
        uint32_t run_ms = 0, vi_stall_ms = 0;
        uint32_t current_turn = 1;
        uint32_t has_initial_input = 0;

        if (!get_u32(p, e, version)) return false;
        if (version != PayloadVersion) return false;
        if (!get_u32(p, e, run_ms)) return false;
        if (!get_u32(p, e, vi_stall_ms)) return false;
        if (!get_u32(p, e, current_turn)) return false;
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
        turn.spec.reserve(action_count);
        for (uint32_t i = 0; i < action_count; ++i) {
            soa::battle::actions::ActionPlan ap{};
            if (!soa::battle::actions::ActionPlan::from_wire(p, e, ap)) return false;
            turn.spec.push_back(ap);
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
            const size_t bytes = size_t(pred_count) * sizeof(simcore::pred::PredicateRecord);
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

        if (p != e) return false;

        soa::battle::actions::BattlePath path;
        path.push_back(std::move(turn));

        out_ctx[simcore::keys::core::RUN_MS] = run_ms;
        out_ctx[simcore::keys::core::VI_STALL_MS] = vi_stall_ms;

        out_ctx[simcore::keys::battle::ACTIVE_TURN] = (uint32_t)1;
        out_ctx[simcore::keys::battle::TURN_INPUT_INDEX] = current_turn;
        out_ctx[simcore::keys::battle::TURN_OUTPUT_INDEX] = current_turn;
        out_ctx[simcore::keys::battle::HAS_INITIAL_INPUT] = has_initial_input ? 1u : 0u;
        out_ctx[simcore::keys::battle::INITIAL_INPUT] = initial;

        out_ctx[simcore::keys::battle::TURN_PLANS] = path;
        out_ctx[simcore::keys::battle::LAST_TURN] = (uint32_t)1;

        out_ctx[simcore::keys::battle::FAKE_ATTACK_COUNT_THIS_TURN] = fake_attack_count;
        out_ctx[simcore::keys::battle::FAKE_ATTACK_BUDGET_MAX] = budget_max;
        out_ctx[simcore::keys::battle::FAKE_ATTACK_USED_BEFORE] = used_before;
        out_ctx[simcore::keys::battle::OUTPUT_SAVESTATE_PATH] = output_savestate_path;

        out_ctx[simcore::keys::core::PRED_COUNT] = pred_count;
        out_ctx[simcore::keys::core::PRED_TABLE] = pred_table;
        out_ctx[simcore::keys::core::PRED_BASELINES] = pred_bases;
        out_ctx[simcore::keys::core::PRED_PASSED] = (uint32_t)0;
        out_ctx[simcore::keys::core::PRED_TOTAL] = (uint32_t)0;
        out_ctx[simcore::keys::core::PRED_ABORT_RUN] = (uint32_t)0;

        simcore::progress::ProgressDeets progress{ .poll_rate = 5000 };
        progress.set_flag(simcore::CoreProgressFlags::BattleProgress);
        progress.set_flag(simcore::CoreProgressFlags::PredicateProgress);
        progress.set_flag(simcore::CoreProgressFlags::DontRecordHeartbeat);

        out_ctx[simcore::keys::core::PROGRESS_RATE] = progress.poll_rate;
        out_ctx[simcore::keys::core::PROGRESS_CORE_FLAGS] = progress.flags;
        return true;
    }

} // namespace phase::battle::turnrunner
