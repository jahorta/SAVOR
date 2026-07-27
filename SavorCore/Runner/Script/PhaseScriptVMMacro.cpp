#include "PhaseScriptVM.h"

#include "../InputMacro/InputMacroRuntime.h"
#include "../IPC/Wire.h"
#include "../../Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
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
    SCLOGE(
        "[input-macro] legacy exclusive session is disconnected; use interaction composition");
    return false;
}

void PhaseScriptVM::release_exclusive_session()
{
    if (!input_macro_session_active_) return;
    input_macro_enabled_bp_keys_.clear();
    input_macro_provider_keys_.clear();
    input_macro_session_active_ = false;
    SCLOGI("[input-macro] legacy exclusive session state released locally");
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
        .track_input_poll = true,
    });

    input_macro_enabled_bp_keys_.clear();

    result.hit = run.run.hit;
    result.hit_key = static_cast<BPKey>(run.hit_bp_key);
    result.hit_pc = run.run.hit ? static_cast<std::uint32_t>(run.run.pc) : 0u;
    result.input_epoch = run.input_epoch;
    result.requested_input = run.requested_input;
    result.input_poll_count = run.input_poll_count;
    result.input_acknowledged = run.input_acknowledged;
    if (run.run.hit) {
        result.stop_sequence = ++input_macro_stop_sequence_;
        current_input_macro_stop_ = inputmacro::InputMacroStopInfo{
            .key = result.hit_key,
            .pc = result.hit_pc,
            .stop_sequence = result.stop_sequence,
            .input_epoch = result.input_epoch,
            .requested_input = result.requested_input,
            .input_poll_count = result.input_poll_count,
            .input_acknowledged = result.input_acknowledged,
        };
    }
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

    SCLOGI("[input-macro] wait expected=%s hit=%u pc=%08X status=%u input_epoch=%llu input_polls=%u input_ack=%u",
        BreakpointListDescription(action.expected_keys).c_str(),
        static_cast<std::uint32_t>(result.hit_key),
        result.hit_pc,
        static_cast<std::uint32_t>(result.status),
        static_cast<unsigned long long>(result.input_epoch),
        result.input_poll_count,
        result.input_acknowledged ? 1u : 0u);
    return result;
}

inputmacro::InputMacroHostStatus PhaseScriptVM::step_neutral_frames(
    std::uint32_t frame_count)
{
    (void)frame_count;
    SCLOGE(
        "[input-macro] frame advancement is disconnected; use interaction composition");
    return inputmacro::InputMacroHostStatus::Failed;
}

bool PhaseScriptVM::read_u32(std::uint32_t address, std::uint32_t& value)
{
    return host_.readU32(address, value);
}

inputmacro::MemoryChangeResult PhaseScriptVM::wait_for_u32_change(
    std::uint32_t address,
    std::uint32_t baseline,
    std::uint32_t timeout_ms)
{
    (void)address;
    (void)timeout_ms;
    SCLOGE(
        "[input-macro] memory-change advancement is disconnected; use observation and interaction composition");
    return inputmacro::MemoryChangeResult{
        .status = inputmacro::InputMacroHostStatus::Failed,
        .latest_value = baseline,
    };
}

void PhaseScriptVM::set_neutral_input()
{
    SCLOGE(
        "[input-macro] direct input publication is disconnected; use interaction composition");
}

void PhaseScriptVM::clear_macro_memory_watchpoints()
{
    SCLOGE(
        "[input-macro] memory-watchpoint cleanup is disconnected; use scoped router resources");
}

void PhaseScriptVM::restore_breakpoint_state()
{
    SCLOGE(
        "[input-macro] breakpoint restoration is disconnected; use scoped router resources");
}

