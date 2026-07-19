#include "PhaseScriptVM.h"
#include <algorithm>

#include "../../Core/Memory/IKeyReader.h"
#include "../../Core/Memory/Soa/Battle/DerivedBattleBuffer.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../IPC/Wire.h"
#include "ScriptProgress.h"
#include <thread>


namespace savor {
    PhaseScriptVM::PhaseScriptVM(savor::DolphinWrapper& host, const BreakpointMap& bpmap)
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

    bool PhaseScriptVM::configure_capture_from_context(const PSContext& ctx, PSResult& result) {
        std::string profile_path;
        std::string output_path;
        const bool has_profile = ctx.get<std::string>(savor::context::key::core::CAPTURE_PROFILE_PATH, profile_path);
        const bool has_output = ctx.get<std::string>(savor::context::key::core::CAPTURE_OUTPUT_PATH, output_path);
        uint32_t progress_flags = 0;
        ctx.get<uint32_t>(savor::context::key::core::PROGRESS_CORE_FLAGS, progress_flags);

        std::vector<uint32_t> denied_profile_pcs;
        denied_profile_pcs.reserve(bpmap_.addrs.size());
        for (const auto& entry : bpmap_.addrs) {
            if (entry.visibility != BreakpointVisibility::Internal || entry.pc == 0)
                continue;
            if (std::find(denied_profile_pcs.begin(), denied_profile_pcs.end(), entry.pc)
                == denied_profile_pcs.end()) {
                denied_profile_pcs.push_back(entry.pc);
            }
        }

        std::string error;
        if (!host_.startProbeJob(
            has_profile ? std::filesystem::path(profile_path) : std::filesystem::path{},
            has_output ? std::filesystem::path(output_path) : std::filesystem::path{},
            progress_flags,
            std::move(denied_profile_pcs),
            &error)) {
            result.ctx = ctx;
            SCLOGW("[probe] start failed profile=%s output=%s error=%s",
                profile_path.c_str(), output_path.c_str(), error.c_str());
            return false;
        }
        SCLOGI("[probe] active profile=%s output=%s battle_progress=%u",
            profile_path.c_str(),
            output_path.c_str(),
            (progress_flags & static_cast<uint32_t>(CoreProgressFlags::BattleProgress)) != 0 ? 1u : 0u);
        return true;
    }

    bool PhaseScriptVM::init(const PSInit& init, const PhaseScript& program)
    {
        init_ = init;
        prog_ = program;
        host_.clearMemoryWatchpoints();

        switch (init_.derived_buffer_type) {
        case DK_Battle: derived_ = std::make_unique<savor::DerivedBattleBuffer>(); break;
            // case 2: derived_ = std::make_unique<savor::DerivedExploreBuffer>(); break; // future
        default: derived_.reset(); break;
        }

        SCLOGDX(SC_TAGS("vm", "init"), "[VM] init begin sav=%s timeout=%u", init.savestate_path.c_str(), init.default_timeout_ms);

        // Disarm any previously armed set (enables program swapping)
        if (armed_ && !armed_pcs_.empty()) {
            macro_breakpoint_scope_active_ = false;
            macro_enabled_bp_keys_.clear();
            host_.disarmPcBreakpoints(armed_pcs_);
            armed_pcs_.clear();
        }
        armed_ = false;

        // Optional savestate (allow empty path for boot-based phases)
        if (!init_.savestate_path.empty()) {
            if (!host_.loadSavestate(init_.savestate_path.c_str()))
                return false;
        }

        // Update BP keys and arm once. Gated keys stay disabled unless a specialized op enables them.
        canonical_bp_keys_ = prog_.canonical_bp_keys;
        gated_bp_keys_ = prog_.gated_bp_keys;

        SCLOGDX(
            SC_TAGS("vm", "breakpoint"),
            "[VM] attach bp count=%zu gated=%zu",
            program.canonical_bp_keys.size(),
            program.gated_bp_keys.size());
        arm_bps_once();

        // Capture a snapshot to use as the per-job baseline
        const bool snapshot_ok = save_snapshot();
        if (snapshot_ok) SCLOGDX(SC_TAGS("vm", "init"), "[VM] init ok");
        return snapshot_ok;
    }

