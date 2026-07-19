#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Authoring/RuntimeSymbolAuthoringService.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Input/SoaBattle/BattleCommandCodec.h"
#include "Core/Memory/Soa/SoaAddrRegistry.h"
#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Script/CtxRegistry.h"
#include "Runner/Script/PSContextCodec.h"
#include "Runner/Symbols/RuntimeSymbolRegistry.h"

#include "common/SqliteDbFixture.h"

namespace {

bool SameBattlePath(
    const soa::battle::actions::BattlePath& lhs,
    const soa::battle::actions::BattlePath& rhs)
{
    if (lhs.size() != rhs.size()) return false;
    for (size_t i = 0; i < lhs.size(); ++i) {
        if (lhs[i].fake_attack_count != rhs[i].fake_attack_count) return false;
        if (lhs[i].commands.size() != rhs[i].commands.size()) return false;
        for (size_t j = 0; j < lhs[i].commands.size(); ++j) {
            const auto& a = lhs[i].commands[j];
            const auto& b = rhs[i].commands[j];
            if (a.actor_slot != b.actor_slot) return false;
            if (a.macro != b.macro) return false;
            if (a.params.target_slot != b.params.target_slot) return false;
            if (a.params.item_id != b.params.item_id) return false;
        }
    }
    return true;
}

std::string ValidSymbolPackJson(std::string_view extra_name = "Runtime Symbols")
{
    return std::string(R"JSON({
  "schema": "savor.runtime-symbol-pack",
  "version": 1,
  "id": "user.pack.tests",
  "name": ")JSON") + std::string(extra_name) + R"JSON(",
  "context_keys": [
    {"id": "user.ctx.test.observed_rng", "name": "Observed RNG", "type": "u32"}
  ],
  "addresses": [
    {"id": "user.addr.test.rng_seed_shadow", "name": "RNG Seed Shadow", "region": "MEM1", "address": "0x803469A8", "width": 4, "type": "u32"}
  ],
  "breakpoints": [
    {"id": "user.bp.test.rng_probe", "name": "RNG Probe", "address_id": "user.addr.test.rng_seed_shadow", "kind": "execute", "enabled": true}
  ]
})JSON";
}

} // namespace

TEST(RuntimeRegistryNaming, BuiltinLookupParity)
{
    savor::context::key::KeyId key = 0;
    ASSERT_TRUE(savor::context::key::CtxRegistry::id_for_name("core.run.hit_bp", key));
    EXPECT_EQ(key, savor::context::key::core::RUN_HIT_BP_KEY);
    EXPECT_EQ(savor::context::key::CtxRegistry::name_for_id(key), "core.run.hit_bp");

    EXPECT_TRUE(addr::AddrRegistry::exists(addr::core::RNG_SEED));
    EXPECT_EQ(addr::AddrRegistry::base(addr::core::RNG_SEED), 0x803469A8u);
    EXPECT_STREQ(addr::AddrRegistry::name(addr::core::RNG_SEED), "core.RNG_SEED");

    const BPAddr* bp = bp::BpRegistry::FindRuntime(bp::battle::TurnIsReady);
    ASSERT_NE(bp, nullptr);
    EXPECT_STREQ(bp->name, "TurnIsReady");
    EXPECT_EQ(bp::BpRegistry::FindRuntime(bp->pc), bp);
}

TEST(BreakpointRegistry, InternalInputMacroBreakpointsAreConsumerScoped)
{
    const auto all = bp::BpRegistry::AllRuntime();
    const auto predicate_bps = bp::BpRegistry::ForConsumer(BreakpointConsumer::Predicate);
    const auto macro_bps = bp::BpRegistry::ForConsumer(BreakpointConsumer::InputMacroControl);
    std::size_t input_macro_count = 0;

    for (const auto& record : all) {
        const bool macro_named = std::string_view(record.name).rfind("BattleMacro", 0) == 0;
        if (macro_named) {
            ++input_macro_count;
            EXPECT_EQ(record.visibility, BreakpointVisibility::Internal);
            EXPECT_EQ(record.owner, BreakpointOwner::InputMacro);
            EXPECT_FALSE(bp::BpRegistry::IsAllowed(record.key, BreakpointConsumer::Predicate));
            EXPECT_FALSE(bp::BpRegistry::IsAllowed(record.key, BreakpointConsumer::CaptureProfile));
            EXPECT_FALSE(bp::BpRegistry::IsAllowed(record.key, BreakpointConsumer::UserScript));
            EXPECT_TRUE(bp::BpRegistry::IsAllowed(record.key, BreakpointConsumer::PhaseControl));
            EXPECT_TRUE(bp::BpRegistry::IsAllowed(record.key, BreakpointConsumer::InputMacroControl));
            EXPECT_FALSE(bp::BpRegistry::IsAllowedPc(record.pc, BreakpointConsumer::Predicate));
            EXPECT_TRUE(bp::BpRegistry::IsAllowedPc(record.pc, BreakpointConsumer::InputMacroControl));
        } else {
            EXPECT_EQ(record.visibility, BreakpointVisibility::PlayerVisible);
            EXPECT_EQ(record.owner, BreakpointOwner::Shared);
        }
    }

    EXPECT_EQ(input_macro_count, 22u);
    EXPECT_EQ(std::count_if(predicate_bps.begin(), predicate_bps.end(), [](const BPAddr& record) {
        return record.visibility == BreakpointVisibility::Internal;
    }), 0);
    EXPECT_EQ(std::count_if(macro_bps.begin(), macro_bps.end(), [](const BPAddr& record) {
        return record.visibility == BreakpointVisibility::Internal;
    }), static_cast<std::ptrdiff_t>(input_macro_count));
}

