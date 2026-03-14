#include "BattleRunnerPayload.h"

#include <cstring>

#include "../../../Runner/IPC/Wire.h"
#include "../../../Runner/Script/KeyRegistry.h"
#include "../../../Core/Input/SoaBattle/ActionPlanSerializer.h"
#include "../../../Runner/Script/ScriptProgress.h"

namespace phase::battle::runner {
    static inline void put_u32(std::vector<uint8_t>& b, uint32_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v >> 16)); b.push_back(uint8_t(v >> 24)); }
    static inline bool get_u32(const uint8_t*& p, const uint8_t* e, uint32_t& v) { if (p + 4 > e) return false; v = (uint32_t)p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); p += 4; return true; }

    static constexpr int VERSION = 3;

    bool encode_payload(const EncodeSpec& spec, std::vector<uint8_t>& out)
    {
        out.clear();
        out.push_back(PK_BattleTurnRunner);
        put_u32(out, VERSION);
        put_u32(out, spec.run_ms);
        put_u32(out, spec.vi_stall_ms);

        const auto* f = reinterpret_cast<const uint8_t*>(&spec.initial);
        out.insert(out.end(), f, f + sizeof(GCInputFrame));

        // build predicate table (records + blob)
        std::vector<pred::PredicateRecord> records;
        std::vector<uint8_t> blob;
        simcore::pred::BuildTable(spec.predicates, records, blob);

        const uint32_t np = (uint32_t)records.size();
        put_u32(out, np);
        if (np) {
            const auto* p = reinterpret_cast<const uint8_t*>(records.data());
            out.insert(out.end(), p, p + np * sizeof(pred::PredicateRecord));
        }

        const uint32_t blob_sz = (uint32_t)blob.size();
        put_u32(out, blob_sz);
        if (blob_sz) out.insert(out.end(), blob.begin(), blob.end());

        std::vector<std::uint8_t> plans;
        soa::battle::actions::encode_battle_plan_to_buffer(spec.path, plans);
        const uint32_t nt = (uint32_t)plans.size();
        put_u32(out, nt);
        if (nt) out.insert(out.end(), plans.begin(), plans.end());

        return true;
    }

    bool decode_payload(const std::vector<uint8_t>& in, PSContext& out_ctx)
    {
        if (in.size() < 1 + 4 + 4 + 4 + sizeof(GCInputFrame) + 4) return false;
        const uint8_t* p = in.data();
        const uint8_t* e = p + in.size();
        const uint8_t tag = *p++; if (tag != PK_BattleTurnRunner) return false;

        uint32_t version = 0, run_ms = 0, vi_stall_ms = 0;
        if (!get_u32(p, e, version)) return false;
        if (version < 2 || version > 3) return false; // accept v2 (no blob) and v3 (with blob)
        if (!get_u32(p, e, run_ms)) return false;
        if (!get_u32(p, e, vi_stall_ms)) return false;

        if (p + sizeof(GCInputFrame) > e) return false;
        GCInputFrame initial{}; std::memcpy(&initial, p, sizeof(GCInputFrame)); p += sizeof(GCInputFrame);

        uint32_t pred_count = 0; if (!get_u32(p, e, pred_count)) return false;
        std::string pred_table; std::string pred_bases;
        if (pred_count) {
            const size_t bytes = size_t(pred_count) * sizeof(pred::PredicateRecord);
            if (p + bytes > e) return false;
            pred_table.append(reinterpret_cast<const char*>(p), bytes);
            pred_bases.resize(pred_count * sizeof(uint64_t), 0);
            p += bytes;
        }

        // v3: read blob and append behind table
        if (version >= 3) {
            uint32_t blob_sz = 0; if (!get_u32(p, e, blob_sz)) return false;
            if (blob_sz) {
                if (p + blob_sz > e) return false;
                pred_table.append(reinterpret_cast<const char*>(p), blob_sz);
                p += blob_sz;
            }
        }

        uint32_t battle_plan_buf_size = 0; if (!get_u32(p, e, battle_plan_buf_size)) return false;
        soa::battle::actions::BattlePath b_path;
        soa::battle::actions::decode_battle_plan_from_buffer(std::span<const uint8_t>(p, p + battle_plan_buf_size), b_path);
        p += battle_plan_buf_size;

        out_ctx[keys::core::RUN_MS] = run_ms;
        out_ctx[keys::core::VI_STALL_MS] = vi_stall_ms;

        uint32_t n_plans = (uint32_t)b_path.size();
        out_ctx[keys::battle::INITIAL_INPUT] = initial;
        out_ctx[keys::battle::NUM_TURN_PLANS] = (uint32_t)0;
        out_ctx[keys::battle::LAST_TURN] = n_plans;

        out_ctx[keys::core::PRED_COUNT] = pred_count;
        out_ctx[keys::core::PRED_TABLE] = pred_table;   // now [records || blob]
        out_ctx[keys::core::PRED_BASELINES] = pred_bases;
        out_ctx[keys::core::PRED_PASSED] = (uint32_t) 0;
        out_ctx[keys::core::PRED_TOTAL] = (uint32_t) 0;
        out_ctx[keys::core::PRED_ABORT_RUN] = (uint32_t) 0;

        out_ctx[keys::battle::TURN_PLANS] = b_path;

        simcore::progress::ProgressDeets progress{ .poll_rate = 5000 };
        progress.set_flag(CoreProgressFlags::BattleProgress);
        progress.set_flag(CoreProgressFlags::PredicateProgress);
        progress.set_flag(CoreProgressFlags::DontRecordHeartbeat);
        
        out_ctx[simcore::keys::core::PROGRESS_RATE] = progress.poll_rate;
        out_ctx[simcore::keys::core::PROGRESS_CORE_FLAGS] = progress.flags;
        return true;
    }

} // namespace simcore::battle
