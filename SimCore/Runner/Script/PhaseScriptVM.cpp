#include "PhaseScriptVM.h"
#include <algorithm>

#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/SoaAddrProgram.h"
#include "../../Core/Memory/Soa/SoaAddrRegistry.h"
#include "../Breakpoints/Predicate.h"
#include "../../Core/Input/SoaBattle/PlanWriter.h"
#include "../../Core/Input/SoaBattle/ActionLibrary.h"
#include "../../Core/Input/InputPlanFmt.h"
#include "../../Core/Memory/IKeyReader.h"
#include "../../Core/Memory/DerivedBase.h"
#include "../../Core/Memory/Soa/Battle/DerivedBattleBuffer.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../Breakpoints/BPRegistry.h"

namespace {
    inline bool read_via_addrprog(simcore::DolphinWrapper& host,
        const simcore::IDerivedBuffer* derived,
        const std::string& table_and_blob,
        uint32_t prog_off,
        uint8_t width,
        uint64_t& out_bits)
    {
        if (prog_off == 0) return false;

        const uint8_t* base = reinterpret_cast<const uint8_t*>(table_and_blob.data());
        const size_t   sz = table_and_blob.size();
        if (prog_off + 3 > sz) return false;

        const uint8_t* p = base + prog_off;
        uint8_t op = *p++;
        if (op != 0x01) return false; // OP_BASE_KEY
        uint16_t key = uint16_t(p[0]) | (uint16_t(p[1]) << 8);

        const auto region = addr::Registry::region(static_cast<addr::AddrKey>(key)); // MEM1/MEM2/DERIVED
        const auto res = addrprog::exec(base, sz, prog_off, host, derived);        
        if (!res.ok) return false;

        switch (region) {
        case addr::Region::MEM1:
        case addr::Region::MEM2:
            switch (width) {
            case 1: { uint8_t  v = 0; if (!host.readU8(res.va, v))  return false; out_bits = v; return true; }
            case 2: { uint16_t v = 0; if (!host.readU16(res.va, v)) return false; out_bits = v; return true; }
            case 4: { uint32_t v = 0; if (!host.readU32(res.va, v)) return false; out_bits = v; return true; }
            case 8: { uint64_t v = 0; if (!host.readU64(res.va, v)) return false; out_bits = v; return true; }
            default: return false;
            }
        case addr::Region::DERIVED:
            if (!derived) return false;
            return derived->read_raw(res.va, width, out_bits);
        default:
            return false;
        }
    }
}

namespace simcore {

    PhaseScriptVM::PhaseScriptVM(simcore::DolphinWrapper& host, const BreakpointMap& bpmap)
        : host_(host), bpmap_(bpmap) {
    }

    bool PhaseScriptVM::save_snapshot() {
        return host_.saveStateToBuffer(snapshot_);
    }

    bool PhaseScriptVM::load_snapshot() {
        return host_.loadStateFromBuffer(snapshot_);
    }

    void PhaseScriptVM::arm_bps_once() {
        if (armed_) return;
        std::vector<uint32_t> pcs;
        pcs.reserve(canonical_bp_keys_.size());
        for (const auto& k : canonical_bp_keys_) {
            if (const auto* e = bpmap_.find(k)) pcs.push_back(e->pc);
        }
        if (!pcs.empty()) {
            host_.armPcBreakpoints(pcs);
            armed_pcs_ = pcs;
        }
        armed_ = true;
    }

    bool PhaseScriptVM::init(const PSInit& init, const PhaseScript& program)
    {
        init_ = init;
        prog_ = program;

        switch (init_.derived_buffer_type) {
        case DK_Battle: derived_ = std::make_unique<simcore::DerivedBattleBuffer>(); break;
            // case 2: derived_ = std::make_unique<simcore::DerivedExploreBuffer>(); break; // future
        default: derived_.reset(); break;
        }

        SCLOGD("[VM] init begin sav=%s timeout=%u", init.savestate_path.c_str(), init.default_timeout_ms);

        // Disarm any previously armed set (enables program swapping)
        if (armed_ && !armed_pcs_.empty()) {
            host_.disarmPcBreakpoints(armed_pcs_);
            armed_pcs_.clear();
            armed_ = false;
        }

        // Optional savestate (allow empty path for boot-based phases)
        if (!init_.savestate_path.empty()) {
            if (!host_.loadSavestate(init_.savestate_path.c_str()))
                return false;
        }

        // Update canonical BP keys and arm once
        canonical_bp_keys_ = prog_.canonical_bp_keys;

        SCLOGD("[VM] attach bp count=%zu", program.canonical_bp_keys.size());
        arm_bps_once();

        // Capture a snapshot to use as the per-job baseline
        const bool snapshot_ok = save_snapshot();
        if (snapshot_ok) SCLOGD("[VM] init ok");
        return snapshot_ok;
    }

