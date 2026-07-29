#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include "Cli.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Phases/Programs/NavigationContext/NavigationContextPayload.h"
#include "Phases/Programs/NavigationContext/NavigationContextResult.h"
#include "Phases/Programs/NavigationContext/NavigationContextScript.h"
#include "Phases/Programs/ProgramRegistry.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"

namespace {

bool IsInputMacroAdapterOpcode(savor::PSOpCode code)
{
    switch (code) {
    case savor::PSOpCode::MATERIALIZE_BATTLE_MACRO_STEPS:
    case savor::PSOpCode::MATERIALIZE_BATTLE_TURN_MACRO_STEPS:
    case savor::PSOpCode::EXECUTE_BATTLE_MACRO_STEP:
    case savor::PSOpCode::MATERIALIZE_BATTLE_RESULTS_SCREEN_MACRO_STEPS:
    case savor::PSOpCode::MATERIALIZE_BATTLE_COMPLETION_MACRO_STEPS:
        return true;
    default:
        return false;
    }
}

std::size_t FindOpcode(
    const savor::PhaseScript& script,
    savor::PSOpCode code,
    std::size_t start = 0)
{
    const auto begin = script.ops.begin()
        + static_cast<std::ptrdiff_t>(
            (std::min)(start, script.ops.size()));
    const auto found = std::find_if(
        begin,
        script.ops.end(),
        [code](const savor::PSOp& op) { return op.code == code; });
    return found == script.ops.end()
        ? script.ops.size()
        : static_cast<std::size_t>(
            std::distance(script.ops.begin(), found));
}

bool ParseTestArgs(
    std::initializer_list<const char*> args,
    savor::e2e::CliOptions* options,
    std::string* error)
{
    std::vector<std::string> storage(args.begin(), args.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) argv.push_back(value.data());
    return savor::e2e::ParseArgs(
        static_cast<int>(argv.size()),
        argv.data(),
        options,
        error);
}

struct ScriptHostPlan {
    std::uint32_t entry_pc = 0x800e3694u;
    savor::RunToBpOutcome run_outcome = savor::RunToBpOutcome::Hit;
    std::uint32_t run_hit_key =
        bp::navigation::NavigationContextInitialPlayerInputReady;
    std::uint32_t run_hit_pc = soa::navigation::ctx::CapturePc;
    bool get_context_succeeds = true;
    bool save_succeeds = true;
};

struct ScriptRun {
    savor::PSContext context;
    bool returned = false;
    bool dispatch_failed = false;
    bool result_ok = false;
    int apply_input_calls = 0;
    int run_calls = 0;
    int get_context_calls = 0;
    int save_calls = 0;
    int emit_calls = 0;
};

bool Compare(
    std::uint32_t lhs,
    savor::PSCmp comparison,
    std::uint32_t rhs)
{
    switch (comparison) {
    case savor::PSCmp::EQ: return lhs == rhs;
    case savor::PSCmp::NE: return lhs != rhs;
    case savor::PSCmp::LT: return lhs < rhs;
    case savor::PSCmp::LE: return lhs <= rhs;
    case savor::PSCmp::GT: return lhs > rhs;
    case savor::PSCmp::GE: return lhs >= rhs;
    }
    return false;
}

std::uint32_t ReadU32(
    const savor::PSContext& context,
    savor::context::key::KeyId key)
{
    std::uint32_t value = 0;
    (void)context.get(key, value);
    return value;
}

ScriptRun ExecuteNavigationScript(const ScriptHostPlan& host)
{
    namespace key = savor::context::key;
    const auto script =
        phase::navigation::ctx::MakeNavigationContextProgram();
    std::unordered_map<std::string, std::size_t> labels;
    for (std::size_t index = 0; index < script.ops.size(); ++index) {
        if (script.ops[index].code == savor::PSOpCode::LABEL) {
            labels[script.ops[index].label.name] = index;
        }
    }

    ScriptRun run{};
    run.context[key::navigation::NEUTRAL_INPUT] =
        savor::GCInputFrame{};
    run.context[key::navigation::OUTPUT_SAVESTATE_PATH] =
        std::string{"navigation-context.sav"};
    run.context[key::navigation::OUTCOME] =
        static_cast<std::uint32_t>(
            phase::navigation::ctx::Outcome::Failed);
    run.context[key::navigation::FAILURE] =
        static_cast<std::uint32_t>(
            phase::navigation::ctx::FailureCode::None);

    auto jump = [&](const std::string& label, std::size_t& pc) {
        const auto found = labels.find(label);
        EXPECT_NE(found, labels.end()) << label;
        if (found == labels.end()) {
            run.dispatch_failed = true;
            return;
        }
        // PhaseScriptVM assigns the label's index and the for-loop advances
        // once, so the next dispatched operation follows the LABEL.
        pc = found->second + 1;
    };

    std::size_t pc = 0;
    std::size_t dispatch_budget = 256;
    while (pc < script.ops.size()
        && !run.returned
        && !run.dispatch_failed
        && dispatch_budget-- != 0) {
        const auto& op = script.ops[pc];
        bool advance = true;
        switch (op.code) {
        case savor::PSOpCode::ARM_PHASE_BPS_ONCE:
        case savor::PSOpCode::LOAD_SNAPSHOT:
        case savor::PSOpCode::LABEL:
            break;
        case savor::PSOpCode::APPLY_INPUT_FROM: {
            savor::GCInputFrame frame{};
            if (!run.context.get(op.key.id, frame)) {
                run.dispatch_failed = true;
                break;
            }
            ++run.apply_input_calls;
            EXPECT_EQ(frame.buttons, 0u);
            EXPECT_EQ(frame.main_x, 128u);
            EXPECT_EQ(frame.main_y, 128u);
            break;
        }
        case savor::PSOpCode::RECORD_CURRENT_PC_TO:
            run.context[op.key.id] = host.entry_pc;
            break;
        case savor::PSOpCode::RECORD_CURRENT_BP: {
            const bool at_capture =
                host.entry_pc == soa::navigation::ctx::CapturePc;
            run.context[key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<std::uint32_t>(
                    at_capture
                        ? savor::RunToBpOutcome::Hit
                        : savor::RunToBpOutcome::Unknown);
            run.context[key::core::RUN_HIT_PC] = host.entry_pc;
            run.context[key::core::RUN_HIT_BP_KEY] =
                at_capture
                    ? static_cast<std::uint32_t>(
                        bp::navigation::
                            NavigationContextInitialPlayerInputReady)
                    : 0u;
            break;
        }
        case savor::PSOpCode::GOTO_IF: {
            const auto lhs = ReadU32(run.context, op.jcc.key);
            if (Compare(lhs, op.jcc.cmp, op.jcc.imm)) {
                jump(op.jcc.name, pc);
                advance = false;
            }
            break;
        }
        case savor::PSOpCode::GOTO:
            jump(op.jmp.name, pc);
            advance = false;
            break;
        case savor::PSOpCode::RUN_UNTIL_BP:
            ++run.run_calls;
            run.context[key::core::DW_RUN_OUTCOME_CODE] =
                static_cast<std::uint32_t>(host.run_outcome);
            run.context[key::core::RUN_HIT_BP_KEY] =
                host.run_hit_key;
            run.context[key::core::RUN_HIT_PC] =
                host.run_hit_pc;
            break;
        case savor::PSOpCode::GET_NAVIGATION_CONTEXT:
            ++run.get_context_calls;
            if (!host.get_context_succeeds) {
                run.dispatch_failed = true;
                break;
            }
            run.context[key::navigation::CTX_BLOB] =
                std::string{"valid-nctx"};
            break;
        case savor::PSOpCode::SET_U32:
            run.context[op.keyimm.key] = op.keyimm.imm;
            break;
        case savor::PSOpCode::SAVE_SAVESTATE_FROM:
            ++run.save_calls;
            if (!host.save_succeeds) {
                run.dispatch_failed = true;
            }
            break;
        case savor::PSOpCode::EMIT_RESULT: {
            std::string blob;
            if (!run.context.get(op.key.id, blob)
                || blob.empty()) {
                run.dispatch_failed = true;
                break;
            }
            ++run.emit_calls;
            break;
        }
        case savor::PSOpCode::RETURN_RESULT:
            run.context[op.keyimm.key] = op.keyimm.imm;
            run.returned = true;
            run.result_ok =
                ReadU32(
                    run.context,
                    key::core::DW_RUN_OUTCOME_CODE)
                == static_cast<std::uint32_t>(
                    savor::RunToBpOutcome::Hit);
            break;
        default:
            ADD_FAILURE()
                << "test interpreter does not support "
                << savor::get_psop_identifier(op.code);
            run.dispatch_failed = true;
            break;
        }
        if (advance) ++pc;
    }
    EXPECT_NE(dispatch_budget, 0u);
    return run;
}

} // namespace

TEST(NavigationContextFramework, RegistersCompleteContextKeyContract)
{
    using namespace savor::context::key;

    EXPECT_EQ(NAVIGATION_CONTEXT_MIN, static_cast<KeyId>(0x0700));
    EXPECT_EQ(NAVIGATION_CONTEXT_MAX, static_cast<KeyId>(0x07ff));
    EXPECT_EQ(navigation::CTX_BLOB, static_cast<KeyId>(0x0700));
    EXPECT_EQ(
        navigation::OUTPUT_SAVESTATE_PATH,
        static_cast<KeyId>(0x0701));
    EXPECT_EQ(navigation::NEUTRAL_INPUT, static_cast<KeyId>(0x0702));
    EXPECT_EQ(navigation::ENTRY_PC, static_cast<KeyId>(0x0703));
    EXPECT_EQ(navigation::OUTCOME, static_cast<KeyId>(0x0704));
    EXPECT_EQ(navigation::FAILURE, static_cast<KeyId>(0x0705));
    EXPECT_EQ(navigation::DIAGNOSTIC, static_cast<KeyId>(0x0706));

    std::string registry_error;
    ASSERT_TRUE(validate_registry(&registry_error)) << registry_error;
    for (const auto& [key, name] : {
             std::pair{navigation::CTX_BLOB, "navigation.CTX_BLOB"},
             std::pair{
                 navigation::OUTPUT_SAVESTATE_PATH,
                 "navigation.output_savestate_path"},
             std::pair{
                 navigation::NEUTRAL_INPUT,
                 "navigation.neutral_input"},
             std::pair{navigation::ENTRY_PC, "navigation.entry_pc"},
             std::pair{navigation::OUTCOME, "navigation.outcome"},
             std::pair{navigation::FAILURE, "navigation.failure"},
             std::pair{
                 navigation::DIAGNOSTIC,
                 "navigation.diagnostic"},
         }) {
        EXPECT_EQ(name_for_id(key), name);
        KeyId resolved = 0;
        ASSERT_TRUE(id_for_name(name, resolved));
        EXPECT_EQ(resolved, key);
    }
}

TEST(NavigationContextFramework, ProgramUsesFixedCaptureAndNeutralInput)
{
    const auto script =
        phase::navigation::ctx::MakeNavigationContextProgram();

    ASSERT_EQ(
        script.canonical_bp_keys,
        (std::vector<BPKey>{
            bp::navigation::NavigationContextInitialPlayerInputReady}));
    EXPECT_TRUE(script.gated_bp_keys.empty());
    ASSERT_GE(script.ops.size(), 20u);

    EXPECT_EQ(
        script.ops[0].code,
        savor::PSOpCode::ARM_PHASE_BPS_ONCE);
    EXPECT_EQ(script.ops[1].code, savor::PSOpCode::LOAD_SNAPSHOT);
    EXPECT_EQ(script.ops[2].code, savor::PSOpCode::APPLY_INPUT_FROM);
    EXPECT_EQ(
        script.ops[2].key.id,
        savor::context::key::navigation::NEUTRAL_INPUT);
    EXPECT_EQ(
        script.ops[3].code,
        savor::PSOpCode::RECORD_CURRENT_PC_TO);
    EXPECT_EQ(
        script.ops[3].key.id,
        savor::context::key::navigation::ENTRY_PC);
    EXPECT_EQ(
        script.ops[4].code,
        savor::PSOpCode::RECORD_CURRENT_BP);

    const auto& immediate = script.ops[5];
    ASSERT_EQ(immediate.code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(
        immediate.jcc.key,
        savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(immediate.jcc.cmp, savor::PSCmp::EQ);
    EXPECT_EQ(
        immediate.jcc.imm,
        bp::navigation::NavigationContextInitialPlayerInputReady);

    EXPECT_EQ(script.ops[6].code, savor::PSOpCode::RUN_UNTIL_BP);

    EXPECT_EQ(
        std::count_if(
            script.ops.begin(),
            script.ops.end(),
            [](const savor::PSOp& op) {
                return op.code
                    == savor::PSOpCode::CAPTURE_SNAPSHOT;
            }),
        0);
    EXPECT_EQ(
        std::count_if(
            script.ops.begin(),
            script.ops.end(),
            [](const savor::PSOp& op) {
                return op.code
                    == savor::PSOpCode::SAVE_SAVESTATE_FROM;
            }),
        1);
    EXPECT_EQ(
        std::count_if(
            script.ops.begin(),
            script.ops.end(),
            [](const savor::PSOp& op) {
                return IsInputMacroAdapterOpcode(op.code);
            }),
        0);
}

TEST(NavigationContextFramework, CaptureIsQualifiedBeforeContextAndSavestate)
{
    const auto script =
        phase::navigation::ctx::MakeNavigationContextProgram();
    const auto capture =
        FindOpcode(script, savor::PSOpCode::GET_NAVIGATION_CONTEXT);
    const auto save =
        FindOpcode(script, savor::PSOpCode::SAVE_SAVESTATE_FROM);
    const auto emit =
        FindOpcode(script, savor::PSOpCode::EMIT_RESULT);
    const auto success =
        FindOpcode(script, savor::PSOpCode::RETURN_RESULT);
    ASSERT_LT(capture, script.ops.size());
    ASSERT_LT(save, script.ops.size());
    ASSERT_LT(emit, script.ops.size());
    ASSERT_LT(success, script.ops.size());
    EXPECT_LT(capture, save);
    EXPECT_LT(save, emit);
    EXPECT_LT(emit, success);

    ASSERT_GE(capture, 2u);
    const auto& key_check = script.ops[capture - 4];
    const auto& pc_check = script.ops[capture - 3];
    EXPECT_EQ(key_check.code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(
        key_check.jcc.key,
        savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(key_check.jcc.cmp, savor::PSCmp::NE);
    EXPECT_EQ(
        key_check.jcc.imm,
        bp::navigation::NavigationContextInitialPlayerInputReady);
    EXPECT_EQ(pc_check.code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(
        pc_check.jcc.key,
        savor::context::key::core::RUN_HIT_PC);
    EXPECT_EQ(pc_check.jcc.cmp, savor::PSCmp::NE);
    EXPECT_EQ(pc_check.jcc.imm, soa::navigation::ctx::CapturePc);

    ASSERT_GT(save, 0u);
    const auto& prearmed_failure = script.ops[save - 1];
    ASSERT_EQ(prearmed_failure.code, savor::PSOpCode::SET_U32);
    EXPECT_EQ(
        prearmed_failure.keyimm.key,
        savor::context::key::navigation::FAILURE);
    EXPECT_EQ(
        prearmed_failure.keyimm.imm,
        static_cast<std::uint32_t>(
            phase::navigation::ctx::FailureCode::SavestateFailed));
    ASSERT_LT(save + 1, script.ops.size());
    EXPECT_EQ(script.ops[save + 1].code, savor::PSOpCode::SET_U32);
    EXPECT_EQ(
        script.ops[save + 1].keyimm.imm,
        static_cast<std::uint32_t>(
            phase::navigation::ctx::FailureCode::None));
    EXPECT_EQ(
        script.ops[save].key.id,
        savor::context::key::navigation::OUTPUT_SAVESTATE_PATH);
    EXPECT_EQ(
        script.ops[emit].key.id,
        savor::context::key::navigation::CTX_BLOB);
    EXPECT_EQ(
        script.ops[success].keyimm.key,
        savor::context::key::navigation::OUTCOME);
    EXPECT_EQ(
        script.ops[success].keyimm.imm,
        static_cast<std::uint32_t>(
            phase::navigation::ctx::Outcome::Completed));
}

TEST(NavigationContextProgramExecution, ImmediateCaptureSkipsGuestRun)
{
    ScriptHostPlan host{};
    host.entry_pc = soa::navigation::ctx::CapturePc;
    const auto run = ExecuteNavigationScript(host);

    EXPECT_TRUE(run.returned);
    EXPECT_FALSE(run.dispatch_failed);
    EXPECT_TRUE(run.result_ok);
    EXPECT_EQ(run.apply_input_calls, 1);
    EXPECT_EQ(run.run_calls, 0);
    EXPECT_EQ(run.get_context_calls, 1);
    EXPECT_EQ(run.save_calls, 1);
    EXPECT_EQ(run.emit_calls, 1);
    EXPECT_EQ(
        ReadU32(
            run.context,
            savor::context::key::navigation::ENTRY_PC),
        soa::navigation::ctx::CapturePc);
    EXPECT_EQ(
        ReadU32(
            run.context,
            savor::context::key::navigation::OUTCOME),
        static_cast<std::uint32_t>(
            phase::navigation::ctx::Outcome::Completed));
}

TEST(NavigationContextProgramExecution, ArbitraryEntryRunsToCapture)
{
    for (const std::uint32_t entry_pc : {
             0x800e3694u,
             0x801012b4u,
             0x80000000u,
         }) {
        ScriptHostPlan host{};
        host.entry_pc = entry_pc;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.returned) << std::hex << entry_pc;
        EXPECT_FALSE(run.dispatch_failed) << std::hex << entry_pc;
        EXPECT_TRUE(run.result_ok) << std::hex << entry_pc;
        EXPECT_EQ(run.run_calls, 1) << std::hex << entry_pc;
        EXPECT_EQ(run.get_context_calls, 1);
        EXPECT_EQ(run.save_calls, 1);
        EXPECT_EQ(run.emit_calls, 1);
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::ENTRY_PC),
            entry_pc);
    }
}

TEST(NavigationContextProgramExecution, RunFailuresStopBeforeCapture)
{
    struct Case {
        savor::RunToBpOutcome outcome;
        phase::navigation::ctx::FailureCode failure;
    };
    for (const auto& test : {
             Case{
                 savor::RunToBpOutcome::MovieEnded,
                 phase::navigation::ctx::FailureCode::UnexpectedStop},
             Case{
                 savor::RunToBpOutcome::Aborted,
                 phase::navigation::ctx::FailureCode::HostFailure},
         }) {
        ScriptHostPlan host{};
        host.run_outcome = test.outcome;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.returned);
        EXPECT_FALSE(run.result_ok);
        EXPECT_EQ(run.run_calls, 1);
        EXPECT_EQ(run.get_context_calls, 0);
        EXPECT_EQ(run.save_calls, 0);
        EXPECT_EQ(run.emit_calls, 0);
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::OUTCOME),
            static_cast<std::uint32_t>(
                phase::navigation::ctx::Outcome::Failed));
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::FAILURE),
            static_cast<std::uint32_t>(test.failure));
    }
}

