#include "PhaseScriptVM.h"

#include "../InputMacro/InputMacroRuntime.h"
#include "../IPC/Wire.h"
#include "../../Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace savor {
namespace {

const BPAddr* FindHitBreakpoint(
    const BreakpointMap& bpmap,
    const std::vector<BPKey>& canonical_keys,
    const std::vector<BPKey>& gated_keys,
    const std::vector<BPKey>& predicate_keys,
    std::uint32_t pc)
{
    const auto find_in = [&](const std::vector<BPKey>& keys) -> const BPAddr* {
        for (const auto key : keys) {
            if (const auto* entry = bpmap.find(key); entry != nullptr && entry->pc == pc)
                return entry;
        }
        return nullptr;
    };
    if (const auto* entry = find_in(canonical_keys)) return entry;
    if (const auto* entry = find_in(gated_keys)) return entry;
    return find_in(predicate_keys);
}

std::string BreakpointListDescription(std::span<const BPKey> keys)
{
    std::ostringstream out;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) out << ',';
        out << static_cast<std::uint32_t>(keys[index]);
    }
    return out.str();
}

void ResetBattleMacroContext(PSContext& ctx)
{
    using namespace savor::context::key;
    ctx[battle::MACRO_RESULT] = 1u;
    ctx[battle::MACRO_FAILURE_CODE] = 0u;
    ctx[battle::MACRO_STEP_COUNT] = 0u;
    ctx[battle::MACRO_LAST_STEP_INDEX] = 0u;
    ctx[battle::MACRO_LAST_EXPECTED_BP] = 0u;
    ctx[battle::MACRO_LAST_HIT_BP] = 0u;
    ctx[battle::MACRO_LAST_HIT_PC] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;
    ctx[battle::MACRO_MEMORY_ADDR] = 0u;
    ctx[battle::MACRO_MEMORY_BASELINE] = 0u;
    ctx[battle::MACRO_MEMORY_LATEST] = 0u;
    ctx[battle::MACRO_MEMORY_CHANGED] = 0u;
    ctx[battle::MACRO_MEMORY_POLL_COUNT] = 0u;
    ctx[battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
    ctx[battle::MACRO_MEMORY_GATE_COUNT] = 0u;
    ctx[battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
    ctx[battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
    ctx[battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
    ctx[battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
    ctx[battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
    ctx[battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
}

phase::battle::macroprobe::FailureCode LegacyFailureFor(
    inputmacro::InputMacroFailure failure)
{
    using Legacy = phase::battle::macroprobe::FailureCode;
    using Failure = inputmacro::InputMacroFailure;
    switch (failure) {
    case Failure::None:
        return Legacy::Ok;
    case Failure::BreakpointTimeout:
    case Failure::MemoryTimeout:
        return Legacy::Timeout;
    case Failure::UnexpectedBreakpoint:
    case Failure::Cancelled:
        return Legacy::UnexpectedBreakpoint;
    case Failure::MemoryReadFailed:
    case Failure::MissingBaseline:
        return Legacy::MemoryReadFailed;
    case Failure::NotRunning:
    case Failure::EmptyPlan:
    case Failure::EmptyExpectedKeys:
    case Failure::InvalidAddress:
    case Failure::InvalidTimeout:
    case Failure::InvalidBaselineReference:
    case Failure::UndeclaredBreakpoint:
    case Failure::UnauthorizedBreakpoint:
    case Failure::SessionUnavailable:
    case Failure::HostFailure:
        return Legacy::InvalidMode;
    }
    return Legacy::InvalidMode;
}

void ApplyRuntimeOutcome(
    inputmacro::InputMacroFailure failure,
    PSContext& ctx)
{
    using Failure = inputmacro::InputMacroFailure;
    if (failure == Failure::BreakpointTimeout || failure == Failure::MemoryTimeout) {
        ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
            static_cast<std::uint32_t>(RunToBpOutcome::Timeout);
    } else if (failure == Failure::Cancelled) {
        ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
            static_cast<std::uint32_t>(RunToBpOutcome::Aborted);
    } else if (failure != Failure::None) {
        ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
            static_cast<std::uint32_t>(RunToBpOutcome::InputPlaybackFailed);
    }
}

} // namespace

bool PhaseScriptVM::acquire_exclusive_session(std::span<const BPKey> provider_keys)
{
    if (input_macro_session_active_) return false;
    for (const auto key : provider_keys) {
        const auto* entry = bpmap_.find(key);
        if (entry == nullptr
            || !bp::BpRegistry::IsAllowed(key, BreakpointConsumer::InputMacroControl)) {
            SCLOGE("[input-macro] session rejected unavailable provider key=%u",
                static_cast<std::uint32_t>(key));
            return false;
        }
    }

    input_macro_provider_keys_.assign(provider_keys.begin(), provider_keys.end());
    input_macro_enabled_bp_keys_.clear();
    host_.setEnabledPcBreakpointsOnly({});
    input_macro_session_active_ = true;
    host_.emitProbeMarker("macro.begin");
    SCLOGI("[input-macro] exclusive session acquired keys=%zu", provider_keys.size());
    return true;
}

void PhaseScriptVM::release_exclusive_session()
{
    if (!input_macro_session_active_) return;
    host_.setEnabledPcBreakpointsOnly({});
    input_macro_enabled_bp_keys_.clear();
    input_macro_provider_keys_.clear();
    input_macro_session_active_ = false;
    host_.emitProbeMarker("macro.end");
    SCLOGI("[input-macro] exclusive session released");
}

inputmacro::BreakpointWaitResult PhaseScriptVM::run_to_breakpoints(
    const inputmacro::BreakpointWaitAction& action)
{
    inputmacro::BreakpointWaitResult result{};
    if (!input_macro_session_active_
        || active_input_macro_context_ == nullptr
        || action.expected_keys.empty()) {
        return result;
    }

    std::vector<std::uint32_t> enabled_pcs;
    enabled_pcs.reserve(action.expected_keys.size());
    for (const auto key : action.expected_keys) {
        if (std::find(input_macro_provider_keys_.begin(), input_macro_provider_keys_.end(), key)
                == input_macro_provider_keys_.end()) {
            return result;
        }
        const auto* entry = bpmap_.find(key);
        if (entry == nullptr || entry->pc == 0) return result;
        if (std::find(enabled_pcs.begin(), enabled_pcs.end(), entry->pc) == enabled_pcs.end())
            enabled_pcs.push_back(entry->pc);
    }

    input_macro_enabled_bp_keys_ = action.expected_keys;
    host_.setEnabledPcBreakpointsOnly(enabled_pcs);
    host_.emitProbeMarker(
        "macro.step.begin",
        static_cast<std::uint32_t>(input_macro_runtime_->current_index()));

    const auto run = run_until_bp_core(*active_input_macro_context_, RunUntilBpSpec{
        .expected_bp_keys = action.expected_keys,
        .input = action.input,
        .apply_input = true,
        .release_input = true,
        .hold_input_through_hit_opcode = action.hold_input_through_hit_opcode,
        .step_off_current_bp = true,
        .expected_only_scope = false,
        .watch_movie = false,
        .include_gated_hit_lookup = true,
        .update_derived = false,
    });

    host_.setInput(GCInputFrame{});
    host_.setEnabledPcBreakpointsOnly({});
    input_macro_enabled_bp_keys_.clear();
    host_.emitProbeMarker(
        "macro.step.end",
        static_cast<std::uint32_t>(input_macro_runtime_->current_index()));

    result.hit = run.run.hit;
    result.hit_key = static_cast<BPKey>(run.hit_bp_key);
    result.hit_pc = run.run.hit ? static_cast<std::uint32_t>(run.run.pc) : 0u;
    result.elapsed_ms = run.elapsed_ms;
    if (run.run.hit && run.expected_match) {
        result.status = inputmacro::InputMacroHostStatus::Succeeded;
        if (derived_) derived_->update_on_bp(run.hit_bp_key, *active_input_macro_context_, host_);
    } else if (run.outcome == RunToBpOutcome::Aborted) {
        result.status = inputmacro::InputMacroHostStatus::Cancelled;
    } else if (run.run.hit) {
        // The runtime converts a successful-but-unexpected hit into its
        // dedicated UnexpectedBreakpoint failure.
        result.status = inputmacro::InputMacroHostStatus::Succeeded;
    } else {
        // The legacy battle adapter classified every non-aborted no-hit
        // outcome as a timeout, including watchdog and movie-stop variants.
        result.status = inputmacro::InputMacroHostStatus::TimedOut;
    }

    SCLOGI("[input-macro] wait expected=%s hit=%u pc=%08X status=%u",
        BreakpointListDescription(action.expected_keys).c_str(),
        static_cast<std::uint32_t>(result.hit_key),
        result.hit_pc,
        static_cast<std::uint32_t>(result.status));
    return result;
}

inputmacro::InputMacroHostStatus PhaseScriptVM::step_neutral_frames(
    std::uint32_t frame_count)
{
    if (!input_macro_session_active_ || active_input_macro_context_ == nullptr)
        return inputmacro::InputMacroHostStatus::Failed;

    host_.clearMemoryWatchpoints();
    host_.setInput(GCInputFrame{});
    host_.setEnabledPcBreakpointsOnly({});
    for (std::uint32_t frame = 0; frame < frame_count; ++frame) {
        if (!host_.stepOneFrameBlocking())
            return inputmacro::InputMacroHostStatus::Failed;
    }
    if (derived_) derived_->update_on_bp(0u, *active_input_macro_context_, host_);
    return inputmacro::InputMacroHostStatus::Succeeded;
}

bool PhaseScriptVM::read_u32(std::uint32_t address, std::uint32_t& value)
{
    // Baseline capture must not inherit an unrelated probe watchpoint from a
    // previous VM operation. This preserves the old battle-macro capture
    // invariant while keeping the generic runtime host-neutral.
    host_.clearMemoryWatchpoints();
    return host_.readU32(address, value);
}

inputmacro::MemoryChangeResult PhaseScriptVM::wait_for_u32_change(
    std::uint32_t address,
    std::uint32_t baseline,
    std::uint32_t timeout_ms)
{
    inputmacro::MemoryChangeResult result{.latest_value = baseline};
    if (!input_macro_session_active_ || active_input_macro_context_ == nullptr)
        return result;

    host_.clearMemoryWatchpoints();
    host_.setInput(GCInputFrame{});
    host_.setEnabledPcBreakpointsOnly({});
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        if (!host_.readU32(address, result.latest_value)) {
            result.status = inputmacro::InputMacroHostStatus::ReadFailed;
            break;
        }
        if (result.latest_value != baseline) {
            result.status = inputmacro::InputMacroHostStatus::Succeeded;
            break;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_ms) {
            result.status = inputmacro::InputMacroHostStatus::TimedOut;
            break;
        }
        if (!host_.stepOneFrameBlocking()) {
            result.status = inputmacro::InputMacroHostStatus::Failed;
            break;
        }
        ++result.poll_count;
    }
    result.elapsed_ms = static_cast<std::uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count());
    return result;
}

void PhaseScriptVM::set_neutral_input()
{
    host_.setInput(GCInputFrame{});
}

void PhaseScriptVM::clear_macro_memory_watchpoints()
{
    host_.clearMemoryWatchpoints();
}

void PhaseScriptVM::restore_breakpoint_state()
{
    restore_canonical_breakpoint_scope();
}

BPKey PhaseScriptVM::current_breakpoint_key() const
{
    const auto* entry = FindHitBreakpoint(
        bpmap_,
        canonical_bp_keys_,
        gated_bp_keys_,
        predicate_bp_keys_,
        host_.getPC());
    return entry != nullptr ? entry->key : BPKey{0};
}

inputmacro::BattleCommandProviderWaitResult PhaseScriptVM::wait_for_breakpoints(
    std::span<const BPKey> expected_keys)
{
    inputmacro::BattleCommandProviderWaitResult result{};
    if (active_input_macro_context_ == nullptr || expected_keys.empty()) return result;

    const auto run = run_until_bp_core(*active_input_macro_context_, RunUntilBpSpec{
        .expected_bp_keys = std::vector<BPKey>(expected_keys.begin(), expected_keys.end()),
        .input = GCInputFrame{},
        .apply_input = true,
        .release_input = true,
        .step_off_current_bp = true,
        .expected_only_scope = true,
        .watch_movie = false,
        .include_gated_hit_lookup = true,
        .update_derived = false,
    });
    host_.setInput(GCInputFrame{});
    result.hit = run.run.hit;
    result.hit_key = static_cast<BPKey>(run.hit_bp_key);
    result.hit_pc = run.run.hit ? static_cast<std::uint32_t>(run.run.pc) : 0u;
    return result;
}

bool PhaseScriptVM::capture_mem1(std::string& out_mem1)
{
    return host_.getMem1(out_mem1);
}

void PhaseScriptVM::start_prepared_battle_macro(
    inputmacro::BattleCommandInputMacroProvider::PrepareResult prepared,
    PSContext& ctx,
    bool authored_turn)
{
    using FailureCode = phase::battle::macroprobe::FailureCode;
    const auto step_count = static_cast<std::uint32_t>(prepared.plan.steps.size());
    ctx[context::key::battle::MACRO_LAST_EXPECTED_BP] =
        static_cast<std::uint32_t>(prepared.last_expected_key);
    ctx[context::key::battle::MACRO_LAST_HIT_BP] =
        static_cast<std::uint32_t>(prepared.last_hit_key);
    ctx[context::key::battle::MACRO_LAST_HIT_PC] = prepared.last_hit_pc;
    if (authored_turn) {
        ctx[context::key::battle::PLAN_MATERIALIZE_ERR] =
            static_cast<std::uint32_t>(prepared.materialize_error);
    }
    ctx[context::key::battle::MACRO_STEP_COUNT] = step_count;
    ctx[context::key::battle::MACRO_FAILURE_CODE] =
        static_cast<std::uint32_t>(prepared.failure);

    if (!prepared.ok()) {
        if (prepared.failure == FailureCode::Timeout) {
            ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<std::uint32_t>(RunToBpOutcome::Timeout);
        } else if (prepared.failure == FailureCode::UnexpectedBreakpoint
            || prepared.failure == FailureCode::BattleContextUnavailable
            || !authored_turn) {
            ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<std::uint32_t>(RunToBpOutcome::InputPlaybackFailed);
        }
        SCLOGW("[battle-input-macro] preparation failed code=%u materialize=%u detail=%s",
            static_cast<std::uint32_t>(prepared.failure),
            static_cast<std::uint32_t>(prepared.materialize_error),
            prepared.diagnostic.c_str());
        return;
    }

    const auto required_keys =
        inputmacro::BattleCommandInputMacroProvider::required_breakpoint_keys();
    const auto start = input_macro_runtime_->Replace(std::move(prepared.plan), required_keys);
    if (start.failure != inputmacro::InputMacroFailure::None) {
        const auto legacy_failure = LegacyFailureFor(start.failure);
        ctx[context::key::battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(legacy_failure);
        ApplyRuntimeOutcome(start.failure, ctx);
        SCLOGW("[battle-input-macro] runtime start failed failure=%u detail=%s",
            static_cast<std::uint32_t>(start.failure),
            start.diagnostic.c_str());
        return;
    }

    ctx[context::key::battle::MACRO_FAILURE_CODE] =
        static_cast<std::uint32_t>(FailureCode::Ok);
    SCLOGI("[battle-input-macro] prepared steps=%u context='%s'",
        step_count,
        phase::battle::macroprobe::FormatPlanningContext(prepared.planning_context).c_str());
}

void PhaseScriptVM::op_materialize_battle_macro_steps(PSContext& ctx)
{
    using namespace phase::battle::macroprobe;

    cancel_input_macro();
    active_input_macro_context_ = &ctx;
    ResetBattleMacroContext(ctx);

    std::uint32_t raw_mode = 0;
    std::uint32_t target_slot = 4;
    std::uint32_t transition_neutral_frames = 3;
    std::string plan_blob;
    ctx.get(context::key::battle::MACRO_MODE, raw_mode);
    ctx.get(context::key::battle::MACRO_TARGET_SLOT, target_slot);
    ctx.get(context::key::battle::MACRO_PLAN_BLOB, plan_blob);
    ctx.get(context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);

    std::vector<MacroCommand> commands;
    if (!plan_blob.empty()) {
        std::string parse_error;
        if (!DeserializeCommandPlan(plan_blob, &commands, &parse_error)) {
            ctx[context::key::battle::MACRO_FAILURE_CODE] =
                static_cast<std::uint32_t>(FailureCode::InvalidMode);
            ctx[context::key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<std::uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            SCLOGW("[battle-input-macro] invalid plan '%s': %s",
                plan_blob.c_str(), parse_error.c_str());
            return;
        }
    } else {
        commands.push_back(MacroCommand{
            .mode = static_cast<MacroMode>(raw_mode),
            .target_slot = target_slot,
        });
    }

    auto read_pattern = [&](context::key::KeyId mode_key,
                            context::key::KeyId target_neutral_key,
                            context::key::KeyId input_neutral_key,
                            context::key::KeyId timeout_key,
                            FakeAttackPattern defaults) {
        std::uint32_t raw_gate = static_cast<std::uint32_t>(defaults.memory_gate_mode);
        ctx.get(mode_key, raw_gate);
        ctx.get(target_neutral_key, defaults.target_neutral_before_b_frames);
        ctx.get(input_neutral_key, defaults.input_neutral_after_b_frames);
        ctx.get(timeout_key, defaults.memory_timeout_ms);
        defaults.memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_gate);
        return defaults;
    };

    const auto repeat_pattern = read_pattern(
        context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE,
        context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS,
        FakeAttackPattern{});
    const auto first_pattern = read_pattern(
        context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE,
        context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS,
        FakeAttackPattern{});
    const auto final_pattern = read_pattern(
        context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE,
        context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES,
        context::key::battle::MACRO_FAKE_FINAL_MEMORY_TIMEOUT_MS,
        FakeAttackPattern{});

    std::uint32_t fake_attack_count = 0;
    std::uint32_t mixed_patterns = 0;
    std::uint32_t final_pattern_enabled = 0;
    ctx.get(context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, fake_attack_count);
    ctx.get(context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS, mixed_patterns);
    ctx.get(context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN, final_pattern_enabled);

    inputmacro::BattleCommandInputMacroProvider::ProbeRequest request{
        .commands = std::move(commands),
        .transition_neutral_frames = transition_neutral_frames,
        .fake_attack_count = fake_attack_count,
        .fake_attack_pattern = repeat_pattern,
        .use_mixed_fake_attack_patterns = mixed_patterns != 0,
        .first_fake_attack_pattern = first_pattern,
        .use_final_fake_attack_pattern = final_pattern_enabled != 0,
        .final_fake_attack_pattern = final_pattern,
    };
    inputmacro::BattleCommandInputMacroProvider provider;
    start_prepared_battle_macro(provider.prepare(*this, request), ctx, false);
}

void PhaseScriptVM::op_materialize_battle_turn_macro_steps(PSContext& ctx)
{
    using FailureCode = phase::battle::macroprobe::FailureCode;
    using MaterializeErr = soa::battle::actions::MaterializeErr;

    cancel_input_macro();
    active_input_macro_context_ = &ctx;
    ResetBattleMacroContext(ctx);
    ctx[context::key::battle::PLAN_MATERIALIZE_ERR] =
        static_cast<std::uint32_t>(MaterializeErr::OK);

    std::uint32_t turn = 0;
    ctx.get(context::key::battle::ACTIVE_TURN, turn);
    if (turn == 0) {
        ctx[context::key::battle::PLAN_MATERIALIZE_ERR] =
            static_cast<std::uint32_t>(MaterializeErr::InvalidTurnIdxZero);
        ctx[context::key::battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(FailureCode::InvalidMode);
        return;
    }

    soa::battle::actions::BattlePath path;
    if (!ctx.get(context::key::battle::TURN_PLANS, path)) {
        ctx[context::key::battle::PLAN_MATERIALIZE_ERR] =
            static_cast<std::uint32_t>(MaterializeErr::BadBlob);
        ctx[context::key::battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(FailureCode::InvalidMode);
        return;
    }
    if (turn > path.size()) {
        ctx[context::key::battle::PLAN_MATERIALIZE_ERR] =
            static_cast<std::uint32_t>(MaterializeErr::OutOfTurns);
        ctx[context::key::battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(FailureCode::InvalidMode);
        return;
    }

    std::uint32_t transition_neutral_frames = 3;
    ctx.get(context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);
    inputmacro::BattleCommandInputMacroProvider::AuthoredTurnRequest request{
        .turn_plan = path[turn - 1],
        .transition_neutral_frames = transition_neutral_frames,
    };
    inputmacro::BattleCommandInputMacroProvider provider;
    start_prepared_battle_macro(provider.prepare(*this, request), ctx, true);
}

void PhaseScriptVM::apply_input_macro_step_result(
    const inputmacro::InputMacroStepResult& step_result,
    PSContext& ctx)
{
    using namespace context::key;
    const auto legacy_failure = LegacyFailureFor(step_result.failure);
    ctx[battle::MACRO_FAILURE_CODE] = static_cast<std::uint32_t>(legacy_failure);
    ctx[battle::MACRO_LAST_STEP_INDEX] =
        step_result.step_completed && input_macro_runtime_->active()
        ? static_cast<std::uint32_t>(input_macro_runtime_->current_index())
        : static_cast<std::uint32_t>(step_result.step_index);
    ctx[battle::MACRO_LAST_EXPECTED_BP] =
        static_cast<std::uint32_t>(step_result.expected_key);
    ctx[battle::MACRO_LAST_HIT_BP] = static_cast<std::uint32_t>(step_result.hit_key);
    ctx[battle::MACRO_LAST_HIT_PC] = step_result.hit_pc;
    ctx[battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
    ctx[battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;

    const bool completed_baseline_capture =
        step_result.action_kind == inputmacro::InputMacroActionKind::CaptureU32Baseline
        && step_result.step_completed;
    if (completed_baseline_capture
        || step_result.action_kind == inputmacro::InputMacroActionKind::WaitU32Change) {
        ctx[battle::MACRO_MEMORY_ADDR] = step_result.memory_address;
        ctx[battle::MACRO_MEMORY_BASELINE] = step_result.memory_baseline;
        ctx[battle::MACRO_MEMORY_LATEST] = step_result.memory_latest;
        ctx[battle::MACRO_MEMORY_CHANGED] = step_result.memory_changed ? 1u : 0u;
        ctx[battle::MACRO_MEMORY_POLL_COUNT] = step_result.memory_poll_count;
        ctx[battle::MACRO_MEMORY_ELAPSED_MS] = step_result.elapsed_ms;
    }

    if (step_result.action_kind == inputmacro::InputMacroActionKind::WaitU32Change) {
        std::uint32_t gate_count = 0;
        ctx.get(battle::MACRO_MEMORY_GATE_COUNT, gate_count);
        ctx[battle::MACRO_MEMORY_GATE_COUNT] = gate_count + 1u;
        const auto set_cycle = [&](context::key::KeyId baseline_key,
                                   context::key::KeyId latest_key,
                                   context::key::KeyId changed_key,
                                   context::key::KeyId polls_key,
                                   context::key::KeyId elapsed_key) {
            ctx[baseline_key] = step_result.memory_baseline;
            ctx[latest_key] = step_result.memory_latest;
            ctx[changed_key] = step_result.memory_changed ? 1u : 0u;
            ctx[polls_key] = step_result.memory_poll_count;
            ctx[elapsed_key] = step_result.elapsed_ms;
        };
        if (step_result.diagnostic_cycle_index == 0) {
            set_cycle(
                battle::MACRO_MEMORY_FIRST_BASELINE,
                battle::MACRO_MEMORY_FIRST_LATEST,
                battle::MACRO_MEMORY_FIRST_CHANGED,
                battle::MACRO_MEMORY_FIRST_POLL_COUNT,
                battle::MACRO_MEMORY_FIRST_ELAPSED_MS);
        } else if (step_result.diagnostic_cycle_index == 1) {
            set_cycle(
                battle::MACRO_MEMORY_REPEAT_BASELINE,
                battle::MACRO_MEMORY_REPEAT_LATEST,
                battle::MACRO_MEMORY_REPEAT_CHANGED,
                battle::MACRO_MEMORY_REPEAT_POLL_COUNT,
                battle::MACRO_MEMORY_REPEAT_ELAPSED_MS);
        } else if (step_result.diagnostic_cycle_index == 2) {
            set_cycle(
                battle::MACRO_MEMORY_REPEAT2_BASELINE,
                battle::MACRO_MEMORY_REPEAT2_LATEST,
                battle::MACRO_MEMORY_REPEAT2_CHANGED,
                battle::MACRO_MEMORY_REPEAT2_POLL_COUNT,
                battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS);
        }
    }

    ApplyRuntimeOutcome(step_result.failure, ctx);
    if (step_result.terminal_status == inputmacro::InputMacroTerminalStatus::Completed) {
        ctx[battle::MACRO_RESULT] = 0u;
        ctx[battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(phase::battle::macroprobe::FailureCode::Ok);
    }
}

void PhaseScriptVM::op_execute_battle_macro_step(PSContext& ctx)
{
    active_input_macro_context_ = &ctx;
    if (!input_macro_runtime_) {
        ctx[context::key::battle::MACRO_FAILURE_CODE] =
            static_cast<std::uint32_t>(phase::battle::macroprobe::FailureCode::InvalidMode);
        return;
    }
    if (input_macro_runtime_->state() == inputmacro::InputMacroRuntimeState::Completed) {
        ctx[context::key::battle::MACRO_RESULT] = 0u;
        ctx[context::key::battle::MACRO_FAILURE_CODE] = 0u;
        return;
    }

    const auto result = input_macro_runtime_->ExecuteNext();
    apply_input_macro_step_result(result, ctx);
    SCLOGI("[battle-input-macro] step=%zu label=%s kind=%u failure=%u terminal=%u expected=%s hit=%u pc=%08X",
        result.step_index,
        result.label.c_str(),
        static_cast<std::uint32_t>(result.action_kind),
        static_cast<std::uint32_t>(result.failure),
        static_cast<std::uint32_t>(result.terminal_status),
        BreakpointListDescription(result.expected_keys).c_str(),
        static_cast<std::uint32_t>(result.hit_key),
        result.hit_pc);
}

} // namespace savor
