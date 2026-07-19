#include "PhaseScriptVM.h"
#include "ScriptProgress.h"

#include "../Breakpoints/Predicate.h"
#include "../IPC/Wire.h"
#include "../../Core/Memory/DerivedBase.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../../Core/Memory/Soa/SoaAddrProgram.h"

#include <algorithm>
#include <cstring>

namespace {
    static uint16_t read_u16_le(const char* ptr) {
        return static_cast<uint16_t>(static_cast<unsigned char>(ptr[0]))
            | static_cast<uint16_t>(static_cast<unsigned char>(ptr[1]) << 8);
    }

    static bool record_has_baseline_bp(const std::string& table_blob, const savor::pred::PredicateRecord& record, const uint32_t bp_key) {
        if (record.baseline_bps_offset == 0 || bp_key == 0) {
            return false;
        }
        const auto offset = static_cast<size_t>(record.baseline_bps_offset);
        if (offset + sizeof(uint16_t) > table_blob.size()) {
            return false;
        }
        const char* ptr = table_blob.data() + offset;
        const uint16_t count = read_u16_le(ptr);
        ptr += sizeof(uint16_t);
        if (offset + sizeof(uint16_t) + static_cast<size_t>(count) * sizeof(uint16_t) > table_blob.size()) {
            return false;
        }
        for (uint16_t i = 0; i < count; ++i) {
            if (read_u16_le(ptr + i * sizeof(uint16_t)) == bp_key) {
                return true;
            }
        }
        return false;
    }

    static void append_record_baseline_bps(const std::string& table_blob, const savor::pred::PredicateRecord& record, std::vector<uint32_t>& bp_keys) {
        if (record.baseline_bps_offset == 0) {
            return;
        }
        const auto offset = static_cast<size_t>(record.baseline_bps_offset);
        if (offset + sizeof(uint16_t) > table_blob.size()) {
            return;
        }
        const char* ptr = table_blob.data() + offset;
        const uint16_t count = read_u16_le(ptr);
        ptr += sizeof(uint16_t);
        if (offset + sizeof(uint16_t) + static_cast<size_t>(count) * sizeof(uint16_t) > table_blob.size()) {
            return;
        }
        for (uint16_t i = 0; i < count; ++i) {
            bp_keys.push_back(read_u16_le(ptr + i * sizeof(uint16_t)));
        }
    }

    inline bool read_via_addrprog(savor::DolphinWrapper& host,
        const savor::IDerivedBuffer* derived,
        const std::string& table_and_blob,
        uint32_t prog_off,
        uint8_t width,
        uint64_t& out_bits)
    {
        if (prog_off == 0) return false;

        const uint8_t* base = reinterpret_cast<const uint8_t*>(table_and_blob.data());
        const size_t   sz = table_and_blob.size();
        return addrprog::read_value(base, sz, prog_off, host, derived, width, out_bits);
    }
} // namespace

namespace savor {
    bool PhaseScriptVM::op_arm_bps_from_pred_table(PSResult& result, PSContext& ctx) {
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        if (itN == ctx.end() || itT == ctx.end()) return true;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        if (!n || !tbl) return true;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        std::vector<uint32_t> bp_keys; bp_keys.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            if (rec[i].required_bp != 0) {
                bp_keys.push_back(rec[i].required_bp);
            }
            append_record_baseline_bps(*tbl, rec[i], bp_keys);
        }
        std::sort(bp_keys.begin(), bp_keys.end());
        bp_keys.erase(std::unique(bp_keys.begin(), bp_keys.end()), bp_keys.end());

        for (const auto bp_key : bp_keys) {
            const auto* entry = bpmap_.find(static_cast<BPKey>(bp_key));
            if (entry == nullptr || entry->visibility != BreakpointVisibility::PlayerVisible) {
                SCLOGE("[VM] predicate table references an unavailable breakpoint");
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                ctx[savor::context::key::core::PRED_ABORT_RUN] = uint32_t{ 1 };
                result.ctx = ctx;
                return false;
            }
        }