    PhaseScriptVM::DispatchResult PhaseScriptVM::dispatch_op(
        const PSOp& op,
        PSContext& ctx,
        PSResult& result,
        KeyHostRouter& router,
        const std::unordered_map<std::string, size_t>& label_vm_pc_map,
        size_t& vm_pc,
        std::string& section)
    {
#pragma warning(push)
#pragma warning(error : 4062)
        switch (op.code) {
        case PSOpCode::ARM_PHASE_BPS_ONCE:
            return op_arm_phase_bps_once() ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::LOAD_SNAPSHOT:
            return op_load_snapshot(ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::CAPTURE_SNAPSHOT:
            return op_capture_snapshot() ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::REBOOT_CORE:
            return op_reboot_core(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::APPLY_INPUT_FROM:
            return op_apply_input_from(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::STEP_FRAMES:
            op_step_frames(op);
            return DispatchResult::Continue;
        case PSOpCode::RUN_UNTIL_BP:
            op_run_until_bp(ctx);
            return DispatchResult::Continue;
        case PSOpCode::RUN_UNTIL_BP_KEY:
            return op_run_until_bp_key(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::RECORD_CURRENT_BP:
            op_record_current_bp(ctx);
            return DispatchResult::Continue;
        case PSOpCode::SET_TIMEOUT:
            op_set_timeout(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::SET_TIMEOUT_FROM:
            op_set_timeout_from(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::START_DETERMINISTIC_RUN:
            op_start_deterministic_run();
            return DispatchResult::Continue;
        case PSOpCode::END_DETERMINISTIC_RUN:
            op_end_deterministic_run();
            return DispatchResult::Continue;
        case PSOpCode::READ_U8:
            return op_read_u8(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::READ_U16:
            return op_read_u16(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::READ_U32:
            return op_read_u32(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::WRITE_U32:
            return op_write_u32(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::READ_F32:
            return op_read_f32(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::READ_F64:
            return op_read_f64(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::GET_BATTLE_CONTEXT:
            op_get_battle_context(result, ctx);
            return DispatchResult::Continue;
        case PSOpCode::EMIT_RESULT:
            op_emit_result(op, result, ctx);
            return DispatchResult::Continue;
        case PSOpCode::GC_SLOT_A_SET_FROM:
            SCLOGE("[VM] unsupported opcode: GC_SLOT_A_SET_FROM (21)");
            ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
            result.ctx = ctx;
            result.ok = false;
            return DispatchResult::Failed;
        case PSOpCode::MOVIE_PLAY_FROM:
            return op_movie_play_from(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::MOVIE_STOP:
            op_movie_stop();
            return DispatchResult::Continue;
        case PSOpCode::SAVE_SAVESTATE_FROM:
            return op_save_savestate_from(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::REQUIRE_DISC_GAMEID_FROM:
            return op_require_disc_gameid_from(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::LABEL:
            op_label();
            return DispatchResult::Continue;
        case PSOpCode::GOTO:
            op_goto(op, label_vm_pc_map, vm_pc, section);
            return DispatchResult::Continue;
        case PSOpCode::GOTO_IF:
            op_goto_if(op, ctx, label_vm_pc_map, vm_pc, section);
            return DispatchResult::Continue;
        case PSOpCode::GOTO_IF_KEYS:
            op_goto_if_keys(op, ctx, label_vm_pc_map, vm_pc, section);
            return DispatchResult::Continue;
        case PSOpCode::RETURN_RESULT:
            return op_return_result(op, result, ctx) ? DispatchResult::Returned : DispatchResult::Failed;
        case PSOpCode::CAPTURE_PRED_BASELINES:
            op_capture_pred_baselines(ctx, router);
            return DispatchResult::Continue;
        case PSOpCode::EVAL_PREDICATES_AT_HIT_BP:
            op_eval_predicates_at_hit_bp(ctx, router);
            return DispatchResult::Continue;
        case PSOpCode::ARM_BPS_FROM_PRED_TABLE:
            return op_arm_bps_from_pred_table(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::SET_U32:
            op_set_u32(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::ADD_U32:
            op_add_u32(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES:
            op_apply_battle_inputplan_frames(ctx);
            return DispatchResult::Continue;
        case PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH:
            op_build_turn_inputplan_from_battle_path(ctx);
            return DispatchResult::Continue;
        case PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS:
            op_materialize_battle_macro_steps(ctx);
            return DispatchResult::Continue;
        case PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS:
            op_materialize_battle_turn_macro_steps(ctx);
            return DispatchResult::Continue;
        case PSOpCode::EXECUTE_BATTLE_MACRO_STEP:
            op_execute_battle_macro_step(ctx);
            return DispatchResult::Continue;
        case PSOpCode::RECORD_TAS_INPUT_SAMPLE:
            op_record_tas_input_sample(ctx);
            return DispatchResult::Continue;
        case PSOpCode::STEP_OPCODE:
            op_step_opcode(op);
            return DispatchResult::Continue;
        case PSOpCode::ARM_MEMORY_WATCHPOINT:
            return op_arm_memory_watchpoint(op, result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::CLEAR_MEMORY_WATCHPOINTS:
            op_clear_memory_watchpoints();
            return DispatchResult::Continue;
        case PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS:
            return op_arm_capture_memory_watchpoints(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::RUN_UNTIL_DEBUG_STOP:
            op_run_until_debug_stop(ctx);
            return DispatchResult::Continue;
        case PSOpCode::CAPTURE_SEED_OVERRIDE:
            return op_capture_seed_override(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::Count:
            break;
        }
#pragma warning(pop)

        SCLOGE("[VM] invalid opcode ordinal: %u", static_cast<unsigned>(op.code));
        ctx[savor::context::key::core::WORKER_ERROR] = static_cast<uint32_t>(WERR_UnknownError);
        result.ctx = ctx;
        result.ok = false;
        return DispatchResult::Failed;
    }

    PSResult PhaseScriptVM::run(const PSJob& job)
    {
        PSResult R{};
        auto ctx_heap = std::make_unique<PSContext>(job.ctx);
        PSContext& ctx = *ctx_heap;
        struct MemoryWatchpointRunScope {
            DolphinWrapper& host;
            ~MemoryWatchpointRunScope() { host.clearMemoryWatchpoints(); }
        } memory_watchpoint_scope{ host_ };
        host_.clearMemoryWatchpoints();
        predicate_bp_keys_.clear();
        std::string _section = "Entry Point";

        if (!configure_capture_from_context(ctx, R)) {
            return R;
        }
        struct ProbeJobRunScope {
            DolphinWrapper& host;
            ~ProbeJobRunScope() { host.stopProbeJob(); }
        } probe_job_scope{ host_ };

        // Restore the job baseline while the probe layer is active so the epoch marker is durable.
        if (!load_snapshot()) return R;

        ctx[savor::context::key::core::VI_FIRST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);

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

            switch (dispatch_op(op, ctx, R, *router, label_vm_pc_map, vm_pc, _section)) {
            case DispatchResult::Continue:
                break;
            case DispatchResult::Returned:
            case DispatchResult::Failed:
                return R;
            }
        }
        ctx[savor::context::key::core::VI_LAST] = (uint32_t)(host_.getViFieldCountApprox() & 0xFFFFFFFFull);
        R.ctx = ctx;
        R.ok = true;
        return R;
    }

} // namespace savor