inputmacro::InputMacroStopInfo PhaseScriptVM::current_stop() const
{
    if (current_input_macro_stop_.stop_sequence != 0
        && current_input_macro_stop_.stop_sequence == input_macro_stop_sequence_) {
        return current_input_macro_stop_;
    }

    auto stop = inputmacro::InputMacroStopInfo{};
    stop.pc = host_.getPC();
    stop.stop_sequence = input_macro_stop_sequence_;
    stop.key = current_breakpoint_key();
    return stop;
}

bool PhaseScriptVM::read_guest_memory(
    std::uint32_t address,
    std::span<std::byte> output) const
{
    if (address == 0
        || output.size() > static_cast<std::size_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }
    if (output.empty()) return true;

    std::string bytes;
    if (!host_.getMem1RangeRaw(
            bytes,
            address,
            static_cast<std::uint32_t>(output.size()))
        || bytes.size() != output.size()) {
        return false;
    }
    std::memcpy(output.data(), bytes.data(), output.size());
    return true;
}

std::uint64_t PhaseScriptVM::current_vi() const
{
    return host_.getViFieldCountApprox();
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

void PhaseScriptVM::op_materialize_battle_results_screen_macro_steps(PSContext& ctx)
{
    namespace endresults = phase::battle::endresults;
    namespace key = context::key;

    cancel_input_macro();
    active_input_macro_context_ = &ctx;
    ctx[key::battleend::OUTCOME] = static_cast<std::uint32_t>(endresults::Outcome::Failed);
    ctx[key::battleend::PROVIDER_FAILURE] =
        static_cast<std::uint32_t>(endresults::FailureCode::None);
    ctx[key::battleend::RUNTIME_FAILURE] =
        static_cast<std::uint32_t>(inputmacro::InputMacroFailure::None);
    ctx[key::battleend::MACRO_RESULT] = 1u;

    std::uint32_t raw_policy =
        static_cast<std::uint32_t>(endresults::AccelerationPolicy::FullAdaptive);
    ctx.get(key::battleend::ACCELERATION_POLICY, raw_policy);
    std::string manifest_blob;
    ctx.get(key::battleend::COMPLETION_MANIFEST_BLOB, manifest_blob);
    auto driver = std::make_unique<inputmacro::BattleResultsScreenInputMacroProvider>(
        inputmacro::BattleResultsScreenInputMacroProvider::Request{
            .acceleration_policy =
                static_cast<endresults::AccelerationPolicy>(raw_policy),
            .completion_manifest_blob = std::move(manifest_blob),
        });
    auto* driver_view = driver.get();
    input_macro_plan_driver_ = std::move(driver);
    input_macro_context_sink_ = InputMacroContextSink::BattleResultsScreen;

    auto decision = input_macro_plan_driver_->Start(*this);
    if (decision.status == inputmacro::InputMacroDriverStatus::PlanReady) {
        const auto declared = input_macro_plan_driver_->declared_breakpoint_keys();
        const auto started = input_macro_runtime_->Replace(
            std::move(decision.plan),
            declared);
        if (started.failure != inputmacro::InputMacroFailure::None) {
            ctx[key::battleend::RUNTIME_FAILURE] =
                static_cast<std::uint32_t>(started.failure);
            (void)input_macro_plan_driver_->Advance(*this, started);
        }
    } else if (decision.status == inputmacro::InputMacroDriverStatus::Failed) {
        ctx[key::battleend::RUNTIME_FAILURE] =
            static_cast<std::uint32_t>(decision.failure);
    }

    sync_input_macro_driver_context(ctx);
    std::uint32_t runtime_failure = 0;
    ctx.get(key::battleend::RUNTIME_FAILURE, runtime_failure);
    SCLOGI(
        "[battle-end-results-macro] materialize driver_status=%u provider_failure=%u runtime_failure=%u",
        static_cast<std::uint32_t>(decision.status),
        static_cast<std::uint32_t>(driver_view->failure()),
        runtime_failure);
}

void PhaseScriptVM::op_materialize_battle_completion_macro_steps(PSContext& ctx)
{
    namespace endresults = phase::battle::endresults;
    namespace key = context::key;

    cancel_input_macro();
    active_input_macro_context_ = &ctx;
    ctx[key::battlecompletion::OUTCOME] =
        static_cast<std::uint32_t>(endresults::Outcome::Failed);
    ctx[key::battlecompletion::PROVIDER_FAILURE] =
        static_cast<std::uint32_t>(endresults::FailureCode::None);
    ctx[key::battlecompletion::RUNTIME_FAILURE] =
        static_cast<std::uint32_t>(inputmacro::InputMacroFailure::None);
    ctx[key::battlecompletion::MACRO_RESULT] = 1u;

    auto driver = std::make_unique<inputmacro::BattleCompletionInputMacroProvider>();
    auto* driver_view = driver.get();
    input_macro_plan_driver_ = std::move(driver);
    input_macro_context_sink_ = InputMacroContextSink::BattleCompletion;
    auto decision = input_macro_plan_driver_->Start(*this);
    if (decision.status == inputmacro::InputMacroDriverStatus::PlanReady) {
        const auto started = input_macro_runtime_->Replace(
            std::move(decision.plan),
            input_macro_plan_driver_->declared_breakpoint_keys());
        if (started.failure != inputmacro::InputMacroFailure::None) {
            ctx[key::battlecompletion::RUNTIME_FAILURE] =
                static_cast<std::uint32_t>(started.failure);
            (void)input_macro_plan_driver_->Advance(*this, started);
        }
    } else if (decision.status == inputmacro::InputMacroDriverStatus::Failed) {
        ctx[key::battlecompletion::RUNTIME_FAILURE] =
            static_cast<std::uint32_t>(decision.failure);
    }
    sync_input_macro_driver_context(ctx);
    SCLOGI(
        "[battle-completion-macro] materialize driver_status=%u provider_failure=%u",
        static_cast<std::uint32_t>(decision.status),
        static_cast<std::uint32_t>(driver_view->failure()));
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
    if (input_macro_plan_driver_) {
        namespace key = context::key;
        auto result = input_macro_runtime_->ExecuteNext();
        const auto set_runtime_failure = [&](inputmacro::InputMacroFailure failure) {
            if (input_macro_context_sink_ == InputMacroContextSink::BattleCompletion)
                ctx[key::battlecompletion::RUNTIME_FAILURE] =
                    static_cast<std::uint32_t>(failure);
            else if (input_macro_context_sink_ == InputMacroContextSink::BattleResultsScreen)
                ctx[key::battleend::RUNTIME_FAILURE] =
                    static_cast<std::uint32_t>(failure);
        };
        set_runtime_failure(result.failure);

        inputmacro::InputMacroDriverDecision decision{};
        bool advanced = false;
        if (result.terminal()) {
            decision = input_macro_plan_driver_->Advance(*this, result);
            advanced = true;
            if (decision.status == inputmacro::InputMacroDriverStatus::PlanReady) {
                const auto started = input_macro_runtime_->Replace(
                    std::move(decision.plan),
                    input_macro_plan_driver_->declared_breakpoint_keys());
                if (started.failure != inputmacro::InputMacroFailure::None) {
                    set_runtime_failure(started.failure);
                    decision = input_macro_plan_driver_->Advance(*this, started);
                } else {
                    set_runtime_failure(inputmacro::InputMacroFailure::None);
                }
            } else if (decision.status == inputmacro::InputMacroDriverStatus::Failed) {
                set_runtime_failure(decision.failure);
            }
        }

        sync_input_macro_driver_context(ctx);
        SCLOGI(
            "[input-macro-driver] sink=%u step=%zu label=%s failure=%u terminal=%u advanced=%u driver_status=%u hit=%u pc=%08X",
            static_cast<std::uint32_t>(input_macro_context_sink_),
            result.step_index,
            result.label.c_str(),
            static_cast<std::uint32_t>(result.failure),
            static_cast<std::uint32_t>(result.terminal_status),
            advanced ? 1u : 0u,
            advanced ? static_cast<std::uint32_t>(decision.status) : 0u,
            static_cast<std::uint32_t>(result.hit_key),
            result.hit_pc);
        return;
    }
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

void PhaseScriptVM::sync_input_macro_driver_context(PSContext& ctx) const
{
    switch (input_macro_context_sink_) {
    case InputMacroContextSink::None: return;
    case InputMacroContextSink::BattleCompletion:
        sync_battle_completion_context(ctx);
        return;
    case InputMacroContextSink::BattleResultsScreen:
        sync_battle_results_screen_context(ctx);
        return;
    }
}

void PhaseScriptVM::sync_battle_results_screen_context(PSContext& ctx) const
{
    const auto* driver = dynamic_cast<const inputmacro::BattleResultsScreenInputMacroProvider*>(
        input_macro_plan_driver_.get());
    if (driver == nullptr) return;

    namespace endresults = phase::battle::endresults;
    namespace key = context::key;
    const auto diagnostics = driver->diagnostics();
    ctx[key::battleend::ACCELERATION_POLICY] =
        static_cast<std::uint32_t>(diagnostics.policy);
    ctx[key::battleend::OUTCOME] = static_cast<std::uint32_t>(diagnostics.outcome);
    ctx[key::battleend::PROVIDER_FAILURE] =
        static_cast<std::uint32_t>(diagnostics.failure);
    ctx[key::battleend::MACRO_RESULT] = diagnostics.completed ? 0u : 1u;
    ctx[key::battleend::ACTION_COUNT] = diagnostics.action_count;
    ctx[key::battleend::INPUT_REQUEST_COUNT] = diagnostics.input_request_count;
    ctx[key::battleend::INPUT_OBSERVED_COUNT] = diagnostics.input_observed_count;
    ctx[key::battleend::RELEASE_REQUEST_COUNT] = diagnostics.release_request_count;
    ctx[key::battleend::RELEASE_OBSERVED_COUNT] = diagnostics.release_observed_count;
    ctx[key::battleend::LAST_EXPECTED_BP] =
        static_cast<std::uint32_t>(diagnostics.last_expected_key);
    ctx[key::battleend::LAST_HIT_BP] =
        static_cast<std::uint32_t>(diagnostics.last_hit_key);
    ctx[key::battleend::LAST_HIT_PC] = diagnostics.last_hit_pc;
    ctx[key::battleend::LAST_STATE] = diagnostics.last_state;
    ctx[key::battleend::LAST_SUBSTATE] = diagnostics.last_substate;
    ctx[key::battleend::LAST_TOKEN_LO] = static_cast<std::uint32_t>(diagnostics.last_token);
    ctx[key::battleend::LAST_TOKEN_HI] =
        static_cast<std::uint32_t>(diagnostics.last_token >> 32);
    ctx[key::battleend::EXPECTED_STAT_WAVES] = diagnostics.expected_stat_waves;
    ctx[key::battleend::OBSERVED_STAT_WAVES] = diagnostics.observed_stat_waves;
    ctx[key::battleend::EXPECTED_LEARNED_WAVES] = diagnostics.expected_learned_waves;
    ctx[key::battleend::OBSERVED_LEARNED_WAVES] = diagnostics.observed_learned_waves;
    ctx[key::battleend::EXPECTED_ITEM_POPUP] = diagnostics.expected_item_popup ? 1u : 0u;
    ctx[key::battleend::OBSERVED_ITEM_POPUP] = diagnostics.observed_item_popup ? 1u : 0u;
    ctx[key::battleend::MISMATCH_FLAGS] = diagnostics.mismatch_flags;
    ctx[key::battleend::INVARIANT_FLAGS] = diagnostics.invariant_flags;
    ctx[key::battleend::SOURCE_INVARIANT_FLAGS] =
        diagnostics.invariant_flags & endresults::InvariantSourcePc;
    ctx[key::battleend::REWARD_INVARIANT_FLAGS] = diagnostics.invariant_flags
        & (endresults::InvariantVictoryState | endresults::InvariantRewardPhase);
    ctx[key::battleend::LIFECYCLE_INVARIANT_FLAGS] =
        diagnostics.invariant_flags & endresults::InvariantLifecycleState;
    ctx[key::battleend::COMPLETION_INVARIANT_FLAGS] = diagnostics.invariant_flags
        & (endresults::InvariantCompletionState
            | endresults::InvariantCompletionPublished
            | endresults::InvariantResultPointerCleared);
    ctx[key::battleend::ENTRY_RNG_SEED] = diagnostics.entry_rng_seed;
    ctx[key::battleend::FINAL_RNG_SEED] = diagnostics.final_rng_seed;
    ctx[key::battleend::RNG_EFFECT_KIND] = 0u; // RngEffectKind::Preserve
    ctx[key::battleend::RNG_ADVANCE_COUNT] = 0u;
    ctx[key::battleend::REPORT_BLOB] = driver->report_blob();
    ctx[key::battleend::DIAGNOSTIC] = driver->diagnostic();
}

void PhaseScriptVM::sync_battle_completion_context(PSContext& ctx) const
{
    const auto* driver = dynamic_cast<const inputmacro::BattleCompletionInputMacroProvider*>(
        input_macro_plan_driver_.get());
    if (driver == nullptr) return;
    namespace key = context::key;
    const auto diagnostics = driver->diagnostics();
    ctx[key::battlecompletion::OUTCOME] =
        static_cast<std::uint32_t>(diagnostics.outcome);
    ctx[key::battlecompletion::PROVIDER_FAILURE] =
        static_cast<std::uint32_t>(diagnostics.failure);
    ctx[key::battlecompletion::MACRO_RESULT] = diagnostics.completed ? 0u : 1u;
    ctx[key::battlecompletion::MANIFEST_BLOB] = driver->manifest_blob();
    ctx[key::battlecompletion::DIAGNOSTIC] = driver->diagnostic();
    ctx[key::battlecompletion::INVARIANT_FLAGS] = diagnostics.invariant_flags;
    ctx[key::battlecompletion::LAST_EXPECTED_BP] =
        static_cast<std::uint32_t>(diagnostics.last_expected_key);
    ctx[key::battlecompletion::LAST_HIT_BP] =
        static_cast<std::uint32_t>(diagnostics.last_hit_key);
    ctx[key::battlecompletion::LAST_HIT_PC] = diagnostics.last_hit_pc;
    ctx[key::battlecompletion::EXPECTED_LEVEL_PANELS] =
        diagnostics.expected_level_panels;
    ctx[key::battlecompletion::EXPECTED_STAT_WAVES] =
        diagnostics.expected_stat_waves;
    ctx[key::battlecompletion::EXPECTED_MAGIC_RANK_EVENTS] =
        diagnostics.expected_magic_rank_events;
    ctx[key::battlecompletion::EXPECTED_LEARNED_WAVES] =
        diagnostics.expected_learned_waves;
    ctx[key::battlecompletion::EXPECTED_ITEM_POPUPS] =
        diagnostics.expected_item_popups;
    ctx[key::battlecompletion::START_VI_LO] =
        static_cast<std::uint32_t>(diagnostics.start_vi);
    ctx[key::battlecompletion::START_VI_HI] =
        static_cast<std::uint32_t>(diagnostics.start_vi >> 32);
    ctx[key::battlecompletion::END_VI_LO] =
        static_cast<std::uint32_t>(diagnostics.end_vi);
    ctx[key::battlecompletion::END_VI_HI] =
        static_cast<std::uint32_t>(diagnostics.end_vi >> 32);
}

} // namespace savor