TEST(NavigationContextProgramExecution, UnexpectedKeyAndPcFailClosed)
{
    {
        ScriptHostPlan host{};
        host.run_hit_key = bp::battle::TurnIsReady;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.returned);
        EXPECT_FALSE(run.result_ok);
        EXPECT_EQ(run.get_context_calls, 0);
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::FAILURE),
            static_cast<std::uint32_t>(
                phase::navigation::ctx::FailureCode::UnexpectedStop));
    }
    {
        ScriptHostPlan host{};
        host.run_hit_pc = soa::navigation::ctx::CapturePc + 4u;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.returned);
        EXPECT_FALSE(run.result_ok);
        EXPECT_EQ(run.get_context_calls, 0);
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::FAILURE),
            static_cast<std::uint32_t>(
                phase::navigation::ctx::FailureCode::
                    CapturePcMismatch));
    }
}

TEST(NavigationContextProgramExecution, ContextAndSavestateFailuresPublishNothing)
{
    {
        ScriptHostPlan host{};
        host.get_context_succeeds = false;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.dispatch_failed);
        EXPECT_FALSE(run.returned);
        EXPECT_EQ(run.get_context_calls, 1);
        EXPECT_EQ(run.save_calls, 0);
        EXPECT_EQ(run.emit_calls, 0);
    }
    {
        ScriptHostPlan host{};
        host.save_succeeds = false;
        const auto run = ExecuteNavigationScript(host);
        EXPECT_TRUE(run.dispatch_failed);
        EXPECT_FALSE(run.returned);
        EXPECT_EQ(run.get_context_calls, 1);
        EXPECT_EQ(run.save_calls, 1);
        EXPECT_EQ(run.emit_calls, 0);
        EXPECT_EQ(
            ReadU32(
                run.context,
                savor::context::key::navigation::FAILURE),
            static_cast<std::uint32_t>(
                phase::navigation::ctx::FailureCode::
                    SavestateFailed));
    }
}

