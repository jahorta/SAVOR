#include <gtest/gtest.h>

#include "Runner/Breakpoints/BpRegistry.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceCapabilityPacks.h"
#include "Runner/Runtime/ProgramRuntime/Capabilities/SourceReducers.h"
#include "Runner/Runtime/ProgramRuntime/Composition/CompositionSupport.h"
#include "Runner/Runtime/ProgramRuntime/Registry/CanonicalActionCatalog.h"
#include "Core/Input/SoaBattle/ActionLibrary.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace {

using namespace savor::runtime::program;
using namespace savor::runtime::program::capabilities;

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
    ASSERT_EQ(catalog.manifests.size(), 4u);
    const auto& field = Manifest(catalog, "soa.field");
    const auto& battle = Manifest(catalog, "soa.battle");
    const auto& navigation = Manifest(catalog, "soa.navigation");

    EXPECT_EQ(field.semantic_points.size(), 3u);
    EXPECT_EQ(battle.semantic_points.size(), 64u);
    EXPECT_EQ(navigation.semantic_points.size(), 1u);
    EXPECT_EQ(field.address_symbols.size(), 7u);
    EXPECT_EQ(battle.address_symbols.size(), 9u);
    EXPECT_EQ(navigation.address_symbols.size(), 5u);

    for (const auto* manifest : {&field, &battle, &navigation})
    {
        EXPECT_EQ(manifest->compatibility, SupportedSoaUsaCompatibility());
        EXPECT_TRUE(std::ranges::none_of(
            manifest->semantic_points,
            [](const SemanticPointDescriptor& point)
            {
                return point.kind ==
                        SemanticPointPhysicalKind::ProgramCounter &&
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
    EXPECT_NE(std::ranges::find(
        navigation.semantic_points,
        0x80111770u,
        &SemanticPointDescriptor::pc), navigation.semantic_points.end());
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
    EXPECT_EQ(packs.size(), 4u);

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
            CanonicalReducer::BattleMaterializeTurnInput));
    ASSERT_NE(battle_action, nullptr);
    ASSERT_NE(navigation_action, nullptr);
    ASSERT_NE(reducer, nullptr);
    EXPECT_EQ(battle_action->providing_pack, BattlePackIdentity());
    EXPECT_EQ(navigation_action->providing_pack, NavigationPackIdentity());
    EXPECT_EQ(reducer->providing_pack, BattlePackIdentity());
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

TEST(CapabilityPackSources, BattleMaterializerReducerIsPureAndDeterministic)
{
    soa::battle::ctx::BattleContext context{};
    for (std::size_t slot = 0; slot < 4; ++slot)
    {
        context.slots_[slot].present = 1;
        context.slots_[slot].is_player = 1;
        context.slots_[slot].is_alive = 1;
    }
    context.slots_[4].present = 1;
    context.slots_[4].is_alive = 1;

    soa::battle::actions::BattleTurnExecutionSpec specification{};
    specification.commands.push_back({
        .actor_slot = 0,
        .macro = soa::battle::actions::BattleAction::Defend,
    });

    const std::array inputs{
        EncodeBattleContextValue(context),
        EncodeBattleTurnExecutionSpecValue(specification),
    };
    std::string diagnostic;
    const auto first = InvokeSourceReducer(
        CanonicalReducerIdentity(
            CanonicalReducer::BattleMaterializeTurnInput),
        inputs,
        &diagnostic);
    ASSERT_TRUE(first.has_value()) << diagnostic;
    const auto second = InvokeSourceReducer(
        CanonicalReducerIdentity(
            CanonicalReducer::BattleMaterializeTurnInput),
        inputs,
        &diagnostic);
    ASSERT_TRUE(second.has_value()) << diagnostic;
    EXPECT_EQ(*first, *second);

    bool success = false;
    savor::ControllerInputSequence actual;
    std::string domain_diagnostic;
    ASSERT_TRUE(DecodeTurnInputMaterializationValue(
        *first,
        success,
        actual,
        &domain_diagnostic));
    EXPECT_TRUE(success);
    EXPECT_TRUE(domain_diagnostic.empty());

    savor::ControllerInputSequence expected;
    soa::battle::actions::MaterializeErr error{};
    ASSERT_TRUE(soa::battle::actions::MaterializeBattleTurnInputs(
        context,
        specification,
        expected,
        error));
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index)
    {
        EXPECT_EQ(actual[index].buttons, expected[index].buttons);
        EXPECT_EQ(actual[index].main_x, expected[index].main_x);
        EXPECT_EQ(actual[index].main_y, expected[index].main_y);
        EXPECT_EQ(actual[index].c_x, expected[index].c_x);
        EXPECT_EQ(actual[index].c_y, expected[index].c_y);
        EXPECT_EQ(actual[index].trig_l, expected[index].trig_l);
        EXPECT_EQ(actual[index].trig_r, expected[index].trig_r);
    }
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
            CanonicalReducer::BattleMaterializeTurnInput),
        inputs,
        &diagnostic));
    EXPECT_EQ(
        diagnostic,
        "materialize_turn_input received malformed typed input");
}

} // namespace
