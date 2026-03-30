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
#include "../../Core/Input/AppliedTurnTapeBlob.h"
#include "../../Core/Memory/IKeyReader.h"
#include "../../Core/Memory/DerivedBase.h"
#include "../../Core/Memory/Soa/Battle/DerivedBattleBuffer.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../Breakpoints/BPRegistry.h"
#include "ScriptProgress.h"
#include <thread>
#include <sstream>

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
    namespace {
        std::string ps_cmp_to_string(PSCmp cmp) {
            switch (cmp) {
            case PSCmp::EQ: return "EQ";
            case PSCmp::NE: return "NE";
            case PSCmp::LT: return "LT";
            case PSCmp::LE: return "LE";
            case PSCmp::GT: return "GT";
            case PSCmp::GE: return "GE";
            default: return "?";
            }
        }

        std::string key_desc(simcore::keys::KeyId key) {
            const std::string_view name = simcore::keys::name_for_id(key);
            if (!name.empty()) return std::string(name);
            return std::to_string(static_cast<uint32_t>(key));
        }
    }

    PhaseScriptVM::PhaseScriptVM(simcore::DolphinWrapper& host, const BreakpointMap& bpmap)
        : host_(host), bpmap_(bpmap) {
    }

    void PhaseScriptVM::SetVisualDebugMode(bool enabled)
    {
        visual_debug_mode_ = enabled;
        if (!enabled) {
            visual_debug_paused_.store(false, std::memory_order_release);
            visual_debug_vm_step_budget_.store(0, std::memory_order_release);
        }
    }

    void PhaseScriptVM::SetVisualDebugPaused(bool paused)
    {
        visual_debug_paused_.store(paused, std::memory_order_release);
        if (!paused) {
            visual_debug_vm_step_budget_.store(0, std::memory_order_release);
        }
    }

    void PhaseScriptVM::StepVisualDebugVmOnce()
    {
        visual_debug_paused_.store(true, std::memory_order_release);
        visual_debug_vm_step_budget_.fetch_add(1, std::memory_order_acq_rel);
    }

    bool PhaseScriptVM::IsVisualDebugVmPaused() const
    {
        return visual_debug_paused_.load(std::memory_order_acquire);
    }

    bool PhaseScriptVM::IsRunUntilBpActive() const
    {
        return run_until_bp_active_.load(std::memory_order_acquire);
    }

    void PhaseScriptVM::wait_for_visual_debug_gate()
    {
        if (!visual_debug_mode_) return;
        for (;;) {
            if (!visual_debug_paused_.load(std::memory_order_acquire)) {
                return;
            }
            uint32_t budget = visual_debug_vm_step_budget_.load(std::memory_order_acquire);
            if (budget > 0) {
                if (visual_debug_vm_step_budget_.compare_exchange_strong(budget, budget - 1, std::memory_order_acq_rel)) {
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
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

        SCLOGDX(SC_TAGS("vm", "init"), "[VM] init begin sav=%s timeout=%u", init.savestate_path.c_str(), init.default_timeout_ms);

        // Disarm any previously armed set (enables program swapping)
        if (armed_ && !armed_pcs_.empty()) {
            host_.disarmPcBreakpoints(armed_pcs_);
            armed_pcs_.clear();
        }
        armed_ = false;

        // Optional savestate (allow empty path for boot-based phases)
        if (!init_.savestate_path.empty()) {
            if (!host_.loadSavestate(init_.savestate_path.c_str()))
                return false;
        }

        // Update canonical BP keys and arm once
        canonical_bp_keys_ = prog_.canonical_bp_keys;

        SCLOGDX(SC_TAGS("vm", "breakpoint"), "[VM] attach bp count=%zu", program.canonical_bp_keys.size());
        arm_bps_once();

        // Capture a snapshot to use as the per-job baseline
        const bool snapshot_ok = save_snapshot();
        if (snapshot_ok) SCLOGDX(SC_TAGS("vm", "init"), "[VM] init ok");
        return snapshot_ok;
    }

    bool PhaseScriptVM::compare_u32(uint32_t lhs, PSCmp cmp, uint32_t rhs) const {
        switch (cmp) {
        case PSCmp::EQ: return lhs == rhs;
        case PSCmp::NE: return lhs != rhs;
        case PSCmp::LT: return lhs < rhs;
        case PSCmp::LE: return lhs <= rhs;
        case PSCmp::GT: return lhs > rhs;
        case PSCmp::GE: return lhs >= rhs;
        default: return false;
        }
    }

    void PhaseScriptVM::jump_to_label_if_exists(const std::string& label, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        auto it = label_vm_pc_map.find(label);
        if (it != label_vm_pc_map.end()) {
            section = label;
            vm_pc = it->second;
        }
    }

    bool PhaseScriptVM::op_arm_phase_bps_once() { arm_bps_once(); return true; }
    bool PhaseScriptVM::op_load_snapshot(PSContext& ctx) { if (!load_snapshot()) return false; ctx[keys::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); return true; }
    bool PhaseScriptVM::op_capture_snapshot() { return save_snapshot(); }
    bool PhaseScriptVM::op_reboot_core(PSResult& result, PSContext& ctx) {
        std::string iso_path{};
        if (!ctx.get(keys::core::GAME_ISO_PATH, iso_path)) {
            ctx[keys::core::WORKER_ERROR] = (uint32_t)32;
            result.ctx = ctx;
            return false;
        }
        host_.clearAllPcBreakpoints();
        host_.loadGame(iso_path);
        host_.ConfigurePortsStandardPadP1();
        armed_ = false;
        armed_pcs_.clear();
        arm_bps_once();
        return true;
    }
    void PhaseScriptVM::op_label() const {}
    void PhaseScriptVM::op_goto(const PSOp& op, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const { jump_to_label_if_exists(op.jmp.name, label_vm_pc_map, vm_pc, section); }
    void PhaseScriptVM::op_goto_if(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        uint32_t lv = 0; ctx.get(op.jcc.key, lv);
        if (compare_u32(lv, op.jcc.cmp, op.jcc.imm)) jump_to_label_if_exists(op.jcc.name, label_vm_pc_map, vm_pc, section);
    }
    void PhaseScriptVM::op_goto_if_keys(const PSOp& op, PSContext& ctx, const std::unordered_map<std::string, size_t>& label_vm_pc_map, size_t& vm_pc, std::string& section) const {
        uint32_t lv = 0, rv = 0; ctx.get(op.jcc2.left, lv); ctx.get(op.jcc2.right, rv);
        if (compare_u32(lv, op.jcc2.cmp, rv)) jump_to_label_if_exists(op.jcc2.name, label_vm_pc_map, vm_pc, section);
    }
    void PhaseScriptVM::op_set_u32(const PSOp& op, PSContext& ctx) const { ctx[op.keyimm.key] = op.keyimm.imm; }
    void PhaseScriptVM::op_add_u32(const PSOp& op, PSContext& ctx) const { uint32_t v = 0; ctx.get<uint32_t>(op.keyimm.key, v); ctx[op.keyimm.key] = v + op.keyimm.imm; }
    void PhaseScriptVM::op_step_frames(const PSOp& op) { SCLOGD("[VM] phase=run_inputs begin frames=%zu", op.step.n); if (op.imm.v == 1) host_.setEnableAllBreakpoints(false); for (uint32_t i = 0; i < op.step.n; ++i) host_.stepOneFrameBlocking(); if (op.imm.v == 1) host_.setEnableAllBreakpoints(true); SCLOGD("[VM] phase=run_inputs end"); }
    void PhaseScriptVM::op_start_deterministic_run() const { if (!host_.startMovieRecording()) SCLOGE("[VM] Unable to start recording for deterministic run"); }
    void PhaseScriptVM::op_end_deterministic_run() const { host_.endMovieRecording(); }
    bool PhaseScriptVM::op_read_u8(const PSOp& op, PSResult&, PSContext& ctx) { uint8_t v{}; if (!read_u8(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u16(const PSOp& op, PSResult&, PSContext& ctx) { uint16_t v{}; if (!read_u16(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u32(const PSOp& op, PSResult&, PSContext& ctx) { uint32_t v{}; if (!read_u32(op.rd.addr, v)) { SCLOGD("[VM] READ_U32 FAIL @%08X key=%s", op.rd.addr, keys::name_for_id(op.rd.dst).data()); return false; } SCLOGD("[VM] READ_U32 @%08X -> %08X key=%s", op.rd.addr, v, keys::name_for_id(op.rd.dst).data()); ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_f32(const PSOp& op, PSResult&, PSContext& ctx) { float v{}; if (!read_f32(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_f64(const PSOp& op, PSResult&, PSContext& ctx) { double v{}; if (!read_f64(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    void PhaseScriptVM::op_emit_result(const PSOp& op, PSResult& result, PSContext& ctx) const { SCLOGD("[VM] EMIT_RESULT %s=%08X", keys::name_for_id(op.key.id).data(), ctx[op.key.id]); result.ctx[op.key.id] = ctx[op.key.id]; }
    bool PhaseScriptVM::op_return_result(const PSOp& op, PSResult& result, PSContext& ctx) const { ctx[keys::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); result.ctx = ctx; result.ctx[op.keyimm.key] = op.keyimm.imm; uint32_t dw_outcome = 0; ctx.get(keys::core::DW_RUN_OUTCOME_CODE, dw_outcome); result.ok = dw_outcome == 0; return true; }
    bool PhaseScriptVM::op_apply_input_from(const PSOp& op, PSResult&, PSContext& ctx) { auto it = ctx.find(op.key.id); if (it == ctx.end()) return false; if (auto p = std::get_if<GCInputFrame>(&it->second)) { host_.setInput(*p); return true; } return false; }
    void PhaseScriptVM::op_set_timeout(const PSOp& op, PSContext& ctx) const { ctx[keys::core::RUN_MS] = op.imm.v; }
    void PhaseScriptVM::op_set_timeout_from(const PSOp& op, PSContext& ctx) const { uint32_t timeout_ms; ctx.get<uint32_t>(op.key.id, timeout_ms); ctx[keys::core::RUN_MS] = timeout_ms; }
    bool PhaseScriptVM::op_movie_play_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string path; ctx.get<std::string>(op.key.id, path); host_.clearAllPcBreakpoints(); if (!host_.startMoviePlayback(path)) return false; armed_ = false; armed_pcs_.clear(); arm_bps_once(); return true; }
    bool PhaseScriptVM::op_save_savestate_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string path; ctx.get<std::string>(op.key.id, path); if (path.empty()) return true; if (!host_.saveSavestateBlocking(path)) return false; ctx[keys::core::LAST_SAVESTATE_PATH] = path; return true; }
    bool PhaseScriptVM::op_require_disc_gameid_from(const PSOp& op, PSResult&, PSContext& ctx) { std::string tmp; ctx.get<std::string>(op.key.id, tmp); if (tmp.size() < 6) return false; auto di = host_.getDiscInfo(); return di.has_value() && di->game_id.size() >= 6 && std::memcmp(di->game_id.data(), tmp.c_str(), 6) == 0; }
    void PhaseScriptVM::op_build_turn_inputplan_from_battle_path(PSContext& ctx) const {
        uint32_t turn = 0;
        ctx.get<uint32_t>(keys::battle::ACTIVE_TURN, turn);
        if (turn == 0) {
            ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::InvalidTurnIdxZero;
            ctx[keys::core::PLAN_DONE] = (uint32_t)1;
        }
        soa::battle::actions::BattlePath bp;
        if (!ctx.get<soa::battle::actions::BattlePath>(keys::battle::TURN_PLANS, bp)) {
            ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::BadBlob;
            ctx[keys::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        if (turn > bp.size()) {
            ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)soa::battle::actions::MaterializeErr::OutOfTurns;
            ctx[keys::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        soa::battle::ctx::BattleContext bc{};
        std::string blob;
        if (ctx.get<std::string>(keys::battle::CTX_BLOB, blob)) soa::battle::ctx::codec::decode(blob, bc);
        const auto& turn_plan = bp[turn - 1];
        simcore::InputPlan plan;
        auto err = soa::battle::actions::MaterializeErr::OK;
        if (!soa::battle::actions::ActionLibrary::generateTurnPlan(bc, turn_plan, plan, err)) {
            ctx[keys::battle::PLAN_MATERIALIZE_ERR] = (uint32_t)err;
            ctx[keys::core::PLAN_DONE] = (uint32_t)1;
            return;
        }
        const uint32_t n = static_cast<uint32_t>(plan.size());
        std::string counts; counts.resize(sizeof(uint32_t)); std::memcpy(counts.data(), &n, sizeof(uint32_t));
        std::string frames; frames.resize(n * sizeof(simcore::GCInputFrame)); if (n) std::memcpy(frames.data(), plan.data(), frames.size());
        ctx[simcore::keys::battle::NUM_TURN_PLANS] = uint32_t(1);
        ctx[simcore::keys::battle::INPUTPLAN_FRAME_COUNT] = counts;
        ctx[simcore::keys::battle::INPUTPLAN] = frames;
        ctx[keys::battle::PLAN_MATERIALIZE_ERR] = uint32_t((uint32_t)soa::battle::actions::MaterializeErr::OK);
        ctx[keys::core::PLAN_DONE] = uint32_t(0);
    }

    void PhaseScriptVM::op_apply_battle_inputplan_frames(PSContext& ctx) {
        auto itC = ctx.find(keys::battle::INPUTPLAN_FRAME_COUNT);
        auto itT = ctx.find(keys::battle::INPUTPLAN);
        if (itC == ctx.end() || itT == ctx.end()) return;
        const auto* counts_s = std::get_if<std::string>(&itC->second);
        const auto* table_s = std::get_if<std::string>(&itT->second);
        if (!counts_s || !table_s) return;
        const uint8_t* counts = (const uint8_t*)counts_s->data();
        const uint8_t* frames = (const uint8_t*)table_s->data();
        if (counts == 0) { ctx[keys::core::PLAN_DONE] = uint32_t(1); return; }
        const uint32_t apply_vi_start = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        host_.setEnableAllBreakpoints(false);
        uint32_t idx;
        const uint32_t count = *(const uint32_t*)(counts);
        simcore::InputPlan applied_plan{}; applied_plan.reserve(count);
        std::vector<uint32_t> vi_durations{}; vi_durations.reserve(count);
        for (idx = 0; idx < count; idx++) {
            wait_for_visual_debug_gate();
            GCInputFrame f{}; std::memcpy(&f, frames + (idx * sizeof(GCInputFrame)), sizeof(GCInputFrame)); applied_plan.push_back(f);
            const uint32_t vi_before = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
            SCLOGDX("[vm] setting input [%d]: %s", idx, DescribeFrameCompact(f).c_str());
            host_.setInput(f);
            host_.stepOneFrameBlocking();
            const uint32_t vi_after = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
            vi_durations.push_back((vi_after >= vi_before) ? (vi_after - vi_before) : 0u);
        }
        host_.setEnableAllBreakpoints(true);
        const uint32_t apply_vi_end = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        uint32_t turn_number = 0;
        if (!ctx.get(keys::battle::TURN_OUTPUT_INDEX, turn_number)) (void)ctx.get(keys::battle::ACTIVE_TURN, turn_number);
        std::string turn_blob; (void)ctx.get(keys::battle::APPLIED_INPUTPLAN_TURN_BLOB, turn_blob);
        simcore::inputtape::TurnChunk chunk{}; chunk.turn_number = turn_number; chunk.vi_start = apply_vi_start; chunk.vi_end = apply_vi_end; chunk.frames = applied_plan; chunk.vi_durations = vi_durations;
        (void)simcore::inputtape::append_turn_chunk(turn_blob, chunk);
        ctx[keys::battle::APPLIED_INPUTPLAN_TURN_BLOB] = std::move(turn_blob);
        if (idx >= count) ctx[keys::core::PLAN_DONE] = uint32_t(1);
        uint32_t cur_turn_plans = 0;
        ctx.get(keys::battle::NUM_TURN_PLANS, cur_turn_plans);
        ctx[keys::battle::NUM_TURN_PLANS] = cur_turn_plans > 0 ? cur_turn_plans - 1 : 0;
    }
    void PhaseScriptVM::op_run_until_bp(PSContext& ctx) {
        using simcore::RunToBpOutcome;
        run_until_bp_active_.store(true, std::memory_order_release);
        uint32_t timeout_ms = init_.default_timeout_ms; ctx.get<uint32_t>(keys::core::RUN_MS, timeout_ms);
        uint32_t vi_stall_ms = 0; ctx.get<uint32_t>(keys::core::VI_STALL_MS, vi_stall_ms);
        const uint32_t poll_ms = host_.pickPollIntervalMs(timeout_ms);
        const bool watch_movie = true;
        uint32_t progress_flags = 0; ctx.get<uint32_t>(keys::core::PROGRESS_CORE_FLAGS, progress_flags);
        auto t0 = std::chrono::steady_clock::now();
        host_.disableThrottle();
        auto rr = host_.runUntilBreakpointFlexible(timeout_ms, vi_stall_ms, watch_movie, poll_ms, progress_flags);
        run_until_bp_active_.store(false, std::memory_order_release);
        host_.enableThrottle();
        auto t1 = std::chrono::steady_clock::now();
        const uint32_t elapsed_ms = (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        RunToBpOutcome outcome = RunToBpOutcome::Unknown;
        if (rr.hit) outcome = RunToBpOutcome::Hit;
        else if (rr.reason) {
            if (std::strcmp(rr.reason, "timeout") == 0) outcome = RunToBpOutcome::Timeout;
            else if (std::strcmp(rr.reason, "vi_stalled") == 0) outcome = RunToBpOutcome::ViStalled;
            else if (std::strcmp(rr.reason, "movie_ended") == 0) outcome = RunToBpOutcome::MovieEnded;
        }
        ctx[keys::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(outcome);
        ctx[keys::core::ELAPSED_MS] = elapsed_ms;
        ctx[keys::core::RUN_HIT_PC] = rr.hit ? (uint32_t)rr.pc : (uint32_t)0u;
        ctx[keys::core::VI_DELTA] = (uint32_t)(host_.getViFieldCountApproxFromBaseline() & 0xFFFFFFFFull);
        ctx[keys::core::POLL_MS] = poll_ms;
        ctx[keys::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        uint32_t hit_bp_key = 0;
        if (rr.hit) {
            for (auto k : canonical_bp_keys_) if (const auto* e = bpmap_.find(k); e && e->pc == rr.pc) { hit_bp_key = (uint32_t)e->key; break; }
            for (auto k : predicate_bp_keys_) if (const auto* e = bpmap_.find(k); e && e->pc == rr.pc) { hit_bp_key = (uint32_t)e->key; break; }
        }
        ctx[keys::core::RUN_HIT_BP_KEY] = hit_bp_key;
        if (derived_) derived_->update_on_bp(hit_bp_key, ctx, host_);
    }
    void PhaseScriptVM::op_get_battle_context(PSResult& result, PSContext& ctx) const {
        std::string mem1;
        if (!host_.getMem1(mem1)) { result.ok = false; return; }
        simcore::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        soa::battle::ctx::BattleContext bc{};
        if (!soa::battle::ctx::codec::extract_from_mem1(view, bc)) { result.ok = false; return; }
        std::string blob; soa::battle::ctx::codec::encode(bc, blob);
        ctx[simcore::keys::battle::CTX_BLOB] = blob;
    }
    void PhaseScriptVM::op_arm_bps_from_pred_table(PSContext& ctx) {
        auto itN = ctx.find(keys::core::PRED_COUNT);
        auto itT = ctx.find(keys::core::PRED_TABLE);
        if (itN == ctx.end() || itT == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        if (!n || !tbl) return;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        std::vector<uint32_t> pcs; pcs.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            const BPAddr* e = bpmap_.find(static_cast<BPKey>(rec[i].required_bp));
            if (!e || !e->pc) continue;
            pcs.push_back(e->pc); predicate_bp_keys_.push_back(e->key);
        }
        if (!pcs.empty()) host_.armPcBreakpoints(pcs);
    }
    void PhaseScriptVM::op_capture_pred_baselines(PSContext& ctx, KeyHostRouter& router) {
        auto itN = ctx.find(keys::core::PRED_COUNT);
        auto itT = ctx.find(keys::core::PRED_TABLE);
        auto itB = ctx.find(keys::core::PRED_BASELINES);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        auto* bas = std::get_if<std::string>(&itB->second);
        if (!n || !tbl || !bas) return;
        using simcore::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::CaptureBaseline) || !r.has_flag(PredFlag::Active)) continue;
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
        uint32_t total = 0; ctx.get(keys::core::PRED_TOTAL, total);
        uint32_t pass = 0; ctx.get(keys::core::PRED_PASSED, pass);
        auto itN = ctx.find(keys::core::PRED_COUNT);
        auto itT = ctx.find(keys::core::PRED_TABLE);
        auto itB = ctx.find(keys::core::PRED_BASELINES);
        auto itHit = ctx.find(keys::core::RUN_HIT_BP_KEY);
        if (itN == ctx.end() || itT == ctx.end() || itB == ctx.end() || itHit == ctx.end()) return;
        const uint32_t n = std::get<uint32_t>(itN->second);
        const auto* tbl = std::get_if<std::string>(&itT->second);
        const auto* bas = std::get_if<std::string>(&itB->second);
        const uint32_t hit = std::get<uint32_t>(itHit->second);
        if (!n || !tbl || !bas || !hit) return;
        using simcore::pred::PredFlag;
        const auto* rec = reinterpret_cast<const pred::PredicateRecord*>(tbl->data());
        const uint8_t* bas_ptr = reinterpret_cast<const uint8_t*>(bas->data());
        for (uint32_t i = 0; i < n; ++i) {
            const auto& r = rec[i];
            if (!r.has_flag(PredFlag::Active) || (r.required_bp && r.required_bp != hit)) continue;
            uint64_t lhs = 0, rhs = 0;
            if (r.has_flag(PredFlag::LhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.lhs_addrprog_offset, r.width, lhs)) {}
            else if (r.has_flag(PredFlag::LhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.lhs_addr_key), r.width, lhs)) continue; }
            else { switch (r.width) { case 1: { uint8_t v = 0; if (!host_.readU8(r.lhs_addr, v)) continue; lhs = v; break; } case 2: { uint16_t v = 0; if (!host_.readU16(r.lhs_addr, v)) continue; lhs = v; break; } case 4: { uint32_t v = 0; if (!host_.readU32(r.lhs_addr, v)) continue; lhs = v; break; } case 8: { uint64_t v = 0; if (!host_.readU64(r.lhs_addr, v)) continue; lhs = v; break; } default: continue; } }
            if (r.has_flag(PredFlag::RhsIsProg) && read_via_addrprog(host_, derived_.get(), *tbl, r.rhs_addrprog_offset, r.width, rhs)) {}
            else if (r.has_flag(PredFlag::RhsIsKey)) { if (!router.read(static_cast<addr::AddrKey>(r.rhs_addr_key), r.width, rhs)) continue; }
            else rhs = r.rhs_imm;
            if (r.kind == 1) { uint64_t cap = 0; std::memcpy(&cap, bas_ptr + i * sizeof(uint64_t), sizeof(uint64_t)); rhs = cap; }
            bool ok = false;
            switch (r.cmp) { case 0: ok = (lhs == rhs); break; case 1: ok = (lhs != rhs); break; case 2: ok = (lhs < rhs); break; case 3: ok = (lhs <= rhs); break; case 4: ok = (lhs > rhs); break; case 5: ok = (lhs >= rhs); break; default: ok = false; break; }
            std::string cmp_string = std::to_string(lhs) + " " + pred::get_cmp_string((pred::CmpOp)r.cmp) + " " + std::to_string(rhs);
            uint32_t progress;
            if (ctx.get(keys::core::PROGRESS_CORE_FLAGS, progress) && (progress & (uint32_t)CoreProgressFlags::PredicateProgress) != 0 && host_.getProgressSink()) {
                std::string msg = std::format("{} - {}", r.name, cmp_string);
                msg = std::string("Pred") + (ok ? "(Passed): " : "(Failed): ") + msg;
                host_.getProgressSink()(msg.c_str(), true);
            }
            ++total; if (ok) ++pass;
            if (!ok && r.has_flag(pred::PredFlag::AbortOnFail)) { ctx[keys::core::PRED_ABORT_RUN] = (uint32_t)1; break; }
        }
        ctx[keys::core::PRED_PASSED] = pass;
        ctx[keys::core::PRED_TOTAL] = total;
    }

    PSResult PhaseScriptVM::run(const PSJob& job)
    {
        PSResult R{};
        auto ctx_heap = std::make_unique<PSContext>(job.ctx);
        PSContext& ctx = *ctx_heap;
        predicate_bp_keys_.clear();
        std::string _section = "Entry Point";
        // Always start by restoring the pre-captured snapshot for each job
        if (!load_snapshot()) return R;

        ctx[keys::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);

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
            wait_for_visual_debug_gate();
            const auto& op = prog_.ops[vm_pc];
            SCLOGT("[VM] running op: %s", get_psop_desc(op).c_str());

            switch (op.code) {
            case PSOpCode::ARM_PHASE_BPS_ONCE: if (!op_arm_phase_bps_once()) return R; break;
            case PSOpCode::LOAD_SNAPSHOT: if (!op_load_snapshot(ctx)) return R; break;
            case PSOpCode::CAPTURE_SNAPSHOT: if (!op_capture_snapshot()) return R; break;
            case PSOpCode::REBOOT_CORE: if (!op_reboot_core(R, ctx)) return R; break;
            case PSOpCode::LABEL: op_label(); break;
            case PSOpCode::GOTO: op_goto(op, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::GOTO_IF: op_goto_if(op, ctx, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::GOTO_IF_KEYS: op_goto_if_keys(op, ctx, label_vm_pc_map, vm_pc, _section); break;
            case PSOpCode::SET_U32: op_set_u32(op, ctx); break;
            case PSOpCode::ADD_U32: op_add_u32(op, ctx); break;
            case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH: op_build_turn_inputplan_from_battle_path(ctx); break;
            case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES: op_apply_battle_inputplan_frames(ctx); break;
            case PSOpCode::STEP_FRAMES: op_step_frames(op); break;
            case PSOpCode::START_DETERMINISIC_RUN: op_start_deterministic_run(); break;
            case PSOpCode::END_DETERMINISTIC_RUN: op_end_deterministic_run(); break;
            case PSOpCode::RUN_UNTIL_BP: op_run_until_bp(ctx); break;
            case PSOpCode::READ_U8: if (!op_read_u8(op, R, ctx)) return R; break;
            case PSOpCode::READ_U16: if (!op_read_u16(op, R, ctx)) return R; break;
            case PSOpCode::READ_U32: if (!op_read_u32(op, R, ctx)) return R; break;
            case PSOpCode::READ_F32: if (!op_read_f32(op, R, ctx)) return R; break;
            case PSOpCode::READ_F64: if (!op_read_f64(op, R, ctx)) return R; break;
            case PSOpCode::GET_BATTLE_CONTEXT: op_get_battle_context(R, ctx); break;
            case PSOpCode::EMIT_RESULT: op_emit_result(op, R, ctx); break;
            case PSOpCode::RETURN_RESULT: if (op_return_result(op, R, ctx)) return R; break;
            case PSOpCode::APPLY_INPUT_FROM: if (!op_apply_input_from(op, R, ctx)) return R; break;
            case PSOpCode::SET_TIMEOUT: op_set_timeout(op, ctx); break;
            case PSOpCode::SET_TIMEOUT_FROM: op_set_timeout_from(op, ctx); break;
            case PSOpCode::MOVIE_PLAY_FROM: if (!op_movie_play_from(op, R, ctx)) return R; break;
            case PSOpCode::SAVE_SAVESTATE_FROM: if (!op_save_savestate_from(op, R, ctx)) return R; break;
            case PSOpCode::REQUIRE_DISC_GAMEID_FROM: if (!op_require_disc_gameid_from(op, R, ctx)) return R; break;
            case PSOpCode::ARM_BPS_FROM_PRED_TABLE: op_arm_bps_from_pred_table(ctx); break;
            case PSOpCode::CAPTURE_PRED_BASELINES: op_capture_pred_baselines(ctx, *router); break;
            case PSOpCode::EVAL_PREDICATES_AT_HIT_BP: op_eval_predicates_at_hit_bp(ctx, *router); break;
            default: break;
            }
        }
        ctx[keys::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
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
        case PSOpCode::SET_U32: return { "Set a u32 Context Value" };
        case PSOpCode::ADD_U32: return { "Add to a u32 Context Value" };
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES : return { "Apply Inputplan Frame from Context" };
        default:
            return { "Unknown Code" };
        }
    }

    std::string get_psop_desc(const PSOp& op)
    {
        std::ostringstream args;
        switch (op.code) {
        case PSOpCode::READ_U8:
        case PSOpCode::READ_U16:
        case PSOpCode::READ_U32:
        case PSOpCode::READ_F32:
        case PSOpCode::READ_F64:
            args << "addr=" << op.rd.addr << ", dst=" << key_desc(op.rd.dst);
            break;
        case PSOpCode::APPLY_INPUT_FROM:
        case PSOpCode::SET_TIMEOUT_FROM:
        case PSOpCode::MOVIE_PLAY_FROM:
        case PSOpCode::SAVE_SAVESTATE_FROM:
        case PSOpCode::REQUIRE_DISC_GAMEID_FROM:
        case PSOpCode::GC_SLOT_A_SET_FROM:
        case PSOpCode::EMIT_RESULT:
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES:
            args << "key=" << key_desc(op.key.id);
            break;
        case PSOpCode::STEP_FRAMES:
            args << "n=" << op.step.n << ", disable_breakpoints=" << op.imm.v;
            break;
        case PSOpCode::SET_TIMEOUT:
            args << "ms=" << op.imm.v;
            break;
        case PSOpCode::LABEL:
            args << "name=" << op.label.name;
            break;
        case PSOpCode::GOTO:
            args << "name=" << op.jmp.name;
            break;
        case PSOpCode::GOTO_IF:
            args << "key=" << key_desc(op.jcc.key)
                 << ", cmp=" << ps_cmp_to_string(op.jcc.cmp)
                 << ", imm=" << op.jcc.imm
                 << ", name=" << op.jcc.name;
            break;
        case PSOpCode::GOTO_IF_KEYS:
            args << "left=" << key_desc(op.jcc2.left)
                 << ", cmp=" << ps_cmp_to_string(op.jcc2.cmp)
                 << ", right=" << key_desc(op.jcc2.right)
                 << ", name=" << op.jcc2.name;
            break;
        case PSOpCode::RETURN_RESULT:
        case PSOpCode::SET_U32:
        case PSOpCode::ADD_U32:
            args << "key=" << key_desc(op.keyimm.key) << ", imm=" << op.keyimm.imm;
            break;
        default:
            break;
        }
        return get_psop_name(op.code) + ": [" + args.str() + "]";
    }

} // namespace simcore