TEST(BreakpointRegistry, InternalAndPlayerVisibleBreakpointsDoNotSharePcs)
{
    const auto all = bp::BpRegistry::AllRuntime();
    for (const auto& internal_bp : all) {
        if (internal_bp.visibility != BreakpointVisibility::Internal) continue;
        for (const auto& public_bp : all) {
            if (public_bp.visibility != BreakpointVisibility::PlayerVisible) continue;
            EXPECT_NE(internal_bp.pc, public_bp.pc);
        }
    }
}

TEST(RuntimeSymbolRegistry, MergesBuiltinsAndCustomSymbols)
{
    auto registry = savor::symbols::RuntimeSymbolRegistry::BuiltIns();

    ASSERT_NE(registry.FindContext("builtin.ctx.core.run.hit_bp"), nullptr);
    ASSERT_NE(registry.FindAddress("builtin.addr.core.RNG_SEED"), nullptr);
    ASSERT_NE(registry.FindBreakpoint("builtin.bp.battle.TurnIsReady"), nullptr);
    EXPECT_EQ(registry.FindBreakpoint("builtin.bp.battle.BattleMacroInputReadyGate"), nullptr);

    std::string error;
    savor::symbols::ContextSymbol ctx;
    ctx.stable_id = "user.ctx.test.value";
    ctx.name = "Observed Value";
    ctx.type = savor::symbols::ContextValueType::U32;
    ASSERT_TRUE(registry.AddContextSymbol(std::move(ctx), &error)) << error;

    savor::symbols::AddressSymbol address;
    address.stable_id = "user.addr.test.value";
    address.name = "Observed Address";
    address.region = addr::Region::MEM1;
    address.base = 0x81234568u;
    ASSERT_TRUE(registry.AddAddressSymbol(std::move(address), &error)) << error;

    savor::symbols::BreakpointSymbol breakpoint;
    breakpoint.stable_id = "user.bp.test.value";
    breakpoint.name = "Observed Breakpoint";
    breakpoint.address_id = "user.addr.test.value";
    ASSERT_TRUE(registry.AddBreakpointSymbol(std::move(breakpoint), &error)) << error;

    const auto* custom_ctx = registry.FindContext("user.ctx.test.value");
    const auto* custom_addr = registry.FindAddress("user.addr.test.value");
    const auto* custom_bp = registry.FindBreakpoint("user.bp.test.value");
    ASSERT_NE(custom_ctx, nullptr);
    ASSERT_NE(custom_addr, nullptr);
    ASSERT_NE(custom_bp, nullptr);
    EXPECT_GE(custom_ctx->key, savor::symbols::RuntimeSymbolRegistry::CustomContextKeyMin);
    EXPECT_GE(custom_addr->key, savor::symbols::RuntimeSymbolRegistry::CustomAddressKeyMin);
    EXPECT_GE(custom_bp->key, savor::symbols::RuntimeSymbolRegistry::CustomBreakpointKeyMin);
    EXPECT_EQ(registry.MatchBreakpointPc(0x81234568u), custom_bp);
}

TEST(RuntimeSymbolRegistry, RejectsInternalBreakpointIdsAndCustomPcAliases)
{
    auto registry = savor::symbols::RuntimeSymbolRegistry::BuiltIns();
    std::string error;

    savor::symbols::SymbolicPhaseScript symbolic;
    symbolic.canonical_breakpoint_ids.push_back("builtin.bp.battle.BattleMacroInputReadyGate");
    savor::PhaseScript lowered;
    EXPECT_FALSE(registry.LowerSymbolicPhaseScript(symbolic, lowered, &error));
    EXPECT_TRUE(lowered.canonical_bp_keys.empty());

    const auto* internal_bp = bp::BpRegistry::FindRuntime(bp::battle::BattleMacroInputReadyGate);
    ASSERT_NE(internal_bp, nullptr);

    savor::symbols::AddressSymbol address;
    address.stable_id = "user.addr.test.internal_alias";
    address.name = "Unavailable Address";
    address.region = addr::Region::MEM1;
    address.base = internal_bp->pc;
    ASSERT_TRUE(registry.AddAddressSymbol(std::move(address), &error)) << error;

    savor::symbols::BreakpointSymbol breakpoint;
    breakpoint.stable_id = "user.bp.test.internal_alias";
    breakpoint.name = "Unavailable Breakpoint";
    breakpoint.address_id = "user.addr.test.internal_alias";
    error.clear();
    EXPECT_FALSE(registry.AddBreakpointSymbol(std::move(breakpoint), &error));
    EXPECT_EQ(error, "breakpoint address is unavailable");
}

