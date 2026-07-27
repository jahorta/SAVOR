#include "PhaseScriptVM.h"

#include "../IPC/Wire.h"
#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"

namespace savor {
    bool PhaseScriptVM::op_read_u8(const PSOp& op, PSResult&, PSContext& ctx) { uint8_t v{}; if (!read_u8(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u16(const PSOp& op, PSResult&, PSContext& ctx) { uint16_t v{}; if (!read_u16(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u32(const PSOp& op, PSResult&, PSContext& ctx) { uint32_t v{}; if (!read_u32(op.rd.addr, v)) { SCLOGD("[VM] READ_U32 FAIL @%08X key=%s", op.rd.addr, savor::context::key::name_for_id(op.rd.dst).data()); return false; } SCLOGD("[VM] READ_U32 @%08X -> %08X key=%s", op.rd.addr, v, savor::context::key::name_for_id(op.rd.dst).data()); ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_write_u32(
        const PSOp& op,
        PSResult& result,
        PSContext& ctx)
    {
        uint32_t value = 0;
        ctx[savor::context::key::core::MEMWRITE_ADDR] = op.rd.addr;
        ctx[savor::context::key::core::MEMWRITE_VALUE] = 0u;
        ctx[savor::context::key::core::MEMWRITE_READBACK] = 0u;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = 0u;

        if (!ctx.get<uint32_t>(op.rd.dst, value)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 2u;
            return fail_legacy_service(
                result,
                ctx,
                "[VM] guest-memory write rejected a missing source value");
        }
        ctx[savor::context::key::core::MEMWRITE_VALUE] = value;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = 3u;
        return fail_legacy_service(
            result,
            ctx,
            "[VM] guest-memory mutation is disconnected; use GuestMutationService");
    }
    bool PhaseScriptVM::op_read_f32(const PSOp& op, PSResult&, PSContext& ctx) { float v{}; if (!read_f32(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_f64(const PSOp& op, PSResult&, PSContext& ctx) { double v{}; if (!read_f64(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }

    bool PhaseScriptVM::op_arm_memory_watchpoint(const PSOp& op, PSResult& result, PSContext& ctx) {
        (void)op;
        ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] =
            static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
        return fail_legacy_service(
            result,
            ctx,
            "[VM] memory-watchpoint registration is disconnected; use StopPointRouter through CaptureService");
    }

    bool PhaseScriptVM::op_clear_memory_watchpoints(
        PSResult& result,
        PSContext& ctx) const
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] memory-watchpoint release is disconnected; use scoped router resources");
    }

    bool PhaseScriptVM::op_capture_seed_override(
        PSResult& result,
        PSContext& ctx)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] direct capture emission is disconnected; use TelemetryBus or CaptureService");
    }

    bool PhaseScriptVM::op_arm_capture_memory_watchpoints(
        PSResult& result,
        PSContext& ctx)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] capture watchpoints are disconnected; use CaptureService");
    }

    void PhaseScriptVM::op_get_battle_context(PSResult& result, PSContext& ctx) const {
        std::string mem1;
        if (!host_.getMem1(mem1)) { result.ok = false; return; }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        soa::battle::ctx::BattleContext bc{};
        if (!soa::battle::ctx::codec::extract_from_mem1(view, bc)) { result.ok = false; return; }
        std::string blob; soa::battle::ctx::codec::encode(bc, blob);
        ctx[savor::context::key::battle::CTX_BLOB] = blob;
    }
} // namespace savor
