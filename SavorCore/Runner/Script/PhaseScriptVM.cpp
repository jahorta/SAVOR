#include "PhaseScriptVM.h"
#include <algorithm>

#include "../../Core/Memory/IKeyReader.h"
#include "../../Core/Memory/Soa/Battle/DerivedBattleBuffer.h"
#include "../../Core/Memory/KeyHostRouter.h"
#include "../InputMacro/InputMacroRuntime.h"
#include "../IPC/Wire.h"
#include "ScriptProgress.h"
#include <thread>


namespace savor {
    PhaseScriptVM::PhaseScriptVM(savor::DolphinWrapper& host, const BreakpointMap& bpmap)
        : host_(host),
          bpmap_(bpmap),
          input_macro_runtime_(std::make_unique<inputmacro::InputMacroRuntime>(
              static_cast<inputmacro::IInputMacroHost&>(*this))) {
    }

    PhaseScriptVM::~PhaseScriptVM()
    {
        cancel_input_macro();
    }

    void PhaseScriptVM::cancel_input_macro()
    {
        if (input_macro_plan_driver_) input_macro_plan_driver_->Cancel();
        if (input_macro_runtime_) input_macro_runtime_->Cancel();
        input_macro_plan_driver_.reset();
        input_macro_context_sink_ = InputMacroContextSink::None;
        active_input_macro_context_ = nullptr;
        input_macro_stop_sequence_ = 0;
        current_input_macro_stop_ = {};
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

    bool PhaseScriptVM::fail_legacy_service(
        PSResult& result,
        PSContext& ctx,
        const char* diagnostic) const
    {
        ctx[savor::context::key::core::WORKER_ERROR] =
            static_cast<std::uint32_t>(WERR_UnknownError);
        result.ctx = ctx;
        result.ok = false;
        SCLOGE("%s", diagnostic);
        return false;
    }

    bool PhaseScriptVM::configure_capture_from_context(
        PSContext& ctx,
        PSResult& result)
    {
        return fail_legacy_service(
            result,
            ctx,
            "[VM] capture attachment is disconnected; use the session-owned CaptureService");
    }

    bool PhaseScriptVM::init(const PSInit& init, const PhaseScript& program)
    {
        cancel_input_macro();
        init_ = init;
        prog_ = program;

        switch (init_.derived_buffer_type) {
        case DK_Battle: derived_ = std::make_unique<savor::DerivedBattleBuffer>(); break;
            // case 2: derived_ = std::make_unique<savor::DerivedExploreBuffer>(); break; // future
        default: derived_.reset(); break;
        }

        SCLOGDX(
            SC_TAGS("vm", "init"),
            "[VM] legacy init rejected sav=%s timeout=%u",
            init.savestate_path.c_str(),
            init.default_timeout_ms);

        armed_pcs_.clear();
        armed_ = false;
        canonical_bp_keys_ = prog_.canonical_bp_keys;
        gated_bp_keys_ = prog_.gated_bp_keys;
        predicate_bp_keys_.clear();
        SCLOGE(
            "[VM] PhaseScriptVM initialization is disconnected; use ProgramRuntime and session services");
        return false;
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
            return op_load_snapshot(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::CAPTURE_SNAPSHOT:
            return op_capture_snapshot(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
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
        case PSOpCode::RECORD_CURRENT_PC_TO:
            return op_record_current_pc_to(op, result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
        case PSOpCode::SET_TIMEOUT:
            op_set_timeout(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::SET_TIMEOUT_FROM:
            op_set_timeout_from(op, ctx);
            return DispatchResult::Continue;
        case PSOpCode::START_DETERMINISTIC_RUN:
            return op_start_deterministic_run(result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
        case PSOpCode::END_DETERMINISTIC_RUN:
            return op_end_deterministic_run(result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
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
            return op_movie_stop(result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
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
            return op_clear_memory_watchpoints(result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
        case PSOpCode::ARM_CAPTURE_MEMORY_WATCHPOINTS:
            return op_arm_capture_memory_watchpoints(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::RUN_UNTIL_DEBUG_STOP:
            op_run_until_debug_stop(ctx);
            return DispatchResult::Continue;
        case PSOpCode::CAPTURE_SEED_OVERRIDE:
            return op_capture_seed_override(result, ctx) ? DispatchResult::Continue : DispatchResult::Failed;
        case PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS:
            op_materialize_battle_results_screen_macro_steps(ctx);
            return DispatchResult::Continue;
        case PSOpCode::MATERIALIZE_BATTLE_COMPLETION_MACRO_STEPS:
            op_materialize_battle_completion_macro_steps(ctx);
            return DispatchResult::Continue;
        case PSOpCode::GET_NAVIGATION_CONTEXT:
            return op_get_navigation_context(result, ctx)
                ? DispatchResult::Continue
                : DispatchResult::Failed;
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
        cancel_input_macro();
        PSResult R{};
        auto ctx_heap = std::make_unique<PSContext>(job.ctx);
        PSContext& ctx = *ctx_heap;
        // Declare this after the context so its deleter runs first. Macro
        // cleanup currently does not dereference active_input_macro_context_,
        // but keeping that pointer live through cancellation makes the job-exit
        // ownership boundary explicit and robust to future host cleanup work.
        const auto input_macro_run_scope = std::shared_ptr<void>(
            nullptr,
            [this](void*) { cancel_input_macro(); });
        predicate_bp_keys_.clear();
        std::string _section = "Entry Point";

        if (!configure_capture_from_context(ctx, R)) {
            return R;
        }
        // A correctly initialized legacy VM cannot reach this point after the
        // hard cutover. Keep the interpreter source compiled for translation
        // evidence, but reject state restoration before touching Dolphin.
        if (!op_load_snapshot(R, ctx)) return R;

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