    PSResult PhaseScriptVM::run(const PSJob& job)
    {
        PSResult R{};
        PSContext ctx = job.ctx;
        predicate_bp_keys_.clear();
        std::string _section = "Entry Point";

        ctx[keys::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);

        // Always start by restoring the pre-captured snapshot for each job
        if (!load_snapshot()) return R;


        if (derived_) derived_->on_init(ctx);
        DolphinKeyReader     mem1_reader(&host_);
        std::unique_ptr<KeyHostRouter> router;
        if (derived_) router = std::make_unique<KeyHostRouter>(&mem1_reader, derived_.get());
        else router = std::make_unique<KeyHostRouter>(&mem1_reader, nullptr);

        std::unordered_map<std::string, size_t> label_vm_pc_map;
        for (size_t i = 0; i < prog_.ops.size(); ++i) {
            if (prog_.ops[i].code == PSOpCode::LABEL) label_vm_pc_map[prog_.ops[i].label.name] = i;
        }

        for (size_t vm_pc = 0; vm_pc < prog_.ops.size(); ++vm_pc) {
            const auto& op = prog_.ops[vm_pc];
            SCLOGT("[VM] running op: %s", get_psop_name(op.code).c_str());

            switch (op.code) {
            case PSOpCode::ARM_PHASE_BPS_ONCE: 
            { arm_bps_once(); break; }

            case PSOpCode::LOAD_SNAPSHOT: 
            { if (!load_snapshot()) return R; break; }

            case PSOpCode::CAPTURE_SNAPSHOT: 
            { if (!save_snapshot()) return R; break; }

            case PSOpCode::LABEL: 
            { break; }

            case PSOpCode::GOTO: {
                auto it = label_vm_pc_map.find(op.jmp.name);
                if (it != label_vm_pc_map.end()) 
                {
                    _section = op.jmp.name;
                    vm_pc = it->second;
                }
                break;
            }

            case PSOpCode::GOTO_IF: {
                uint32_t lv = 0;
                ctx.get(op.jcc.key, lv);
                auto rv = op.jcc.imm;
                auto cmp = op.jcc.cmp;
                std::string name = op.jcc.name;
                bool take = false;
                switch (cmp) {
                case PSCmp::EQ: take = (lv == rv); break;
                case PSCmp::NE: take = (lv != rv); break;
                case PSCmp::LT: take = (lv < rv); break;
                case PSCmp::LE: take = (lv <= rv); break;
                case PSCmp::GT: take = (lv > rv); break;
                case PSCmp::GE: take = (lv >= rv); break;
                }
                if (take) {
                    auto it = label_vm_pc_map.find(name);
                    if (it != label_vm_pc_map.end()) 
                    {
                        _section = name;
                        vm_pc = it->second;
                    }
                }
                break;
            }

            case PSOpCode::GOTO_IF_KEYS: {
                uint32_t lv = 0, rv = 0;
                ctx.get(op.jcc2.left, lv);
                ctx.get(op.jcc2.right, rv);
                auto cmp = op.jcc2.cmp;
                std::string name = op.jcc2.name;
                bool take = false;
                switch (cmp) {
                case PSCmp::EQ: take = (lv == rv); break;
                case PSCmp::NE: take = (lv != rv); break;
                case PSCmp::LT: take = (lv < rv); break;
                case PSCmp::LE: take = (lv <= rv); break;
                case PSCmp::GT: take = (lv > rv); break;
                case PSCmp::GE: take = (lv >= rv); break;
                }
                if (take) {
                    auto it = label_vm_pc_map.find(name);
                    if (it != label_vm_pc_map.end()) 
                    {
                        _section = name;
                        vm_pc = it->second;
                    }
                }
                break;
            }

            case PSOpCode::SET_U32: {
                ctx[op.keyimm.key] = op.keyimm.imm;
                break;
            }

            case PSOpCode::ADD_U32: {
                uint32_t v = 0; ctx.get<uint32_t>(op.keyimm.key, v);
                v += op.keyimm.imm;
                ctx[op.keyimm.key] = v;
                break;
            }

            case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH:
            {
                uint32_t turn = 0;
                ctx.get<uint32_t>(keys::battle::ACTIVE_TURN, turn);

                if (turn == 0) {
                    ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::InvalidTurnIdxZero;
                    ctx[keys::core::PLAN_DONE] = (uint32_t)1;
                }

                // Pull typed BattlePath (vector<TurnPlanSpec>)
                soa::battle::actions::BattlePath bp;
                if (!ctx.get<soa::battle::actions::BattlePath>(keys::battle::TURN_PLANS, bp)) {
                    ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::BadBlob;
                    ctx[keys::core::PLAN_DONE] = (uint32_t)1;
                    break;
                }

                if (turn > bp.size()) {
                    ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::OutOfTurns;
                    ctx[keys::core::PLAN_DONE] = (uint32_t)1;
                    break;
                }

                // Get fresh BattleContext (already provided just before this op in your program)
                soa::battle::ctx::BattleContext bc{};
                {
                    std::string blob;
                    if (ctx.get<std::string>(keys::battle::CTX_BLOB, blob)) {
                        soa::battle::ctx::codec::decode(blob, bc);
                    }
                }

                const auto& turn_plan = bp[turn-1];

                simcore::InputPlan plan;
                auto err = soa::battle::actions::MaterializeErr::OK;
                const bool ok = soa::battle::actions::ActionLibrary::generateTurnPlan(bc, turn_plan, plan, err);
                if (!ok) {
                    ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)err;
                    ctx[keys::core::PLAN_DONE] = (uint32_t)1;
                    break;
                }

                const uint32_t n = static_cast<uint32_t>(plan.size());
                std::string counts; counts.resize(sizeof(uint32_t));
                std::memcpy(counts.data(), &n, sizeof(uint32_t));

                std::string frames; frames.resize(n * sizeof(simcore::GCInputFrame));
                if (n) std::memcpy(frames.data(), plan.data(), frames.size());

                ctx[simcore::keys::battle::NUM_TURN_PLANS] = uint32_t(1);
                ctx[simcore::keys::battle::INPUTPLAN_FRAME_COUNT] = counts;
                ctx[simcore::keys::battle::INPUTPLAN] = frames;

                ctx[keys::battle::PLAN_MATERIALIZE_ERR] = uint32_t((uint32_t)soa::battle::actions::MaterializeErr::OK);
                ctx[keys::core::PLAN_DONE] = uint32_t(0);
                break;
            }

            case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES: {
                // tables
                auto itC = ctx.find(keys::battle::INPUTPLAN_FRAME_COUNT);
                auto itT = ctx.find(keys::battle::INPUTPLAN);
                if (itC == ctx.end() || itT == ctx.end()) { break; }

                const auto* counts_s = std::get_if<std::string>(&itC->second);
                const auto* table_s = std::get_if<std::string>(&itT->second);
                if (!counts_s || !table_s) { break; }

                const uint8_t* counts = (const uint8_t*)counts_s->data();
                const uint8_t* frames = (const uint8_t*)table_s->data();

                if (counts == 0) {
                    ctx[keys::core::PLAN_DONE] = uint32_t(1);
                    break;
                }

                host_.setEnableAllBreakpoints(false);
                
                uint32_t idx;
                const uint32_t count = *(const uint32_t*)(counts);
                for (idx = 0; idx < count; idx++) {

                    GCInputFrame f{};
                    std::memcpy(&f, frames + (idx * sizeof(GCInputFrame)), sizeof(GCInputFrame));

                    SCLOGD("[vm] setting input [%d]: %s", idx, DescribeFrame(f).c_str());
                    host_.setInput(f);
                    host_.stepOneFrameBlocking();
                }
                host_.setEnableAllBreakpoints(true);
                if (idx >= count) ctx[keys::core::PLAN_DONE] = uint32_t(1);

                uint32_t cur_turn_plans = 0;
                auto itTP = ctx.get(keys::battle::NUM_TURN_PLANS, cur_turn_plans);
                ctx[keys::battle::NUM_TURN_PLANS] = cur_turn_plans > 0 ? cur_turn_plans - 1 : 0;
                break;
            }

            case PSOpCode::STEP_FRAMES: {
                SCLOGD("[VM] phase=run_inputs begin frames=%zu", op.step.n);
                for (uint32_t i = 0; i < op.step.n; ++i) host_.stepOneFrameBlocking();
                SCLOGD("[VM] phase=run_inputs end");
                break;
            }

            case PSOpCode::RUN_UNTIL_BP: {
                using simcore::RunToBpOutcome;

                uint32_t timeout_ms = init_.default_timeout_ms;
                ctx.get<uint32_t>(keys::core::RUN_MS, timeout_ms);

                uint32_t vi_stall_ms = 0;
                ctx.get<uint32_t>(keys::core::VI_STALL_MS, vi_stall_ms);

                const uint32_t poll_ms = host_.pickPollIntervalMs(timeout_ms);
                const bool watch_movie = true;

                // progress sink handled inside wrapper; host has per-job sink already

                auto t0 = std::chrono::steady_clock::now();
                auto rr = host_.runUntilBreakpointFlexible(timeout_ms, vi_stall_ms, watch_movie, poll_ms);
                auto t1 = std::chrono::steady_clock::now();
                const uint32_t elapsed_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                RunToBpOutcome outcome = RunToBpOutcome::Unknown;
                if (rr.hit) outcome = RunToBpOutcome::Hit;
                else if (rr.reason) {
                    if (std::strcmp(rr.reason, "timeout") == 0)     outcome = RunToBpOutcome::Timeout;
                    else if (std::strcmp(rr.reason, "vi_stalled") == 0)  outcome = RunToBpOutcome::ViStalled;
                    else if (std::strcmp(rr.reason, "movie_ended") == 0) outcome = RunToBpOutcome::MovieEnded;
                    else outcome = RunToBpOutcome::Unknown;
                }

                // mirror -> ctx
                ctx[keys::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(outcome);
                ctx[keys::core::ELAPSED_MS] = elapsed_ms;
                ctx[keys::core::RUN_HIT_PC] = rr.hit ? (uint32_t)rr.pc : (uint32_t)0u;
                ctx[keys::core::VI_DELTA] = (uint32_t)(host_.getViFieldCountApproxFromBaseline() & 0xFFFFFFFFull);
                ctx[keys::core::POLL_MS] = poll_ms;

                // derive hit BP id by matching PC
                uint32_t hit_bp_key = 0;
                if (rr.hit) {
                    for (auto k : canonical_bp_keys_) {
                        if (const auto* e = bpmap_.find(k); e && e->pc == rr.pc) { hit_bp_key = (uint32_t)e->key; break; }
                    }
                    for (auto k : predicate_bp_keys_) {
                        if (const auto* e = bpmap_.find(k); e && e->pc == rr.pc) { hit_bp_key = (uint32_t)e->key; break; }
                    }
                }
                ctx[keys::core::RUN_HIT_BP_KEY] = hit_bp_key;
                // keep derived buffer in sync for this frame
                if (derived_) derived_->update_on_bp(hit_bp_key, ctx, host_);

                break;
            }

            case PSOpCode::READ_U8: 
            { uint8_t  v{}; if (!read_u8(op.rd.addr, v)) return R; ctx[op.rd.dst] = v; break; }

            case PSOpCode::READ_U16: 
            { uint16_t v{}; if (!read_u16(op.rd.addr, v)) return R; ctx[op.rd.dst] = v; break; }

            case PSOpCode::READ_U32:
            {
                uint32_t v{};
                if (!read_u32(op.rd.addr, v))
                {
                    SCLOGD("[VM] READ_U32 FAIL @%08X key=%s", op.rd.addr, keys::name_for_id(op.rd.dst).data());
                    return R;
                }
                SCLOGD("[VM] READ_U32 @%08X -> %08X key=%s", op.rd.addr, v, keys::name_for_id(op.rd.dst).data());
                ctx[op.rd.dst] = v;
                break;
            }
            case PSOpCode::READ_F32: 
            { float    v{}; if (!read_f32(op.rd.addr, v)) return R; ctx[op.rd.dst] = v; break; }

            case PSOpCode::READ_F64: 
            { double   v{}; if (!read_f64(op.rd.addr, v)) return R; ctx[op.rd.dst] = v; break; }

            case PSOpCode::GET_BATTLE_CONTEXT:
            {
                std::string mem1;
                if (!host_.getMem1(mem1)) { R.ok = false; break; }
                simcore::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
                soa::battle::ctx::BattleContext bc{};
                if (!soa::battle::ctx::codec::extract_from_mem1(view, bc)) { R.ok = false; break; }
                std::string blob;
                soa::battle::ctx::codec::encode(bc, blob);
                ctx[simcore::keys::battle::CTX_BLOB] = blob;
                break;
            }

            case PSOpCode::EMIT_RESULT:
            {
                SCLOGD("[VM] EMIT_RESULT %s=%08X", keys::name_for_id(op.key.id).data(), ctx[op.key.id]);
                R.ctx[op.key.id] = ctx[op.key.id]; // copy selected value to result
                break;
            }

            case PSOpCode::RETURN_RESULT: {
                ctx[keys::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
                R.ctx = ctx;
                R.ctx[op.keyimm.key] = op.keyimm.imm;
                uint32_t dw_outcome = 0; ctx.get(keys::core::DW_RUN_OUTCOME_CODE, dw_outcome);
                R.ok = dw_outcome == 0;
                return R;
            }

            case PSOpCode::APPLY_INPUT_FROM: {
                auto it = ctx.find(op.key.id);
                if (it == ctx.end()) return R;
                if (auto p = std::get_if<GCInputFrame>(&it->second)) {
                    host_.setInput(*p);
                }
                else {
                    return R;
                }
                break;
            }

            case PSOpCode::SET_TIMEOUT: 
            { ctx[keys::core::RUN_MS] = op.imm.v; break; }

            case PSOpCode::SET_TIMEOUT_FROM: {
                uint32_t timeout_ms;
                ctx.get<uint32_t>(op.key.id, timeout_ms);
                ctx[keys::core::RUN_MS] = timeout_ms;
                break;
            }

            case PSOpCode::MOVIE_PLAY_FROM: {
                std::string path;
                ctx.get<std::string>(op.key.id, path);
                if (!host_.startMoviePlayback(path)) return R;
                break;
            }

            case PSOpCode::SAVE_SAVESTATE_FROM: {
                std::string path;
                ctx.get<std::string>(op.key.id, path);
                if (!host_.saveSavestateBlocking(path)) return R;
                ctx[keys::core::LAST_SAVESTATE_PATH] = path;    
                break;
            }

            case PSOpCode::REQUIRE_DISC_GAMEID_FROM: {

                const char* id6 = nullptr;
                std::string tmp;
                ctx.get<std::string>(op.key.id, tmp);
                if (tmp.size() < 6) return R;
                id6 = tmp.c_str();

                auto di = host_.getDiscInfo();
                const bool ok = (di.has_value() && di->game_id.size() >= 6 &&
                    std::memcmp(di->game_id.data(), id6, 6) == 0);
                if (!ok) return R;
                break;
            }

            case PSOpCode::ARM_BPS_FROM_PRED_TABLE: {
                auto itN = ctx.find(keys::core::PRED_COUNT);
                auto itT = ctx.find(keys::core::PRED_TABLE);
                if (itN == ctx.end() || itT == ctx.end()) break;

                const uint32_t n = std::get<uint32_t>(itN->second);
                const auto* tbl = std::get_if<std::string>(&itT->second);
                if (!n || !tbl) break;
                
                const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
                std::vector<uint32_t> pcs; pcs.reserve(n);
                for (uint32_t i = 0; i < n; ++i) {
                    const BPAddr* e = bpmap_.find(static_cast<BPKey>(rec[i].required_bp));
                    if (!e || !e->pc) continue;
                    
                    pcs.push_back(e->pc);
                    predicate_bp_keys_.push_back(e->key);
                }
                
                if (!pcs.empty()) host_.armPcBreakpoints(pcs);
                break;
            }

            case PSOpCode::CAPTURE_PRED_BASELINES: {
                auto itN = ctx.find(keys::core::PRED_COUNT);
                auto itT = ctx.find(keys::core::PRED_TABLE);
                auto itB = ctx.find(keys::core::PRED_BASELINES);
                if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end()) break;

                const uint32_t n = std::get<uint32_t>(itN->second);
                const auto* tbl = std::get_if<std::string>(&itT->second);
                auto* bas = std::get_if<std::string>(&itB->second);
                if (!n || !tbl || !bas) break;

                using simcore::pred::PredFlag;
                const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());

                for (uint32_t i = 0; i < n; ++i) {
                    const auto& r = rec[i];
                    if (!r.has_flag(PredFlag::CaptureBaseline)) continue;
                    if (!r.has_flag(PredFlag::Active)) continue;

                    uint64_t vbits = 0;

                    // LHS precedence: addrprog -> key -> absolute
                    if (r.lhs_addrprog_offset &&
                        read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, vbits)) {
                        // ok
                    }
                    else if (r.lhs_addr_key) {
                        if (!router->read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, vbits)) continue;
                    }
                    else {
                        switch (r.width) {
                        case 1: { uint8_t  v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; vbits = v; break; }
                        case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; vbits = v; break; }
                        case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; vbits = v; break; }
                        case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; vbits = v; break; }
                        default: continue;
                        }
                    }

                    std::memcpy(bas->data() + i * sizeof(uint64_t), &vbits, sizeof(uint64_t));
                }
                break;
            }

            case PSOpCode::EVAL_PREDICATES_AT_HIT_BP: {
                uint32_t hit_bp = 0; ctx.get<uint32_t>(keys::core::RUN_HIT_PC, hit_bp);
                uint32_t cur_turn = 0; ctx.get<uint32_t>(keys::battle::ACTIVE_TURN, cur_turn);

                uint32_t total = 0; ctx.get(keys::core::PRED_TOTAL, total);
                uint32_t pass = 0;
                uint32_t all_passed = 1;  ctx.get(keys::core::PRED_ALL_PASSED, all_passed);

                auto itN = ctx.find(keys::core::PRED_COUNT);
                auto itT = ctx.find(keys::core::PRED_TABLE);
                auto itB = ctx.find(keys::core::PRED_BASELINES);
                auto itHit = ctx.find(keys::core::RUN_HIT_BP_KEY);
                if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) break;

                const uint32_t n = std::get<uint32_t>(itN->second);
                const auto* tbl = std::get_if<std::string>(&itT->second);
                const auto* bas = std::get_if<std::string>(&itB->second);
                const uint32_t hit = std::get<uint32_t>(itHit->second);
                if (!n || !tbl || !bas || !hit) break;

                using simcore::pred::PredFlag;

                const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
                const uint8_t* bas_ptr = reinterpret_cast<const uint8_t*>(bas->data());

                for (uint32_t i = 0; i < n; ++i) {
                    const auto& r = rec[i];
                    if (!r.has_flag(PredFlag::Active)) continue;
                    if (r.required_bp && r.required_bp != hit) continue;

                    uint64_t lhs = 0, rhs = 0;

                    // LHS precedence: addrprog -> key -> absolute
                    if (r.has_flag(PredFlag::LhsIsProg) &&
                        read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, lhs)) {
                        // ok
                    }
                    else if (r.lhs_addr_key) {
                        if (!router->read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, lhs)) continue;
                    }
                    else {
                        switch (r.width) {
                        case 1: { uint8_t  v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; lhs = v; break; }
                        case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; lhs = v; break; }
                        case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; lhs = v; break; }
                        case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; lhs = v; break; }
                        default: continue;
                        }
                    }

                    // RHS precedence: key (RhsIsKey) -> addrprog -> immediate
                    if (r.has_flag(PredFlag::RhsIsProg) &&
                        read_via_addrprog(host_, derived_.get(), *tbl, r.rhs_addrprog_offset, r.width, rhs)) {
                        // ok
                    } else if (r.has_flag(PredFlag::RhsIsKey)) {
                        if (!router->read(static_cast<addr::AddrKey>(r.rhs_addr_key), r.width, rhs)) continue;
                    }
                    else {
                        rhs = r.rhs_imm;
                    }

                    // DELTA: swap RHS to captured baseline
                    if (r.kind == 1 /* DELTA */) {
                        uint64_t cap = 0;
                        std::memcpy(&cap, bas_ptr + i * sizeof(uint64_t), sizeof(uint64_t));
                        rhs = cap;
                    }

                    bool ok = false;
                    switch (r.cmp) {
                    case 0: ok = (lhs == rhs); break; // EQ
                    case 1: ok = (lhs != rhs); break; // NE
                    case 2: ok = (lhs < rhs); break; // LT
                    case 3: ok = (lhs <= rhs); break; // LE
                    case 4: ok = (lhs > rhs); break; // GT
                    case 5: ok = (lhs >= rhs); break; // GE
                    default: ok = false; break;
                    }

                    ++total;
                    if (ok) ++pass;
                    else if (all_passed)
                    {
                        ctx[keys::core::PRED_FIRST_FAILED] = (uint32_t)r.id;
                        ctx[keys::core::PRED_FAILED_CMP_STR] = std::to_string(lhs) + " " + simcore::pred::get_cmp_string((simcore::pred::CmpOp)r.cmp) + " " + std::to_string(rhs);
                        all_passed = 0;
                    }
                }
                ctx[keys::core::PRED_PASSED] = pass;
                ctx[keys::core::PRED_ALL_PASSED] = all_passed;
                ctx[keys::core::PRED_TOTAL] = total;
                break;
            }

            case PSOpCode::RECORD_PROGRESS_AT_BP: {
                uint32_t tot = 0; ctx.get<uint32_t>(keys::core::PRED_TOTAL, tot);
                if (tot) {
                    uint32_t turn = 0, hitbp = 0, pass = 0;
                    ctx.get<uint32_t>(keys::battle::ACTIVE_TURN, turn);
                    ctx.get<uint32_t>(keys::core::RUN_HIT_BP_KEY, hitbp);
                    ctx.get<uint32_t>(keys::core::PRED_PASSED, pass);
                    auto sink = host_.getProgressSink();
                    if (sink) {
                        auto tag = host_.getCurrentSctFileTag();
                        char buf[128];
                        std::snprintf(buf, sizeof(buf), "turn=%u bp=%u pred=%u/%u %s", turn, hitbp, pass, tot, tag.c_str());
                        sink(0, 0, 0, 0, buf);
                    }
                }
                break;
            }
            }

        }
        R.ctx = ctx;
        R.ok = true;
        return R;
    }

    std::string get_psop_name(PSOpCode op)
    {
        switch (op) {
        case PSOpCode::ARM_PHASE_BPS_ONCE: return { "Arm Phase BPs Once" };
        case PSOpCode::LOAD_SNAPSHOT: return { "Load Snapshot" };
        case PSOpCode::CAPTURE_SNAPSHOT: return { "Capture Snapshot" };
        case PSOpCode::APPLY_INPUT_FROM: return { "Apply Input" };
        case PSOpCode::STEP_FRAMES: return { "Step Frames" };
        case PSOpCode::RUN_UNTIL_BP: return { "Run Until BP" };
        case PSOpCode::READ_U8: return { "Read u8" };
        case PSOpCode::READ_U16: return { "Read u16" };
        case PSOpCode::READ_U32: return { "Read u32" };
        case PSOpCode::READ_F32: return { "Read float" };
        case PSOpCode::READ_F64: return { "Read double" };
        case PSOpCode::SET_TIMEOUT_FROM: return { "Set Timeout" };
        case PSOpCode::EMIT_RESULT: return { "Emit result" };
        case PSOpCode::MOVIE_PLAY_FROM: return { "Play TAS Movie" };
        case PSOpCode::MOVIE_STOP: return { "Stop TAS Movie" };
        case PSOpCode::SAVE_SAVESTATE_FROM: return { "Save Savestate" };
        case PSOpCode::REQUIRE_DISC_GAMEID_FROM: return { "Require Disc ID" };
        case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH: return { "Build Turn Input From Actions" };
        case PSOpCode::GET_BATTLE_CONTEXT: return { "Get Battle Context" };
        case PSOpCode::GC_SLOT_A_SET_FROM: return { "Set GC Memcard Slot A" };
        case PSOpCode::LABEL: return { "Set Label" };
        case PSOpCode::GOTO: return { "Goto Label" };
        case PSOpCode::GOTO_IF: return { "Constant Goto Label If" };
        case PSOpCode::GOTO_IF_KEYS: return { "Context Goto Label If" };
        case PSOpCode::RETURN_RESULT: return { "Return Result" };
        case PSOpCode::CAPTURE_PRED_BASELINES: return { "Capture Predicate Breakpoint Baselines" };
        case PSOpCode::ARM_BPS_FROM_PRED_TABLE: return { "Arm Breakpoints from Predicate Table" };
        case PSOpCode::EVAL_PREDICATES_AT_HIT_BP: return { "Evaulate Predicates at Hit BP" };
        case PSOpCode::RECORD_PROGRESS_AT_BP: return { "Record Progress at Breakpoint" };
        case PSOpCode::SET_U32: return { "Set a u32 Context Value" };
        case PSOpCode::ADD_U32: return { "Add to a u32 Context Value" };
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES : return { "Apply Inputplan Frame from Context" };
        default:
            return { "Unknown Code" };
        }
    }

} // namespace simcore

