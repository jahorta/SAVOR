#include "PhaseScriptVM.h"

#include "ScriptProgress.h"
#include "../IPC/Wire.h"
#include "../../Core/Memory/MemView.h"
#include "../../Core/Memory/Soa/Battle/BattleContextCodec.h"
#include "../../Phases/Programs/BattleMacroProbe/BattleMacroProbePayload.h"
#include "../../Phases/Programs/BattleRunner/BattleRunnerPayload.h"

#include <algorithm>
#include <chrono>
#include <sstream>

namespace {
const BPAddr* find_hit_bp_in_keys(
    const BreakpointMap& bpmap,
    const std::vector<BPKey>& keys,
    std::uint32_t pc)
{
    for (const auto key : keys) {
        if (const auto* entry = bpmap.find(key); entry != nullptr && entry->pc == pc)
            return entry;
    }
    return nullptr;
}

const BPAddr* find_hit_bp(
    const BreakpointMap& bpmap,
    const std::vector<BPKey>& canonical_keys,
    const std::vector<BPKey>& gated_keys,
    const std::vector<BPKey>& predicate_keys,
    std::uint32_t pc)
{
    if (const auto* entry = find_hit_bp_in_keys(bpmap, canonical_keys, pc))
        return entry;
    if (const auto* entry = find_hit_bp_in_keys(bpmap, gated_keys, pc))
        return entry;
    return find_hit_bp_in_keys(bpmap, predicate_keys, pc);
}

std::string bp_key_list_desc(const std::vector<BPKey>& keys)
{
    std::ostringstream out;
    for (size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) out << ',';
        out << static_cast<uint32_t>(keys[index]);
    }
    return out.str();
}
} // namespace

