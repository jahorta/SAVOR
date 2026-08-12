#include <gtest/gtest.h>

#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Runner/Runtime/DerivedState/DerivedStateRegistry.h"
#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;
namespace derived = savor::runtime::derived;

const CapabilityPackManifest& Manifest(
    const SourceCapabilityPackCatalog& catalog,
    std::string_view id)
{
    return *std::ranges::find(
        catalog.manifests,
        id,
        [](const CapabilityPackManifest& value)
        {
            return std::string_view(value.identity.canonical_id);
        });
}

TEST(CapabilityPackSources, CatalogIsStableSourceBackedAndJitGuardIsAbsent)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    const RuntimeCompatibility supported =
        SupportedSoaUsaCompatibility();
    EXPECT_EQ(supported.game_id, "GEAE8P");
    EXPECT_EQ(
        supported.executable_identity,
        "soal-usa.GEAE8E");
    ASSERT_EQ(catalog.manifests.size(), 5u);
    const auto& field = Manifest(catalog, "soa.field");
    const auto& battle = Manifest(catalog, "soa.battle");
    const auto& battle_command = Manifest(
        catalog, "soa.battle.command");
    const auto& navigation = Manifest(catalog, "soa.navigation");

    EXPECT_EQ(field.semantic_points.size(), 3u);
    EXPECT_EQ(battle.semantic_points.size(), 10u);
    EXPECT_TRUE(navigation.semantic_points.empty());
    EXPECT_EQ(field.address_symbols.size(), 7u);
    EXPECT_EQ(battle.address_symbols.size(), 9u);
    EXPECT_EQ(navigation.address_symbols.size(), 5u);

    for (const auto* manifest : {
             &field, &battle, &battle_command, &navigation})
    {
        EXPECT_EQ(manifest->compatibility, supported);
        EXPECT_TRUE(std::ranges::none_of(
            manifest->semantic_points,
            [](const SemanticPointDescriptor& point)
            {
                return point.kind ==
                        SemanticPointKind::ProgramCounter &&
                    point.pc == 0;
            }));
        EXPECT_TRUE(std::ranges::none_of(
            manifest->semantic_points,
            [](const SemanticPointDescriptor& point)
            {
                return point.pc == 0x801DC288u;
            }));
    }

    EXPECT_NE(std::ranges::find(
        field.semantic_points,
        0x80101e48u,
        &SemanticPointDescriptor::pc), field.semantic_points.end());
    EXPECT_NE(std::ranges::find(
        field.semantic_points,
        0x801012b4u,
        &SemanticPointDescriptor::pc), field.semantic_points.end());
    EXPECT_NE(std::ranges::find(
        battle.semantic_points,
        0x80071740u,
        &SemanticPointDescriptor::pc), battle.semantic_points.end());
}

TEST(CapabilityPackSources, RegistersExactSchemasQueriesActionsAndReducer)
{
    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);

    const auto registered = RegisterSourceCapabilityPacks(
        schemas,
        actions,
        packs);
    ASSERT_TRUE(registered.success) << registered.error.message;
    EXPECT_EQ(packs.size(), 5u);

    const auto* battle = packs.Resolve(BattlePackIdentity());
    const auto* navigation = packs.Resolve(NavigationPackIdentity());
    ASSERT_NE(battle, nullptr);
    ASSERT_NE(navigation, nullptr);
    ASSERT_EQ(battle->coherent_queries.size(), 1u);
    ASSERT_EQ(navigation->coherent_queries.size(), 1u);
    EXPECT_EQ(
        battle->coherent_queries.front().canonical_id,
        "soa.battle.query.BattleContext");
    EXPECT_EQ(
        navigation->coherent_queries.front().canonical_id,
        "soa.navigation.query.NavigationContext");
    EXPECT_EQ(battle->coherent_queries.front().maximum_guest_reads, 64u);
    EXPECT_EQ(
        navigation->coherent_queries.front().maximum_guest_reads,
        21u);

    const auto* battle_action = actions.ResolveAction(
        BattleCaptureContextActionIdentity());
    const auto* navigation_action = actions.ResolveAction(
        NavigationCaptureContextActionIdentity());
    const auto* reducer = actions.ResolveReducer(
        CanonicalReducerIdentity(
            CanonicalReducer::BattlePrepareCommandInteraction));
    ASSERT_NE(battle_action, nullptr);
    ASSERT_NE(navigation_action, nullptr);
    ASSERT_NE(reducer, nullptr);
    EXPECT_EQ(battle_action->providing_pack, BattlePackIdentity());
    EXPECT_EQ(navigation_action->providing_pack, NavigationPackIdentity());
    EXPECT_EQ(reducer->providing_pack, BattleCommandPackIdentity());
    EXPECT_TRUE(reducer->permitted_actions.empty());
}