TEST(RuntimeSymbolRegistry, LowersSymbolicScripts)
{
    auto registry = savor::symbols::RuntimeSymbolRegistry::BuiltIns();
    std::string error;

    ASSERT_TRUE(registry.AddContextSymbol({ "user.ctx.test.read", "Read Result", savor::symbols::ContextValueType::U32 }, &error)) << error;
    savor::symbols::AddressSymbol address;
    address.stable_id = "user.addr.test.read_source";
    address.name = "Read Source";
    address.region = addr::Region::MEM1;
    address.base = 0x803469A8u;
    ASSERT_TRUE(registry.AddAddressSymbol(std::move(address), &error)) << error;
    ASSERT_TRUE(registry.AddBreakpointSymbol({ "user.bp.test.read", "Read Breakpoint", "user.addr.test.read_source" }, &error)) << error;

    savor::symbols::SymbolicPhaseScript symbolic;
    symbolic.canonical_breakpoint_ids.push_back("user.bp.test.read");
    symbolic.ops.push_back({ .kind = savor::symbols::SymbolicOp::Kind::RunUntilBp });
    symbolic.ops.push_back({
        .kind = savor::symbols::SymbolicOp::Kind::ReadU32,
        .left_key_id = "user.ctx.test.read",
        .address_id = "user.addr.test.read_source",
    });
    symbolic.ops.push_back({
        .kind = savor::symbols::SymbolicOp::Kind::EmitResult,
        .left_key_id = "user.ctx.test.read",
    });

    savor::PhaseScript lowered;
    ASSERT_TRUE(registry.LowerSymbolicPhaseScript(symbolic, lowered, &error)) << error;
    ASSERT_EQ(lowered.canonical_bp_keys.size(), 1u);
    ASSERT_EQ(lowered.ops.size(), 3u);
    EXPECT_EQ(lowered.ops[0].code, savor::PSOpCode::RUN_UNTIL_BP);
    EXPECT_EQ(lowered.ops[1].code, savor::PSOpCode::READ_U32);
    EXPECT_EQ(lowered.ops[1].rd.addr, 0x803469A8u);
    EXPECT_EQ(lowered.ops[2].code, savor::PSOpCode::EMIT_RESULT);
}

TEST(PSContextCodec, RoundTripsCustomKeysAndRichValues)
{
    savor::PSContext ctx;
    const savor::context::key::KeyId scalar_key = 0x8000;
    const savor::context::key::KeyId string_key = 0x8001;
    const savor::context::key::KeyId frame_key = 0x8002;
    const savor::context::key::KeyId path_key = 0x8003;

    auto frame = savor::GCInputFrame::new_stk_main(120, 136);
    frame.A().R();

    soa::battle::actions::BattlePath path;
    soa::battle::actions::TurnPlan turn;
    turn.fake_attack_count = 2;
    turn.commands.push_back({
        .actor_slot = 1,
        .macro = soa::battle::actions::BattleAction::UseItem,
        .params = { .target_slot = 3, .item_id = 42 },
    });
    path.push_back(turn);

    ctx.emplace(scalar_key, uint32_t{ 12345 });
    ctx.emplace(string_key, std::string{ "custom" });
    ctx.emplace(frame_key, frame);
    ctx.emplace(path_key, path);

    std::vector<uint8_t> bytes;
    ASSERT_TRUE(savor::psctx::encode_numeric(ctx, bytes));

    savor::PSContext decoded;
    ASSERT_TRUE(savor::psctx::decode_numeric(bytes.data(), bytes.size(), decoded));

    uint32_t scalar = 0;
    std::string text;
    savor::GCInputFrame decoded_frame;
    soa::battle::actions::BattlePath decoded_path;
    ASSERT_TRUE(decoded.get(scalar_key, scalar));
    ASSERT_TRUE(decoded.get(string_key, text));
    ASSERT_TRUE(decoded.get(frame_key, decoded_frame));
    ASSERT_TRUE(decoded.get(path_key, decoded_path));
    EXPECT_EQ(scalar, 12345u);
    EXPECT_EQ(text, "custom");
    EXPECT_EQ(decoded_frame, frame);
    EXPECT_TRUE(SameBattlePath(decoded_path, path));
}