TEST(NavigationContextFramework, ProgramRegistryBuildsAndDecodesKindTen)
{
    EXPECT_EQ(savor::PK_NavigationContextRunner, 10);

    const auto program = savor::programs::build_main_program(
        savor::PK_NavigationContextRunner);
    EXPECT_EQ(
        program.canonical_bp_keys,
        (std::vector<BPKey>{
            bp::navigation::NavigationContextInitialPlayerInputReady}));
    EXPECT_FALSE(program.ops.empty());

    std::vector<std::uint8_t> payload;
    ASSERT_TRUE(phase::navigation::ctx::encode_payload(
        {
            .output_savestate_path = "navigation-context.sav",
        },
        payload));
    savor::PSContext context;
    ASSERT_TRUE(savor::programs::decode_payload_for(
        savor::PK_NavigationContextRunner,
        payload,
        context));

    std::string output_path;
    ASSERT_TRUE(context.get(
        savor::context::key::navigation::OUTPUT_SAVESTATE_PATH,
        output_path));
    EXPECT_EQ(output_path, "navigation-context.sav");
}

TEST(NavigationContextFramework, CliAcceptsAnyPositiveStateDbSavestateId)
{
    savor::e2e::CliOptions options;
    std::string error;
    ASSERT_TRUE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "navigation_context",
        "--iso", ".",
        "--dolphin-base-dir", ".",
        "--source-savestate-id", "77",
    }, &options, &error)) << error;
    ASSERT_EQ(options.scenarios.size(), 1u);
    EXPECT_EQ(options.scenarios.front(), "navigation_context");
    EXPECT_EQ(options.source_savestate_id, 77);
    EXPECT_TRUE(options.savestate_file.empty());

    error.clear();
    EXPECT_FALSE(ParseTestArgs({
        "SavorE2E",
        "--scenario", "navigation_context",
        "--iso", ".",
        "--dolphin-base-dir", ".",
    }, &options, &error));
    EXPECT_NE(error.find("--source-savestate-id"), std::string::npos);
}