TEST(CapabilityPackSources, ExactCompatibilityAndConstructionAreDeterministic)
{
    const auto first = BuildSourceCapabilityPackCatalog();
    const auto second = BuildSourceCapabilityPackCatalog();
    EXPECT_EQ(first.schemas, second.schemas);
    EXPECT_EQ(first.actions, second.actions);
    EXPECT_EQ(first.reducers, second.reducers);
    EXPECT_EQ(first.manifests, second.manifests);

    TypeSchemaRegistry schemas;
    ActionRegistry actions(&schemas);
    CapabilityPackRegistry packs(&schemas, &actions);
    ASSERT_TRUE(RegisterSourceCapabilityPacks(
        schemas,
        actions,
        packs).success);

    RegistryError error;
    const auto closure = packs.ResolveClosure(
        {FieldPackIdentity(), BattlePackIdentity()},
        SupportedSoaUsaCompatibility(),
        &error);
    ASSERT_TRUE(closure.has_value()) << error.message;
    EXPECT_EQ(closure->dependency_order.size(), 3u);

    auto incompatible = SupportedSoaUsaCompatibility();
    incompatible.game_id = "OTHER";
    EXPECT_FALSE(packs.ResolveClosure(
        {BattlePackIdentity()},
        incompatible,
        &error));
    EXPECT_EQ(error.code, RegistryErrorCode::CompatibilityMismatch);
}

TEST(CapabilityPackSources, PublishedIdentitiesCoverExactDescriptorsAndManifests)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    for (const ActionDescriptor& descriptor : catalog.actions)
    {
        EXPECT_EQ(
            descriptor.identity.signature_hash,
            ComputeActionDescriptorContractHash(descriptor))
            << descriptor.identity.canonical_id;
    }
    for (const ReducerDescriptor& descriptor : catalog.reducers)
    {
        EXPECT_EQ(
            descriptor.identity.signature_hash,
            ComputeReducerDescriptorContractHash(descriptor))
            << descriptor.identity.canonical_id;
    }
    for (const CapabilityPackManifest& manifest :
         catalog.manifests)
    {
        EXPECT_EQ(
            manifest.identity.manifest_hash,
            ComputeCapabilityPackManifestContractHash(manifest))
            << manifest.identity.canonical_id;
    }
}

TEST(CapabilityPackSources, InventoryOrderDoesNotChangeManifestIdentity)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    for (const CapabilityPackManifest& manifest :
         catalog.manifests)
    {
        CapabilityPackManifest reordered = manifest;
        std::ranges::reverse(reordered.dependencies);
        std::ranges::reverse(reordered.schemas);
        std::ranges::reverse(reordered.semantic_points);
        std::ranges::reverse(reordered.address_symbols);
        std::ranges::reverse(reordered.coherent_queries);
        std::ranges::reverse(reordered.cpu_evaluators);
        std::ranges::reverse(reordered.actions);
        std::ranges::reverse(reordered.reducers);
        for (auto& query : reordered.coherent_queries)
        {
            std::ranges::reverse(query.address_dependencies);
        }
        EXPECT_EQ(
            ComputeCapabilityPackManifestContractHash(reordered),
            manifest.identity.manifest_hash)
            << manifest.identity.canonical_id;
    }
}

TEST(CapabilityPackSources, DoesNotRegisterPlaceholderFamilies)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    EXPECT_TRUE(std::ranges::none_of(
        catalog.manifests,
        [](const CapabilityPackManifest& manifest)
        {
            return manifest.identity.canonical_id == "soa.cutscene" ||
                manifest.identity.canonical_id == "soa.overworld";
        }));
}

TEST(CapabilityPackSources, ReducerRejectsUnknownIdentityAndMalformedGraphs)
{
    const ProgramValueGraph malformed{
        .root = ProgramValueId(1),
        .values = {},
    };
    const std::array inputs{malformed, malformed};
    std::string diagnostic;
    EXPECT_FALSE(InvokeSourceReducer(
        savor::runtime::program::composition::ExactDependency(
            "test.unknown",
            1,
            "test.unknown/1:()"),
        inputs,
        &diagnostic));
    EXPECT_EQ(diagnostic, "unknown source reducer identity");

    EXPECT_FALSE(InvokeSourceReducer(
        CanonicalReducerIdentity(
            CanonicalReducer::BattleCommandInteractionAdvance),
        inputs,
        &diagnostic));
    EXPECT_TRUE(diagnostic.empty());
}