TEST(BattleCommandCodec, RoundTripsCommandSetAndExecutionScript)
{
    soa::battle::actions::BattleTurnCommandSet commands{
        {
            .actor_slot = 0,
            .macro = soa::battle::actions::BattleAction::Attack,
            .params = { .target_slot = 4, .item_id = 0xFFFF },
        },
        {
            .actor_slot = 1,
            .macro = soa::battle::actions::BattleAction::Focus,
            .params = { .target_slot = 0xFF, .item_id = 0xFFFF },
        },
    };

    const auto hex = soa::battle::actions::encode_battle_turn_commands_hex(commands);
    const auto decoded = soa::battle::actions::decode_battle_turn_commands_hex(hex);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->size(), commands.size());
    EXPECT_EQ((*decoded)[0].actor_slot, 0);
    EXPECT_EQ((*decoded)[0].macro, soa::battle::actions::BattleAction::Attack);
    EXPECT_EQ((*decoded)[0].params.target_slot, 4);
    EXPECT_EQ((*decoded)[1].macro, soa::battle::actions::BattleAction::Focus);

    soa::battle::actions::BattleExecutionScript script;
    script.push_back(soa::battle::actions::BattleTurnExecutionSpec{
        .fake_attack_count = 3,
        .commands = commands,
    });
    std::vector<std::uint8_t> bytes;
    soa::battle::actions::encode_battle_execution_script_to_buffer(script, bytes);
    soa::battle::actions::BattleExecutionScript decoded_script;
    ASSERT_TRUE(soa::battle::actions::decode_battle_execution_script_from_buffer(bytes, decoded_script));
    ASSERT_EQ(decoded_script.size(), 1u);
    EXPECT_EQ(decoded_script[0].fake_attack_count, 3u);
    ASSERT_EQ(decoded_script[0].commands.size(), 2u);
    EXPECT_EQ(decoded_script[0].commands[0].params.target_slot, 4);
}

TEST(BattleCommandCodec, RejectsMalformedCommandBlob)
{
    EXPECT_FALSE(soa::battle::actions::decode_battle_turn_commands_hex("01000000ff").has_value());
    EXPECT_FALSE(soa::battle::actions::decode_battle_turn_commands_hex("not-hex").has_value());
}

TEST_F(SqliteDbFixture, RuntimeSymbolPackImportExportAndLoad)
{
    savor::db::RuntimeSymbolAuthoringService service(db_);

    savor::db::RuntimeSymbolPackImportResult result;
    std::string error;
    ASSERT_TRUE(service.ImportJson(ValidSymbolPackJson(), &result, &error)) << error;
    EXPECT_EQ(result.context_symbol_count, 1);
    EXPECT_EQ(result.address_symbol_count, 1);
    EXPECT_EQ(result.breakpoint_symbol_count, 1);

    std::string exported;
    ASSERT_TRUE(service.ExportJson("user.pack.tests", 1, exported, &error)) << error;
    EXPECT_NE(exported.find("\"schema\": \"savor.runtime-symbol-pack\""), std::string::npos);
    EXPECT_NE(exported.find("user.bp.test.rng_probe"), std::string::npos);

    auto registry = savor::symbols::RuntimeSymbolRegistry::BuiltIns();
    ASSERT_TRUE(service.LoadCustomSymbols(registry, &error)) << error;
    const auto* bp = registry.FindBreakpoint("user.bp.test.rng_probe");
    ASSERT_NE(bp, nullptr);
    EXPECT_EQ(bp->pc, 0x803469A8u);

    ASSERT_TRUE(service.ImportJson(ValidSymbolPackJson("Replacement"), &result, &error)) << error;
    EXPECT_EQ(result.context_symbol_count, 1);
    EXPECT_EQ(result.address_symbol_count, 1);
    EXPECT_EQ(result.breakpoint_symbol_count, 1);
}

TEST_F(SqliteDbFixture, RuntimeSymbolPackRejectsMissingBreakpointAddress)
{
    savor::db::RuntimeSymbolAuthoringService service(db_);
    const std::string json = R"JSON({
  "schema": "savor.runtime-symbol-pack",
  "version": 1,
  "id": "user.pack.bad",
  "context_keys": [],
  "addresses": [],
  "breakpoints": [
    {"id": "user.bp.bad.missing", "name": "Missing", "address_id": "user.addr.bad.missing", "kind": "execute"}
  ]
})JSON";

    std::string error;
    EXPECT_FALSE(service.ImportJson(json, nullptr, &error));
    EXPECT_NE(error.find("missing address"), std::string::npos);
}
