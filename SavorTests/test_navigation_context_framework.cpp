#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Phases/Programs/NavigationContext/NavigationContextScript.h"
#include "Phases/Programs/ProgramRegistry.h"
#include "Runner/IPC/Wire.h"
#include "Runner/Script/CtxRegistry.h"

namespace {

constexpr BPKey kCaptureBreakpoint = static_cast<BPKey>(0x7ffe);

bool IsInputMacroOpcode(savor::PSOpCode code)
{
    switch (code) {
    case savor::PSOpCode::APPLY_INPUT_FROM:
    case savor::PSOpCode::APPLY_BATTLE_INPUTPLAN_FRAMES:
    case savor::PSOpCode::BUILD_TURN_INPUTPLAN_FROM_BATTLE_PATH:
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

} // namespace

TEST(NavigationContextFramework, RegistersContextBlobKey)
{
    using namespace savor::context::key;

    EXPECT_EQ(NAVIGATION_CONTEXT_MIN, static_cast<KeyId>(0x0700));
    EXPECT_EQ(NAVIGATION_CONTEXT_MAX, static_cast<KeyId>(0x07ff));
    EXPECT_EQ(navigation::CTX_BLOB, static_cast<KeyId>(0x0700));

    std::string registry_error;
    ASSERT_TRUE(validate_registry(&registry_error)) << registry_error;
    EXPECT_EQ(name_for_id(navigation::CTX_BLOB), "navigation.CTX_BLOB");

    KeyId resolved = 0;
    ASSERT_TRUE(id_for_name("navigation.CTX_BLOB", resolved));
    EXPECT_EQ(resolved, navigation::CTX_BLOB);
}

TEST(NavigationContextFramework, ProgramHasExactMinimalCaptureShape)
{
    const auto script =
        phase::navigation::ctx::MakeNavigationContextProgram(kCaptureBreakpoint);

    ASSERT_EQ(script.canonical_bp_keys,
        (std::vector<BPKey>{kCaptureBreakpoint}));
    EXPECT_TRUE(script.gated_bp_keys.empty());
    ASSERT_EQ(script.ops.size(), 8u);

    EXPECT_EQ(script.ops[0].code, savor::PSOpCode::ARM_PHASE_BPS_ONCE);
    EXPECT_EQ(script.ops[1].code, savor::PSOpCode::LOAD_SNAPSHOT);
    EXPECT_EQ(script.ops[2].code, savor::PSOpCode::RECORD_CURRENT_BP);
    EXPECT_EQ(script.ops[3].code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(script.ops[4].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(script.ops[5].code, savor::PSOpCode::LABEL);
    EXPECT_EQ(script.ops[6].code, savor::PSOpCode::GET_NAVIGATION_CONTEXT);
    EXPECT_EQ(script.ops[7].code, savor::PSOpCode::EMIT_RESULT);

    EXPECT_EQ(script.ops[7].key.id,
        savor::context::key::navigation::CTX_BLOB);

    EXPECT_EQ(std::count_if(script.ops.begin(), script.ops.end(),
        [](const savor::PSOp& op) {
            return op.code == savor::PSOpCode::CAPTURE_SNAPSHOT;
        }), 0);
    EXPECT_EQ(std::count_if(script.ops.begin(), script.ops.end(),
        [](const savor::PSOp& op) {
            return op.code == savor::PSOpCode::SAVE_SAVESTATE_FROM;
        }), 0);
    EXPECT_EQ(std::count_if(script.ops.begin(), script.ops.end(),
        [](const savor::PSOp& op) { return IsInputMacroOpcode(op.code); }), 0);
}

TEST(NavigationContextFramework, ShortCircuitsRunWhenAlreadyAtCaptureBreakpoint)
{
    const auto script =
        phase::navigation::ctx::MakeNavigationContextProgram(kCaptureBreakpoint);
    ASSERT_EQ(script.ops.size(), 8u);

    const auto& branch = script.ops[3];
    const auto& run = script.ops[4];
    const auto& capture_label = script.ops[5];

    ASSERT_EQ(branch.code, savor::PSOpCode::GOTO_IF);
    EXPECT_EQ(branch.jcc.key, savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(branch.jcc.cmp, savor::PSCmp::EQ);
    EXPECT_EQ(branch.jcc.imm, static_cast<std::uint32_t>(kCaptureBreakpoint));
    EXPECT_EQ(branch.jcc.name, capture_label.label.name);
    EXPECT_EQ(run.code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(capture_label.code, savor::PSOpCode::LABEL);
    EXPECT_FALSE(capture_label.label.name.empty());
}

TEST(NavigationContextFramework, ReservedProgramKindRemainsUnregistered)
{
    EXPECT_EQ(savor::PK_NavigationContextRunner, 10);

    const auto program = savor::programs::build_main_program(
        savor::PK_NavigationContextRunner);
    EXPECT_TRUE(program.canonical_bp_keys.empty());
    EXPECT_TRUE(program.gated_bp_keys.empty());
    EXPECT_TRUE(program.ops.empty());

    const std::vector<std::uint8_t> payload{
        savor::PK_NavigationContextRunner,
    };
    savor::PSContext context;
    EXPECT_FALSE(savor::programs::decode_payload_for(
        savor::PK_NavigationContextRunner,
        payload,
        context));
    EXPECT_TRUE(context.empty());
}