TEST(CapabilityPackSources, BattleCommandReducersHaveOneCanonicalContract)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    const auto& manifest = Manifest(catalog, "soa.battle.command");
    struct ExpectedReducer
    {
        CanonicalReducer reducer;
        std::uint64_t steps;
        std::uint64_t bytes;
    };
    constexpr std::array expected{
        ExpectedReducer{
            CanonicalReducer::BattlePrepareCommandInteraction,
            100000,
            4 * 1024 * 1024},
        ExpectedReducer{
            CanonicalReducer::BattleCommandInteractionInitialize,
            1,
            4 * 1024 * 1024},
        ExpectedReducer{
            CanonicalReducer::BattleCommandInteractionAdvance,
            1000,
            4 * 1024 * 1024},
        ExpectedReducer{
            CanonicalReducer::BattleCommandInteractionCompleteSegment,
            1,
            64 * 1024},
        ExpectedReducer{
            CanonicalReducer::BattleCommandInteractionFinalize,
            1,
            16},
    };

    EXPECT_EQ(std::ranges::count_if(
        catalog.reducers,
        [&](const ReducerDescriptor& descriptor) {
            return descriptor.providing_pack == manifest.identity;
        }), expected.size());
    ASSERT_EQ(manifest.reducers.size(), expected.size());
    for (const ExpectedReducer& item : expected)
    {
        const ExactDependencyIdentity identity =
            CanonicalReducerIdentity(item.reducer);
        const auto descriptor = std::ranges::find(
            catalog.reducers,
            identity,
            &ReducerDescriptor::identity);
        ASSERT_NE(descriptor, catalog.reducers.end());
        EXPECT_EQ(CanonicalReducerName(item.reducer), identity.canonical_id);
        EXPECT_EQ(descriptor->providing_pack, manifest.identity);
        EXPECT_NE(
            std::ranges::find(manifest.reducers, identity),
            manifest.reducers.end());
        EXPECT_EQ(descriptor->maximum_steps, item.steps);
        EXPECT_EQ(descriptor->maximum_value_bytes, item.bytes);
    }
    EXPECT_TRUE(
        CanonicalReducerName(static_cast<CanonicalReducer>(0xff)).empty());
}

TEST(CapabilityPackSources, BattleDerivedReducersAndQueriesUseOneExactPack)
{
    const auto catalog = BuildSourceCapabilityPackCatalog();
    const auto& manifest = Manifest(catalog, "soa.battle");
    const auto* block =
        derived::ProductionDerivedStateRegistry().FindBlock(
            derived::kBattleCoreBlockId);
    ASSERT_NE(block, nullptr);
    constexpr std::array<std::string_view, 9> reducers{
        "soa.battle.derived.current_turn",
        "soa.battle.derived.inventory_count",
        "soa.battle.derived.drop_count",
        "soa.battle.derived.player_count",
        "soa.battle.derived.enemy_count",
        "soa.battle.derived.player_min_position",
        "soa.battle.derived.player_max_position",
        "soa.battle.derived.enemy_min_position",
        "soa.battle.derived.enemy_max_position",
    };
    ASSERT_EQ(manifest.reducers.size(), reducers.size());
    ASSERT_EQ(block->reducers.size(), reducers.size());
    std::vector<ExactDependencyIdentity> expected_reducers;
    for (const auto canonical_id : reducers)
    {
        const auto identity = BattleDerivedReducerIdentity(canonical_id);
        expected_reducers.push_back(identity);
        const auto descriptor = std::ranges::find(
            catalog.reducers, identity, &ReducerDescriptor::identity);
        ASSERT_NE(descriptor, catalog.reducers.end());
        EXPECT_EQ(descriptor->providing_pack, manifest.identity);
        EXPECT_EQ(descriptor->maximum_steps, 256u);
        EXPECT_EQ(descriptor->maximum_value_bytes, 64u * 1024u);
        EXPECT_NE(
            std::ranges::find(manifest.reducers, identity),
            manifest.reducers.end());
    }
    EXPECT_EQ(block->reducers, expected_reducers);

    constexpr std::array<std::string_view, 3> actions{
        derived::kBattleQueryTurnEntryActionId,
        derived::kBattleQueryTurnOrderActionId,
        derived::kBattleQueryRewardsActionId,
    };
    for (const auto canonical_id : actions)
    {
        const auto descriptor = std::ranges::find(
            catalog.actions,
            canonical_id,
            [](const ActionDescriptor& value)
            {
                return std::string_view(value.identity.canonical_id);
            });
        ASSERT_NE(descriptor, catalog.actions.end());
        EXPECT_EQ(descriptor->providing_pack, manifest.identity);
        EXPECT_EQ(
            descriptor->required_derived_state_block_id,
            derived::kBattleCoreBlockId);
        EXPECT_NE(
            std::ranges::find(manifest.actions, descriptor->identity),
            manifest.actions.end());
        const auto group = std::ranges::find(
            block->groups,
            descriptor->identity,
            &derived::DerivedStateRefreshGroupDescriptor::query_action);
        ASSERT_NE(group, block->groups.end());
        EXPECT_EQ(descriptor->output_type, TypeRef::Named(group->output_schema));
    }
}

} // namespace