        std::vector<uint32_t> pcs; pcs.reserve(bp_keys.size());
        for (const auto bp_key : bp_keys) {
            const BPAddr* e = bpmap_.find(static_cast<BPKey>(bp_key));
            if (!e || !e->pc) {
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                result.ctx = ctx;
                return false;
            }
            pcs.push_back(e->pc); predicate_bp_keys_.push_back(e->key);
        }
        if (!pcs.empty()) host_.armPcBreakpoints(pcs);
        return true;
    }
    void PhaseScriptVM::op_capture_pred_baselines(PSContext& ctx, KeyHostRouter& router) {
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        auto itB = ctx.find(savor::context::key::core::PRED_BASELINES);
        auto itHit = ctx.find(savor::context::key::core::RUN_HIT_BP_KEY);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        auto* bas = std::get_if<std::string>(&itB->second);
        const uint32_t hit = std::get<uint32_t>(itHit->second);
        if (!n || !tbl || !bas || !hit) return;
        using savor::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::RhsIsDelta) || !r.has_flag(PredFlag::Active) || !record_has_baseline_bp(*tbl, r, hit)) continue;
            uint64_t vbits = 0;
            if (r.lhs_addrprog_offset && read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, vbits)) {}
            else if (r.lhs_addr_key) { if (!router.read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, vbits)) continue; }
            else {
                switch (r.width) {
                case 1: { uint8_t v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; vbits = v; break; }
                case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; vbits = v; break; }
                case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; vbits = v; break; }
                case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; vbits = v; break; }
                default: continue;
                }
            }
            std::memcpy(bas->data() + i * sizeof(uint64_t), &vbits, sizeof(uint64_t));
        }
    }
    void PhaseScriptVM::op_eval_predicates_at_hit_bp(PSContext& ctx, KeyHostRouter& router) {
        uint32_t total = 0; ctx.get(savor::context::key::core::PRED_TOTAL, total);
        uint32_t pass = 0; ctx.get(savor::context::key::core::PRED_PASSED, pass);
        auto itN = ctx.find(savor::context::key::core::PRED_COUNT);
        auto itT = ctx.find(savor::context::key::core::PRED_TABLE);
        auto itB = ctx.find(savor::context::key::core::PRED_BASELINES);
        auto itHit = ctx.find(savor::context::key::core::RUN_HIT_BP_KEY);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        const auto* bas = std::get_if<std::string>(&itB->second);
        const uint32_t hit = std::get<uint32_t>(itHit->second);
        if (!n || !tbl || !bas || !hit) return;
        using savor::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        const uint8_t* bas_ptr = reinterpret_cast<const uint8_t*>(bas->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::Active) || (r.required_bp && r.required_bp != hit)) continue;
            uint64_t lhs = 0, rhs = 0;
            if (r.has_flag(PredFlag::LhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, lhs)) {}
            else if (r.has_flag(PredFlag::LhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, lhs)) continue; }
            else { switch (r.width) { case 1: { uint8_t v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; lhs = v; break; } case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; lhs = v; break; } case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; lhs = v; break; } case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; lhs = v; break; } default: continue; } }
            if (r.has_flag(PredFlag::RhsIsDelta)) { uint64_t cap = 0; std::memcpy(&cap, bas_ptr + i * sizeof(uint64_t), sizeof(uint64_t)); rhs = cap; }
            else if (r.has_flag(PredFlag::RhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.rhs_addrprog_offset, r.width, rhs)) {}
            else if (r.has_flag(PredFlag::RhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.rhs_addr_key), r.width, rhs)) continue; }
            else rhs = r.rhs_imm;
            bool ok = false;
            switch (r.cmp) { case 0: ok = (lhs == rhs); break; case 1: ok = (lhs != rhs); break; case 2: ok = (lhs < rhs); break; case 3: ok = (lhs <= rhs); break; case 4: ok = (lhs > rhs); break; case 5: ok = (lhs >= rhs); break; default: ok = false; break; }
            std::string cmp_string = std::to_string(lhs) + " " + pred::get_cmp_string((pred::CmpOp)r.cmp) + " " + std::to_string(rhs);
            uint32_t progress;
            if (ctx.get(savor::context::key::core::PROGRESS_CORE_FLAGS, progress)
                && (progress & (uint32_t)CoreProgressFlags::PredicateProgress) != 0) {
                std::string msg = std::format("{} - {}", r.name, cmp_string);
                msg = std::string("Pred") + (ok ? "(Passed): " : "(Failed): ") + msg;
                host_.emitProgress(msg, true);
            }
            ++total; if (ok) ++pass;
            if (!ok && r.has_flag(pred::PredFlag::AbortOnFail)) { ctx[savor::context::key::core::PRED_ABORT_RUN] = (uint32_t)1; break; }
        }
        ctx[savor::context::key::core::PRED_PASSED] = pass;
        ctx[savor::context::key::core::PRED_TOTAL] = total;
    }

} // namespace savor