namespace savor {
    void PhaseScriptVM::op_materialize_battle_macro_steps(PSContext& ctx) {
        using phase::battle::macroprobe::BuildMacroPlanSteps;
        using phase::battle::macroprobe::BuildMacroProbePlanSteps;
        using phase::battle::macroprobe::BuildPlanningContext;
        using phase::battle::macroprobe::DeserializeCommandPlan;
        using phase::battle::macroprobe::FakeAttackMemoryGateMode;
        using phase::battle::macroprobe::FakeAttackPattern;
        using phase::battle::macroprobe::FailureCode;
        using phase::battle::macroprobe::FormatPlanningContext;
        using phase::battle::macroprobe::MacroCommand;
        using phase::battle::macroprobe::MacroMode;
        using phase::battle::macroprobe::MacroStep;

        uint32_t raw_mode = 0;
        uint32_t target_slot = 4;
        uint32_t transition_neutral_frames = 3;
        std::string plan_blob;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_MODE, raw_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TARGET_SLOT, target_slot);
        ctx.get<std::string>(savor::context::key::battle::MACRO_PLAN_BLOB, plan_blob);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);
        end_macro_breakpoint_scope();
        battle_macro_steps_.clear();

        std::vector<MacroCommand> commands;
        FailureCode build_failure = FailureCode::Ok;
        if (!plan_blob.empty()) {
            std::string parse_error;
            if (!DeserializeCommandPlan(plan_blob, &commands, &parse_error)) {
                build_failure = FailureCode::InvalidMode;
                SCLOGW("[battle-macro-probe] invalid plan_blob='%s' error=%s", plan_blob.c_str(), parse_error.c_str());
            }
        } else {
            commands.push_back(MacroCommand{.mode = static_cast<MacroMode>(raw_mode), .target_slot = target_slot});
        }
        ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(build_failure);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
        battle_macro_memory_baseline_valid_ = false;
        battle_macro_memory_addr_ = 0u;
        battle_macro_memory_baseline_ = 0u;

        if (build_failure != FailureCode::Ok) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            SCLOGW("[battle-macro-probe] invalid macro mode=%u target_slot=%u plan='%s' failure=%s",
                raw_mode,
                target_slot,
                plan_blob.c_str(),
                phase::battle::macroprobe::FailureCodeName(build_failure));
            return;
        }

        const uint32_t current_pc = host_.getPC();
        const BPAddr* current_bp = find_hit_bp(bpmap_, canonical_bp_keys_, gated_bp_keys_, predicate_bp_keys_, current_pc);
        const uint32_t current_bp_key = current_bp != nullptr ? static_cast<uint32_t>(current_bp->key) : 0u;
        if (current_bp_key != static_cast<uint32_t>(bp::battle::TurnInputs)) {
            ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::TurnInputs);
            const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
                .expected_bp_keys = {bp::battle::TurnInputs},
                .input = GCInputFrame{},
                .apply_input = true,
                .release_input = true,
                .step_off_current_bp = true,
                .expected_only_scope = true,
                .watch_movie = false,
                .include_gated_hit_lookup = true,
                .update_derived = false,
            });
            ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
            ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;
            SCLOGI("[battle-macro-probe-preflight] current_pc=%08X current_bp=%u expected=%u hit=%u hit_pc=%08X ok=%d",
                current_pc,
                current_bp_key,
                static_cast<uint32_t>(bp::battle::TurnInputs),
                rr.hit_bp_key,
                rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
                rr.expected_match ? 1 : 0);
            if (!rr.run.hit) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                return;
            }
            if (!rr.expected_match) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
                return;
            }
        }

        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate);
        const auto ready_rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = {bp::battle::BattleMacroInputReadyGate},
            .input = GCInputFrame{},
            .apply_input = true,
            .release_input = true,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_gated_hit_lookup = true,
            .update_derived = false,
        });
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = ready_rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u;
        SCLOGI("[battle-macro-probe-start-gate] expected=%u hit=%u hit_pc=%08X ok=%d",
            static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate),
            ready_rr.hit_bp_key,
            ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u,
            ready_rr.expected_match ? 1 : 0);
        if (!ready_rr.run.hit) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            return;
        }
        if (!ready_rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            return;
        }

        std::string mem1;
        soa::battle::ctx::BattleContext battle_context{};
        if (!host_.getMem1(mem1)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-macro-probe] failed to capture MEM1 for planning context");
            return;
        }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        if (!soa::battle::ctx::codec::extract_from_mem1(view, battle_context)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-macro-probe] failed to extract battle planning context from MEM1");
            return;
        }

        const auto planning_context = BuildPlanningContext(battle_context);
        SCLOGI("[battle-macro-probe-planning-context] %s", FormatPlanningContext(planning_context).c_str());

        uint32_t fake_attack_count = 0;
        ctx.get<uint32_t>(savor::context::key::battle::FAKE_ATTACK_COUNT_THIS_TURN, fake_attack_count);
        uint32_t raw_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t fake_target_neutral_frames = 0;
        uint32_t fake_input_neutral_frames = 20;
        uint32_t fake_memory_timeout_ms = 1000;
        uint32_t use_mixed_fake_attack_patterns = 0;
        uint32_t raw_first_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t first_fake_target_neutral_frames = 0;
        uint32_t first_fake_input_neutral_frames = 20;
        uint32_t first_fake_memory_timeout_ms = 1000;
        uint32_t use_final_fake_attack_pattern = 0;
        uint32_t raw_final_fake_memory_gate_mode = static_cast<uint32_t>(FakeAttackMemoryGateMode::TargetSide);
        uint32_t final_fake_target_neutral_frames = 0;
        uint32_t final_fake_input_neutral_frames = 20;
        uint32_t final_fake_memory_timeout_ms = 1000;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_MEMORY_GATE_MODE, raw_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_TARGET_NEUTRAL_FRAMES, fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_INPUT_NEUTRAL_FRAMES, fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_MEMORY_TIMEOUT_MS, fake_memory_timeout_ms);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_USE_MIXED_PATTERNS, use_mixed_fake_attack_patterns);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_GATE_MODE, raw_first_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_TARGET_NEUTRAL_FRAMES, first_fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_INPUT_NEUTRAL_FRAMES, first_fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FIRST_MEMORY_TIMEOUT_MS, first_fake_memory_timeout_ms);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_USE_FINAL_PATTERN, use_final_fake_attack_pattern);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_GATE_MODE, raw_final_fake_memory_gate_mode);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_TARGET_NEUTRAL_FRAMES, final_fake_target_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_INPUT_NEUTRAL_FRAMES, final_fake_input_neutral_frames);
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_FAKE_FINAL_MEMORY_TIMEOUT_MS, final_fake_memory_timeout_ms);
        FakeAttackPattern fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_fake_memory_gate_mode),
            .target_neutral_before_b_frames = fake_target_neutral_frames,
            .input_neutral_after_b_frames = fake_input_neutral_frames,
            .memory_timeout_ms = fake_memory_timeout_ms,
        };
        FakeAttackPattern first_fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_first_fake_memory_gate_mode),
            .target_neutral_before_b_frames = first_fake_target_neutral_frames,
            .input_neutral_after_b_frames = first_fake_input_neutral_frames,
            .memory_timeout_ms = first_fake_memory_timeout_ms,
        };
        FakeAttackPattern final_fake_attack_pattern{
            .memory_gate_mode = static_cast<FakeAttackMemoryGateMode>(raw_final_fake_memory_gate_mode),
            .target_neutral_before_b_frames = final_fake_target_neutral_frames,
            .input_neutral_after_b_frames = final_fake_input_neutral_frames,
            .memory_timeout_ms = final_fake_memory_timeout_ms,
        };
        const auto steps = use_final_fake_attack_pattern != 0
            ? BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                use_mixed_fake_attack_patterns != 0 ? first_fake_attack_pattern : fake_attack_pattern,
                fake_attack_pattern,
                final_fake_attack_pattern,
                &planning_context,
                &build_failure)
            : use_mixed_fake_attack_patterns != 0
            ? BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                first_fake_attack_pattern,
                fake_attack_pattern,
                &planning_context,
                &build_failure)
            : BuildMacroProbePlanSteps(
                commands,
                transition_neutral_frames,
                fake_attack_count,
                fake_attack_pattern,
                &planning_context,
                &build_failure);
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(build_failure);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = static_cast<uint32_t>(steps.size());
        if (steps.empty() || build_failure != FailureCode::Ok) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            SCLOGW("[battle-macro-probe] invalid live macro plan mode=%u target_slot=%u plan='%s' failure=%s context='%s'",
                raw_mode,
                target_slot,
                plan_blob.c_str(),
                phase::battle::macroprobe::FailureCodeName(build_failure),
                FormatPlanningContext(planning_context).c_str());
            return;
        }

        battle_macro_steps_.reserve(steps.size());
        for (const auto& step : steps) {
            battle_macro_steps_.push_back(RuntimeBreakpointStep{
                .label = step.label ? step.label : "",
                .kind = [kind = step.kind]() {
                    switch (kind) {
                    case MacroStep::Kind::NeutralFrames: return RuntimeMacroStepKind::NeutralFrames;
                    case MacroStep::Kind::CaptureMemoryU32: return RuntimeMacroStepKind::CaptureMemoryU32;
                    case MacroStep::Kind::WaitMemoryU32Changed: return RuntimeMacroStepKind::WaitMemoryU32Changed;
                    case MacroStep::Kind::InputGate:
                    default: return RuntimeMacroStepKind::InputGate;
                    }
                }(),
                .input = step.input,
                .frame_count = step.frame_count,
                .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
                .memory_addr = step.memory_addr,
                .memory_timeout_ms = step.memory_timeout_ms,
                .memory_cycle_index = step.memory_cycle_index,
                .expected_bp_keys = step.expected_bps,
            });
        }
        begin_macro_breakpoint_scope();
    }

    void PhaseScriptVM::op_materialize_battle_turn_macro_steps(PSContext& ctx) {
        using phase::battle::macroprobe::BuildMacroPlanStepsFromTurnPlan;
        using phase::battle::macroprobe::BuildPlanningContext;
        using phase::battle::macroprobe::FailureCode;
        using phase::battle::macroprobe::FormatPlanningContext;
        using soa::battle::actions::MaterializeErr;

        end_macro_breakpoint_scope();
        battle_macro_steps_.clear();
        ctx[savor::context::key::battle::MACRO_RESULT] = 1u;
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = 0u;
        ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = 0u;
        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::OK);
        battle_macro_memory_baseline_valid_ = false;
        battle_macro_memory_addr_ = 0u;
        battle_macro_memory_baseline_ = 0u;

        uint32_t turn = 0;
        ctx.get<uint32_t>(savor::context::key::battle::ACTIVE_TURN, turn);
        if (turn == 0) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::InvalidTurnIdxZero);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }

        soa::battle::actions::BattlePath path;
        if (!ctx.get<soa::battle::actions::BattlePath>(savor::context::key::battle::TURN_PLANS, path)) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::BadBlob);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }
        if (turn > path.size()) {
            ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(MaterializeErr::OutOfTurns);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            return;
        }

        const uint32_t current_pc = host_.getPC();
        const BPAddr* current_bp = find_hit_bp(bpmap_, canonical_bp_keys_, gated_bp_keys_, predicate_bp_keys_, current_pc);
        const uint32_t current_bp_key = current_bp != nullptr ? static_cast<uint32_t>(current_bp->key) : 0u;
        if (current_bp_key != static_cast<uint32_t>(bp::battle::TurnInputs)) {
            ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::TurnInputs);
            const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
                .expected_bp_keys = {bp::battle::TurnInputs},
                .input = GCInputFrame{},
                .apply_input = true,
                .release_input = true,
                .step_off_current_bp = true,
                .expected_only_scope = true,
                .watch_movie = false,
                .include_gated_hit_lookup = true,
                .update_derived = false,
            });
            ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
            ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;
            SCLOGI("[battle-turn-macro-preflight] current_pc=%08X current_bp=%u expected=%u hit=%u hit_pc=%08X ok=%d",
                current_pc,
                current_bp_key,
                static_cast<uint32_t>(bp::battle::TurnInputs),
                rr.hit_bp_key,
                rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
                rr.expected_match ? 1 : 0);
            if (!rr.run.hit) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                return;
            }
            if (!rr.expected_match) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
                return;
            }
        }

        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate);
        const auto ready_rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = {bp::battle::BattleMacroInputReadyGate},
            .input = GCInputFrame{},
            .apply_input = true,
            .release_input = true,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_gated_hit_lookup = true,
            .update_derived = false,
        });
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = ready_rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u;
        SCLOGI("[battle-turn-macro-start-gate] expected=%u hit=%u hit_pc=%08X ok=%d",
            static_cast<uint32_t>(bp::battle::BattleMacroInputReadyGate),
            ready_rr.hit_bp_key,
            ready_rr.run.hit ? static_cast<uint32_t>(ready_rr.run.pc) : 0u,
            ready_rr.expected_match ? 1 : 0);
        if (!ready_rr.run.hit) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            return;
        }
        if (!ready_rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            return;
        }

        std::string mem1;
        soa::battle::ctx::BattleContext battle_context{};
        if (!host_.getMem1(mem1)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-turn-macro] failed to capture MEM1 for planning context");
            return;
        }
        savor::MemView view(reinterpret_cast<const uint8_t*>(mem1.data()), mem1.size());
        if (!soa::battle::ctx::codec::extract_from_mem1(view, battle_context)) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::BattleContextUnavailable);
            SCLOGW("[battle-turn-macro] failed to extract battle planning context from MEM1");
            return;
        }

        const auto planning_context = BuildPlanningContext(battle_context);
        SCLOGI("[battle-turn-macro-planning-context] %s", FormatPlanningContext(planning_context).c_str());

        uint32_t transition_neutral_frames = 3;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_TRANSITION_NEUTRAL_FRAMES, transition_neutral_frames);
        MaterializeErr materialize_err = MaterializeErr::OK;
        const auto steps = BuildMacroPlanStepsFromTurnPlan(
            path[turn - 1],
            transition_neutral_frames,
            &planning_context,
            &materialize_err);

        ctx[savor::context::key::battle::PLAN_MATERIALIZE_ERR] = static_cast<uint32_t>(materialize_err);
        ctx[savor::context::key::battle::MACRO_STEP_COUNT] = static_cast<uint32_t>(steps.size());
        if (materialize_err != MaterializeErr::OK || steps.empty()) {
            const auto failure = materialize_err == MaterializeErr::NoValidTarget
                ? FailureCode::InvalidTarget
                : FailureCode::InvalidMode;
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(failure);
            SCLOGW("[battle-turn-macro] failed to materialize turn=%u failure=%s macro_failure=%s context='%s'",
                turn,
                soa::battle::actions::get_materialize_err_string(materialize_err).c_str(),
                phase::battle::macroprobe::FailureCodeName(failure),
                FormatPlanningContext(planning_context).c_str());
            return;
        }

        battle_macro_steps_.reserve(steps.size());
        for (const auto& step : steps) {
            battle_macro_steps_.push_back(RuntimeBreakpointStep{
                .label = step.label ? step.label : "",
                .kind = [kind = step.kind]() {
                    switch (kind) {
                    case phase::battle::macroprobe::MacroStep::Kind::NeutralFrames: return RuntimeMacroStepKind::NeutralFrames;
                    case phase::battle::macroprobe::MacroStep::Kind::CaptureMemoryU32: return RuntimeMacroStepKind::CaptureMemoryU32;
                    case phase::battle::macroprobe::MacroStep::Kind::WaitMemoryU32Changed: return RuntimeMacroStepKind::WaitMemoryU32Changed;
                    case phase::battle::macroprobe::MacroStep::Kind::InputGate:
                    default: return RuntimeMacroStepKind::InputGate;
                    }
                }(),
                .input = step.input,
                .frame_count = step.frame_count,
                .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
                .memory_addr = step.memory_addr,
                .memory_timeout_ms = step.memory_timeout_ms,
                .memory_cycle_index = step.memory_cycle_index,
                .expected_bp_keys = step.expected_bps,
            });
        }
        begin_macro_breakpoint_scope();
        ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
    }

    void PhaseScriptVM::op_execute_battle_macro_step(PSContext& ctx) {
        using phase::battle::macroprobe::FailureCode;

        uint32_t step_index = 0;
        ctx.get<uint32_t>(savor::context::key::battle::MACRO_LAST_STEP_INDEX, step_index);
        if (step_index >= battle_macro_steps_.size()) {
            end_macro_breakpoint_scope();
            ctx[savor::context::key::battle::MACRO_RESULT] = 0u;
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
            return;
        }

        const auto& step = battle_macro_steps_[step_index];
        const uint32_t expected_first = step.expected_bp_keys.empty() ? 0u : static_cast<uint32_t>(step.expected_bp_keys.front());
        ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = step_index;
        ctx[savor::context::key::battle::MACRO_LAST_EXPECTED_BP] = expected_first;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = 0u;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_PC_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_MEMWATCH_HITS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_UNATTRIBUTED_DELTAS] = 0u;
        ctx[savor::context::key::battle::MACRO_CAPTURE_ONLY_LAST_PC] = 0u;

        const auto advance_macro_step = [&]() {
            if (step_index + 1u >= battle_macro_steps_.size()) {
                end_macro_breakpoint_scope();
                ctx[savor::context::key::battle::MACRO_RESULT] = 0u;
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Ok);
            } else {
                ctx[savor::context::key::battle::MACRO_LAST_STEP_INDEX] = step_index + 1u;
            }
        };

        if (step.kind == RuntimeMacroStepKind::CaptureMemoryU32) {
            host_.clearMemoryWatchpoints();
            uint32_t value = 0;
            if (!read_u32(step.memory_addr, value)) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                SCLOGW("[battle-macro-memory-gate] label=%s capture_failed=true addr=%08X",
                    step.label.c_str(),
                    step.memory_addr);
                return;
            }
            battle_macro_memory_baseline_valid_ = true;
            battle_macro_memory_addr_ = step.memory_addr;
            battle_macro_memory_baseline_ = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = step.memory_addr;
            ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = value;
            ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = 0u;
            SCLOGI("[battle-macro-memory-gate] label=%s capture=true addr=%08X before=%08X",
                step.label.c_str(),
                step.memory_addr,
                value);
            advance_macro_step();
            return;
        }

        if (step.kind == RuntimeMacroStepKind::WaitMemoryU32Changed) {
            host_.clearMemoryWatchpoints();
            if (!battle_macro_memory_baseline_valid_ || battle_macro_memory_addr_ != step.memory_addr) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                SCLOGW("[battle-macro-memory-gate] label=%s missing_baseline=true addr=%08X baseline_addr=%08X",
                    step.label.c_str(),
                    step.memory_addr,
                    battle_macro_memory_addr_);
                return;
            }

            host_.setInput(GCInputFrame{});
            host_.setEnabledPcBreakpointsOnly({});
            const auto start = std::chrono::steady_clock::now();
            const uint32_t timeout_ms = step.memory_timeout_ms != 0 ? step.memory_timeout_ms : 1000u;
            uint32_t latest = battle_macro_memory_baseline_;
            uint32_t polls = 0;
            bool changed = false;
            bool read_failed = false;
            for (;;) {
                if (!read_u32(step.memory_addr, latest)) {
                    read_failed = true;
                    break;
                }
                if (latest != battle_macro_memory_baseline_) {
                    changed = true;
                    break;
                }
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed >= timeout_ms) {
                    break;
                }
                host_.stepOneFrameBlocking();
                ++polls;
            }
            if (!macro_breakpoint_scope_active_) {
                restore_canonical_breakpoint_scope();
            }
            const uint32_t elapsed_ms = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count());
            ctx[savor::context::key::battle::MACRO_MEMORY_ADDR] = step.memory_addr;
            ctx[savor::context::key::battle::MACRO_MEMORY_BASELINE] = battle_macro_memory_baseline_;
            ctx[savor::context::key::battle::MACRO_MEMORY_LATEST] = latest;
            ctx[savor::context::key::battle::MACRO_MEMORY_CHANGED] = changed ? 1u : 0u;
            ctx[savor::context::key::battle::MACRO_MEMORY_POLL_COUNT] = polls;
            ctx[savor::context::key::battle::MACRO_MEMORY_ELAPSED_MS] = elapsed_ms;
            uint32_t gate_count = 0;
            ctx.get<uint32_t>(savor::context::key::battle::MACRO_MEMORY_GATE_COUNT, gate_count);
            ctx[savor::context::key::battle::MACRO_MEMORY_GATE_COUNT] = gate_count + 1u;
            if (step.memory_cycle_index == 0) {
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_FIRST_ELAPSED_MS] = elapsed_ms;
            } else if (step.memory_cycle_index == 1) {
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT_ELAPSED_MS] = elapsed_ms;
            } else if (step.memory_cycle_index == 2) {
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_BASELINE] = battle_macro_memory_baseline_;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_LATEST] = latest;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_CHANGED] = changed ? 1u : 0u;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_POLL_COUNT] = polls;
                ctx[savor::context::key::battle::MACRO_MEMORY_REPEAT2_ELAPSED_MS] = elapsed_ms;
            }
            SCLOGI("[battle-macro-memory-gate] label=%s addr=%08X before=%08X after=%08X changed=%u polls=%u elapsed_ms=%u timeout_ms=%u",
                step.label.c_str(),
                step.memory_addr,
                battle_macro_memory_baseline_,
                latest,
                changed ? 1u : 0u,
                polls,
                elapsed_ms,
                timeout_ms);
            if (read_failed) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::MemoryReadFailed);
                end_macro_breakpoint_scope();
                return;
            }
            if (!changed) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
                end_macro_breakpoint_scope();
                return;
            }
            advance_macro_step();
            return;
        }

        if (step.kind == RuntimeMacroStepKind::NeutralFrames) {
            host_.clearMemoryWatchpoints();
            host_.setInput(GCInputFrame{});
            host_.setEnabledPcBreakpointsOnly({});
            for (uint32_t frame = 0; frame < step.frame_count; ++frame) {
                host_.stepOneFrameBlocking();
            }
            if (!macro_breakpoint_scope_active_) {
                restore_canonical_breakpoint_scope();
            }
            SCLOGI("[battle-macro-probe-step] index=%u label=%s neutral_frames=%u ok=1",
                step_index,
                step.label.c_str(),
                step.frame_count);
            if (derived_) derived_->update_on_bp(0u, ctx, host_);
            advance_macro_step();
            return;
        }

        if (step.expected_bp_keys.size() != 1u) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::InvalidMode);
            SCLOGW("[battle-macro-probe-step] invalid_expected_count index=%u label=%s expected=%s",
                step_index,
                step.label.c_str(),
                bp_key_list_desc(step.expected_bp_keys).c_str());
            end_macro_breakpoint_scope();
            return;
        }

        host_.emitProbeMarker("macro.step.begin", step_index);
        enable_macro_step_breakpoint(step.expected_bp_keys.front());
        const auto rr = run_until_bp_core(ctx, RunUntilBpSpec{
            .expected_bp_keys = step.expected_bp_keys,
            .input = step.input,
            .apply_input = true,
            .release_input = false,
            .hold_input_through_hit_opcode = step.hold_input_through_hit_opcode,
            .step_off_current_bp = true,
            .expected_only_scope = true,
            .watch_movie = false,
            .include_gated_hit_lookup = true,
            .update_derived = false,
        });
        disable_macro_step_breakpoint();
        host_.clearMemoryWatchpoints();
        host_.emitProbeMarker("macro.step.end", step_index);
        GCInputFrame released_input = step.input;
        released_input.buttons = static_cast<uint16_t>(released_input.buttons & ~step.input.buttons);
        host_.setInput(released_input);
        ctx[savor::context::key::battle::MACRO_LAST_HIT_BP] = rr.hit_bp_key;
        ctx[savor::context::key::battle::MACRO_LAST_HIT_PC] = rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u;

        SCLOGI("[battle-macro-probe-step] index=%u label=%s expected=%s hit=%u hit_pc=%08X ok=%d",
            step_index,
            step.label.c_str(),
            bp_key_list_desc(step.expected_bp_keys).c_str(),
            rr.hit_bp_key,
            rr.run.hit ? static_cast<uint32_t>(rr.run.pc) : 0u,
            rr.expected_match ? 1 : 0);

        if (!rr.run.hit) {
            if (rr.outcome == RunToBpOutcome::Aborted) {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Aborted);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] =
                    static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            } else {
                ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::Timeout);
                ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::Timeout);
            }
            end_macro_breakpoint_scope();
            return;
        }
        if (!rr.expected_match) {
            ctx[savor::context::key::core::DW_RUN_OUTCOME_CODE] = static_cast<uint32_t>(RunToBpOutcome::InputPlaybackFailed);
            ctx[savor::context::key::battle::MACRO_FAILURE_CODE] = static_cast<uint32_t>(FailureCode::UnexpectedBreakpoint);
            end_macro_breakpoint_scope();
            return;
        }

        if (derived_) derived_->update_on_bp(rr.hit_bp_key, ctx, host_);
        advance_macro_step();
    }

} // namespace savor
