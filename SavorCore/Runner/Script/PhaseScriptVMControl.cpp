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
    void PhaseScriptVM::arm_bps_once() {
        armed_pcs_.clear();
        armed_ = false;
        SCLOGE(
            "[VM] legacy breakpoint ownership is disconnected; use StopPointRouter");
    }


    void PhaseScriptVM::restore_canonical_breakpoint_scope() {
        armed_pcs_.clear();
        armed_ = false;
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

    bool PhaseScriptVM::op_arm_phase_bps_once()
    {
        arm_bps_once();
        return false;
    }
    bool PhaseScriptVM::op_load_snapshot(
        PSResult& result,
        PSContext& ctx)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] state-buffer restoration is disconnected; use StateService");
    }

    bool PhaseScriptVM::op_capture_snapshot(
        PSResult& result,
        PSContext& ctx)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] state-buffer capture is disconnected; use StateService");
    }

    bool PhaseScriptVM::op_reboot_core(PSResult& result, PSContext& ctx) {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] core reboot is disconnected; use StateService");
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
    PhaseScriptVM::RunUntilBpCoreResult PhaseScriptVM::run_until_bp_core(
        PSContext& ctx,
        const RunUntilBpSpec& spec)
    {
        // PhaseScriptVM is retained only as translation evidence during the
        // hard cutover. It must not become a compatibility executor around
        // the canonical ExecutionEngine.
        run_until_bp_active_.store(false, std::memory_order_release);
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] =
            static_cast<uint32_t>(RunToBpOutcome::Aborted);
        ctx[savor::context::key::core::ELAPSED_MS] = 0u;
        ctx[savor::context::key::core::RUN_HIT_PC] = 0u;
        ctx[savor::context::key::core::RUN_HIT_BP_KEY] = 0u;
        ctx[savor::context::key::core::RUN_EXPECTED_MATCH] = 0u;
        ctx[savor::context::key::core::RUN_STOP_KIND] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_ID] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_ADDR] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_SIZE] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_ACCESS] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_HITS_BEFORE] = 0u;
        ctx[savor::context::key::core::RUN_MEMWATCH_HITS_AFTER] = 0u;
        SCLOGE(
            "[VM] run-until is disconnected; use the canonical ExecutionEngine");

        return RunUntilBpCoreResult{
            .run = {},
            .outcome = RunToBpOutcome::Aborted,
            .requested_input = spec.input,
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
    bool PhaseScriptVM::op_record_current_pc_to(
        const PSOp& op,
        PSResult& result,
        PSContext& ctx) const {
        if (savor::context::key::name_for_id(op.key.id).empty()) {
            SCLOGE(
                "[VM] RECORD_CURRENT_PC_TO rejected unknown context key=%u",
                static_cast<unsigned>(op.key.id));
            ctx[savor::context::key::core::WORKER_ERROR] =
                static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            result.ok = false;
            return false;
        }
        ctx[op.key.id] = host_.getPC();
        return true;
    }
} // namespace savor
