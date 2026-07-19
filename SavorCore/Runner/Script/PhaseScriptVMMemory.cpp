#include "PhaseScriptVM.h"

#include "../IPC/Wire.h"
#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"

namespace savor {
    bool PhaseScriptVM::op_read_u8(const PSOp& op, PSResult&, PSContext& ctx) { uint8_t v{}; if (!read_u8(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u16(const PSOp& op, PSResult&, PSContext& ctx) { uint16_t v{}; if (!read_u16(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_u32(const PSOp& op, PSResult&, PSContext& ctx) { uint32_t v{}; if (!read_u32(op.rd.addr, v)) { SCLOGD("[VM] READ_U32 FAIL @%08X key=%s", op.rd.addr, savor::context::key::name_for_id(op.rd.dst).data()); return false; } SCLOGD("[VM] READ_U32 @%08X -> %08X key=%s", op.rd.addr, v, savor::context::key::name_for_id(op.rd.dst).data()); ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_write_u32(const PSOp& op, PSResult&, PSContext& ctx) {
        uint32_t value = 0;
        uint32_t readback = 0;
        ctx[savor::context::key::core::MEMWRITE_ADDR] = op.rd.addr;
        ctx[savor::context::key::core::MEMWRITE_VALUE] = 0u;
        ctx[savor::context::key::core::MEMWRITE_READBACK] = 0u;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = 0u;

        if (!ctx.get<uint32_t>(op.rd.dst, value)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 2u;
            return true;
        }
        ctx[savor::context::key::core::MEMWRITE_VALUE] = value;

        if (!host_.writeU32(op.rd.addr, value)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 3u;
            return true;
        }
        if (!host_.readU32(op.rd.addr, readback)) {
            ctx[savor::context::key::core::MEMWRITE_STATUS] = 4u;
            return true;
        }
        ctx[savor::context::key::core::MEMWRITE_READBACK] = readback;
        ctx[savor::context::key::core::MEMWRITE_STATUS] = readback == value ? 1u : 5u;
        return true;
    }
    bool PhaseScriptVM::op_read_f32(const PSOp& op, PSResult&, PSContext& ctx) { float v{}; if (!read_f32(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }
    bool PhaseScriptVM::op_read_f64(const PSOp& op, PSResult&, PSContext& ctx) { double v{}; if (!read_f64(op.rd.addr, v)) return false; ctx[op.rd.dst] = v; return true; }

    bool PhaseScriptVM::op_arm_memory_watchpoint(const PSOp& op, PSResult& result, PSContext& ctx) {
        uint32_t address = op.memwatch.address;
        if (op.memwatch.use_address_key != 0) {
            if (!ctx.get<uint32_t>(op.memwatch.address_key, address)) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
                result.ctx = ctx;
                return false;
            }
        }

        const auto access = static_cast<DolphinWrapper::MemoryWatchpointAccess>(
            static_cast<uint32_t>(op.memwatch.access));
        if (access != DolphinWrapper::MemoryWatchpointAccess::Read
            && access != DolphinWrapper::MemoryWatchpointAccess::Write
            && access != DolphinWrapper::MemoryWatchpointAccess::Access) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            return false;
        }

        const DolphinWrapper::MemoryWatchpointSpec spec{
            .id = op.memwatch.id,
            .address = address,
            .size = op.memwatch.size,
            .access = access,
        };
        if (!host_.armMemoryWatchpoints({ spec })) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            return false;
        }
        return true;
    }
    void PhaseScriptVM::op_clear_memory_watchpoints() const {
        host_.clearMemoryWatchpoints();
    }
    bool PhaseScriptVM::op_capture_seed_override(PSResult&, PSContext& ctx) {
        uint32_t original_seed = 0;
        uint32_t override_seed = 0;
        uint32_t applied_seed = 0;
        ctx.get<uint32_t>(savor::context::key::battle::RNG_ORIGINAL_SEED, original_seed);
        ctx.get<uint32_t>(savor::context::key::battle::RNG_OVERRIDE_SEED, override_seed);
        ctx.get<uint32_t>(savor::context::key::battle::RNG_APPLIED_SEED, applied_seed);
        host_.emitProbeMarker("rng.seed_override.original", original_seed);
        host_.emitProbeMarker("rng.seed_override.requested", override_seed);
        host_.emitProbeMarker("rng.seed_override.applied", applied_seed);
        return true;
    }
    bool PhaseScriptVM::op_arm_capture_memory_watchpoints(PSResult&, PSContext&) {
        // Probe profile memory subscriptions are active for the full job scope.
        host_.emitProbeMarker("capture.memory_probes.active");
        return true;
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
