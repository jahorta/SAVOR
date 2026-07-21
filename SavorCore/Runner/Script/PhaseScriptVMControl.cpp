#include "PhaseScriptVM.h"

#include "ScriptProgress.h"
#include "../IPC/Wire.h"

#include <algorithm>
#include <chrono>

namespace {

    static const BPAddr* find_hit_bp(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& canonical_bp_keys,
        const std::vector<BPKey>& predicate_bp_keys,
        uint32_t pc)
    {
        for (auto k : canonical_bp_keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        for (auto k : predicate_bp_keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        return nullptr;
    }

    static const BPAddr* find_hit_bp_in_keys(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& keys,
        uint32_t pc)
    {
        for (auto k : keys) {
            if (const auto* e = bpmap.find(k); e && e->pc == pc) return e;
        }
        return nullptr;
    }

    static const BPAddr* find_hit_bp(
        const BreakpointMap& bpmap,
        const std::vector<BPKey>& canonical_bp_keys,
        const std::vector<BPKey>& gated_bp_keys,
        const std::vector<BPKey>& predicate_bp_keys,
        uint32_t pc)
    {
        if (const auto* e = find_hit_bp_in_keys(bpmap, canonical_bp_keys, pc)) return e;
        if (const auto* e = find_hit_bp_in_keys(bpmap, gated_bp_keys, pc)) return e;
        return find_hit_bp_in_keys(bpmap, predicate_bp_keys, pc);
    }

    static const char* stable_bp_id_or_empty(const BPAddr* bp)
    {
        return bp != nullptr && bp->stable_id != nullptr ? bp->stable_id : "";
    }

}

namespace savor {
    bool PhaseScriptVM::save_snapshot() {
        return host_.saveStateToBuffer(snapshot_);
    }

    bool PhaseScriptVM::load_snapshot() {
        return host_.loadStateFromBuffer(snapshot_);
    }

    void PhaseScriptVM::arm_bps_once() {
        if (armed_) return;
        std::vector<uint32_t> pcs;
        pcs.reserve(canonical_bp_keys_.size() + gated_bp_keys_.size());
        const auto append_unique_pc = [&](BPKey k) {
            if (const auto* e = bpmap_.find(k)) {
                if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                    pcs.push_back(e->pc);
                }
            }
        };
        for (const auto& k : canonical_bp_keys_) append_unique_pc(k);
        for (const auto& k : gated_bp_keys_) append_unique_pc(k);
        if (!pcs.empty()) {
            host_.armPcBreakpoints(pcs);
            armed_pcs_ = pcs;
            restore_canonical_breakpoint_scope();
        }
        armed_ = true;
    }


    void PhaseScriptVM::restore_canonical_breakpoint_scope() {
        std::vector<uint32_t> enabled_pcs;
        enabled_pcs.reserve(canonical_bp_keys_.size() + predicate_bp_keys_.size());
        const auto append_pc = [&](BPKey key) {
            if (const auto* e = bpmap_.find(key)) {
                if (std::find(enabled_pcs.begin(), enabled_pcs.end(), e->pc) == enabled_pcs.end()) {
                    enabled_pcs.push_back(e->pc);
                }
            }
        };
        for (const auto& k : canonical_bp_keys_) {
            append_pc(k);
        }
        for (const auto& k : predicate_bp_keys_) {
            append_pc(k);
        }
        host_.setEnabledPcBreakpointsOnly(enabled_pcs);
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
    bool PhaseScriptVM::op_load_snapshot(PSContext& ctx) { if (!load_snapshot()) return false; ctx[savor::context::key::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); return true; }
    bool PhaseScriptVM::op_capture_snapshot() { return save_snapshot(); }
    bool PhaseScriptVM::op_reboot_core(PSResult& result, PSContext& ctx) {
        std::string iso_path{};
        if (!ctx.get(savor::context::key::core::GAME_ISO_PATH, iso_path)) {
            ctx[savor::context::key::core::WORKER_ERROR] = (uint32_t)32;
            result.ctx = ctx;
            return false;
        }
        host_.clearMemoryWatchpoints();
        host_.clearAllPcBreakpoints();
        host_.loadGame(iso_path);
        host_.ConfigurePortsStandardPadP1();
        armed_ = false;
        armed_pcs_.clear();
        arm_bps_once();
        host_.emitProbeMarker("reboot.complete");
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
    void PhaseScriptVM::op_emit_result(const PSOp& op, PSResult& result, PSContext& ctx) const { SCLOGD("[VM] EMIT_RESULT %s=%08X", savor::context::key::name_for_id(op.key.id).data(), ctx[op.key.id]); result.ctx[op.key.id] = ctx[op.key.id]; }
    bool PhaseScriptVM::op_return_result(const PSOp& op, PSResult& result, PSContext& ctx) const { ctx[savor::context::key::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull); result.ctx = ctx; result.ctx[op.keyimm.key] = op.keyimm.imm; uint32_t dw_outcome = 0; ctx.get(savor::context::key::core::DW_RUN_OUTCOME_CODE, dw_outcome); result.ok = dw_outcome == 0; return true; }
    void PhaseScriptVM::op_set_timeout(const PSOp& op, PSContext& ctx) const { ctx[savor::context::key::core::RUN_MS] = op.imm.v; }
    void PhaseScriptVM::op_set_timeout_from(const PSOp& op, PSContext& ctx) const { uint32_t timeout_ms; ctx.get<uint32_t>(op.key.id, timeout_ms); ctx[savor::context::key::core::RUN_MS] = timeout_ms; }
    PhaseScriptVM::RunUntilBpCoreResult PhaseScriptVM::run_until_bp_core(PSContext& ctx, const RunUntilBpSpec& spec) {
        using savor::RunToBpOutcome;

        uint32_t timeout_ms = init_.default_timeout_ms;
        uint32_t vi_stall_ms = 0;
        uint32_t progress_flags = 0;
        ctx.get<uint32_t>(savor::context::key::core::RUN_MS, timeout_ms);
        ctx.get<uint32_t>(savor::context::key::core::VI_STALL_MS, vi_stall_ms);
        ctx.get<uint32_t>(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags);

        uint32_t poll_ms = spec.poll_ms_override;
        if (poll_ms == 0) {
            ctx.get<uint32_t>(savor::context::key::core::RUN_POLL_MS, poll_ms);
        }
        if (poll_ms == 0) {
            poll_ms = host_.pickPollIntervalMs(timeout_ms);
        }

        const auto collect_expected_pcs = [&]() {
            std::vector<uint32_t> pcs;
            pcs.reserve(spec.expected_bp_keys.size());
            for (const auto expected_bp : spec.expected_bp_keys) {
                if (const auto* e = bpmap_.find(expected_bp)) {
                    if (std::find(pcs.begin(), pcs.end(), e->pc) == pcs.end()) {
                        pcs.push_back(e->pc);
                    }
                }
            }
            return pcs;
        };

        uint64_t input_epoch = 0;
        if (spec.apply_input) {
            if (spec.track_input_poll) {
                input_epoch = host_.publishInputEpoch(spec.input);
            } else {
                host_.setInput(spec.input);
            }
        }

        if (spec.step_off_current_bp) {
            const uint32_t entry_pc = host_.getPC();
            if (const BPAddr* entry_bp = find_hit_bp(
                bpmap_,
                canonical_bp_keys_,
                gated_bp_keys_,
                predicate_bp_keys_,
                entry_pc)) {
                SCLOGI("[VM] run_until_bp stepoff pc=%08X bp=%u input_btn=%04X",
                    entry_pc,
                    static_cast<uint32_t>(entry_bp->key),
                    spec.input.buttons);
                host_.setEnabledPcBreakpointsOnly({});
                (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));
                if (spec.expected_only_scope) {
                    restore_canonical_breakpoint_scope();
                } else {
                    host_.setEnabledPcBreakpointsOnly(collect_expected_pcs());
                }
            }
        }

        if (spec.expected_only_scope) {
            host_.setEnabledPcBreakpointsOnly(collect_expected_pcs());
        }

        const auto t0 = std::chrono::steady_clock::now();
        DolphinWrapper::RunUntilHitResult rr{};

        run_until_bp_active_.store(true, std::memory_order_release);
        host_.disableThrottle();
        rr = host_.runUntilBreakpointFlexible(
            timeout_ms,
            vi_stall_ms,
            spec.watch_movie,
            poll_ms,
            progress_flags);
        const auto t1 = std::chrono::steady_clock::now();
        host_.enableThrottle();
        run_until_bp_active_.store(false, std::memory_order_release);

        if (rr.hit && spec.hold_input_through_hit_opcode) {
            SCLOGI("[VM] run_until_bp hold-through-hit pc=%08X input_btn=%04X",
                static_cast<uint32_t>(rr.pc),
                spec.input.buttons);
            host_.setEnabledPcBreakpointsOnly({});
            (void)host_.stepOneOpcodeBlocking(static_cast<int>(timeout_ms));
            if (spec.expected_only_scope) {
                restore_canonical_breakpoint_scope();
            }
        }

        uint32_t input_poll_count = 0;
        bool input_acknowledged = false;
        if (spec.track_input_poll && input_epoch != 0) {
            const auto receipt = host_.getInputPollReceipt();
            if (receipt.epoch == input_epoch) {
                input_poll_count = receipt.callback_count;
                input_acknowledged = receipt.acknowledged();
            }
        }

        if (spec.release_input) {
            GCInputFrame released_input = spec.input;
            released_input.buttons = static_cast<uint16_t>(released_input.buttons & ~spec.input.buttons);
            host_.setInput(released_input);
        }

        if (spec.expected_only_scope) {
            restore_canonical_breakpoint_scope();
        }

        RunToBpOutcome outcome = RunToBpOutcome::Unknown;
        if (rr.hit) outcome = RunToBpOutcome::Hit;
        else if (rr.reason) {
            if (std::strcmp(rr.reason, "timeout") == 0) outcome = RunToBpOutcome::Timeout;
            else if (std::strcmp(rr.reason, "vi_stalled") == 0) outcome = RunToBpOutcome::ViStalled;
            else if (std::strcmp(rr.reason, "movie_ended") == 0) outcome = RunToBpOutcome::MovieEnded;
            else if (std::strcmp(rr.reason, "cancelled") == 0
                || std::strcmp(rr.reason, "shutdown") == 0
                || std::strcmp(rr.reason, "control_unavailable") == 0
                || std::strcmp(rr.reason, "control_pause_failed") == 0) {
                outcome = RunToBpOutcome::Aborted;
            }
        }

        const BPAddr* hit_bp = nullptr;
        uint32_t hit_bp_key = 0;
        bool expected_match = false;
        if (rr.hit) {
            if (!spec.expected_bp_keys.empty()) {
                hit_bp = find_hit_bp_in_keys(bpmap_, spec.expected_bp_keys, static_cast<uint32_t>(rr.pc));
                if (hit_bp != nullptr) {
                    hit_bp_key = static_cast<uint32_t>(hit_bp->key);
                    expected_match = true;
                }
            }
            if (hit_bp == nullptr) {
                hit_bp = spec.include_gated_hit_lookup
                    ? find_hit_bp(bpmap_, canonical_bp_keys_, gated_bp_keys_, predicate_bp_keys_, static_cast<uint32_t>(rr.pc))
                    : find_hit_bp(bpmap_, canonical_bp_keys_, predicate_bp_keys_, static_cast<uint32_t>(rr.pc));
                if (hit_bp != nullptr) {
                    hit_bp_key = static_cast<uint32_t>(hit_bp->key);
                }
            }
            if (spec.expected_bp_keys.empty()) {
                expected_match = true;
            }
        }

        const uint32_t elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(outcome);
        ctx[savor::context::key::core::ELAPSED_MS] = elapsed_ms;
        ctx[savor::context::key::core::RUN_HIT_PC] = rr.hit ? static_cast<uint32_t>(rr.pc) : 0u;
        ctx[savor::context::key::core::RUN_HIT_BP_KEY] = hit_bp_key;
        ctx[savor::context::key::core::RUN_EXPECTED_MATCH] = expected_match ? 1u : 0u;
        ctx[savor::context::key::core::RUN_STOP_KIND] = static_cast<uint32_t>(rr.stop_kind);
        if (rr.memory_watchpoint.has_value()) {
            const auto& hit = *rr.memory_watchpoint;
            ctx[savor::context::key::core::RUN_MEMWATCH_ID] = hit.id;
            ctx[savor::context::key::core::RUN_MEMWATCH_ADDR] = hit.address;
            ctx[savor::context::key::core::RUN_MEMWATCH_SIZE] = hit.size;
            ctx[savor::context::key::core::RUN_MEMWATCH_ACCESS] = static_cast<uint32_t>(hit.access);
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_BEFORE] = hit.num_hits_before;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_AFTER] = hit.num_hits_after;
        } else {
            ctx[savor::context::key::core::RUN_MEMWATCH_ID] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_ADDR] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_SIZE] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_ACCESS] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_BEFORE] = 0u;
            ctx[savor::context::key::core::RUN_MEMWATCH_HITS_AFTER] = 0u;
        }
        ctx[savor::context::key::core::VI_DELTA] = static_cast<uint32_t>(host_.getViFieldCountApproxFromBaseline() & 0xFFFFFFFFull);
        ctx[savor::context::key::core::POLL_MS] = poll_ms;
        ctx[savor::context::key::core::VI_LAST] = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;

        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] run_until_bp outcome=%u pc=%08X bp_key=%u bp_symbol=%s expected_match=%u",
            static_cast<uint32_t>(outcome),
            rr.hit ? static_cast<uint32_t>(rr.pc) : 0u,
            hit_bp_key,
            stable_bp_id_or_empty(hit_bp),
            expected_match ? 1u : 0u);

        if (spec.update_derived && derived_) {
            derived_->update_on_bp(hit_bp_key, ctx, host_);
        }

        return RunUntilBpCoreResult{
            .run = rr,
            .outcome = outcome,
            .hit_bp_key = hit_bp_key,
            .expected_match = expected_match,
            .input_epoch = input_epoch,
            .requested_input = spec.input,
            .input_poll_count = input_poll_count,
            .input_acknowledged = input_acknowledged,
            .elapsed_ms = elapsed_ms,
        };
    }

    void PhaseScriptVM::op_run_until_bp(PSContext& ctx) {
        (void)run_until_bp_core(ctx, RunUntilBpSpec{});
    }
    bool PhaseScriptVM::op_run_until_bp_key(const PSOp& op, PSResult& result, PSContext& ctx) {
        const auto key = static_cast<BPKey>(op.imm.v);
        const auto* active_bp = bpmap_.find(key);
        if (active_bp == nullptr
            || active_bp->visibility != BreakpointVisibility::PlayerVisible) {
            SCLOGE("[VM] generic run-until rejected an unavailable breakpoint");
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            return false;
        }
        (void)run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = { key },
            .expected_only_scope = true,
            .include_gated_hit_lookup = true,
        });
        return true;
    }
    void PhaseScriptVM::op_run_until_debug_stop(PSContext& ctx) {
        (void)run_until_bp_core(ctx, RunUntilBpSpec{});
    }
    void PhaseScriptVM::op_record_current_bp(PSContext& ctx) {
        const uint32_t pc = host_.getPC();
        const BPAddr* hit_bp = find_hit_bp(bpmap_, canonical_bp_keys_, predicate_bp_keys_, pc);
        uint32_t hit_bp_key = hit_bp != nullptr ? static_cast<uint32_t>(hit_bp->key) : 0u;
        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] record_current_bp pc=%08X bp_key=%u bp_symbol=%s",
            pc,
            hit_bp_key,
            stable_bp_id_or_empty(hit_bp));
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = hit_bp_key != 0
            ? static_cast<uint32_t>(RunToBpOutcome::Hit)
            : static_cast<uint32_t>(RunToBpOutcome::Unknown);
        ctx[savor::context::key::core::RUN_HIT_PC] = pc;
        ctx[savor::context::key::core::RUN_HIT_BP_KEY] = hit_bp_key;
        ctx[savor::context::key::core::RUN_STOP_KIND] = hit_bp_key != 0
            ? static_cast<uint32_t>(DolphinWrapper::DebugStopKind::PcBreakpoint)
            : static_cast<uint32_t>(DolphinWrapper::DebugStopKind::None);
        ctx[savor::context::key::core::VI_DELTA] = 0u;
        ctx[savor::context::key::core::VI_LAST] = static_cast<uint32_t>(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        if (derived_ && hit_bp_key != 0) derived_->update_on_bp(hit_bp_key, ctx, host_);
    }
} // namespace savor
