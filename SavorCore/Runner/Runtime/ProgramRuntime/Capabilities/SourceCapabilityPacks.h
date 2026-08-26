#pragma once

#include "Runner/Runtime/ProgramRuntime/Registry/CapabilityPackRegistry.h"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace savor::runtime::program::capabilities {

inline constexpr std::string_view kSupportedGameId = "GEAE8P";
inline constexpr std::string_view kSupportedExecutableIdentity =
    "soal-usa.GEAE8E";
inline constexpr std::string_view kSupportedAddressMapRevision =
    "savor.builtin-soal-usa-addresses/1";

enum class BattleCommandSegment : std::int64_t
{
    AwaitInputReady = 0,
    FakeAccept = 1,
    FakeBack = 2,
    AttackAccept = 3,
    AttackTargetReady = 4,
    AttackTargetReadyConfirm = 5,
    AttackTargetDown = 6,
    AttackTargetReadyBetween = 7,
    AttackTargetAccept = 8,
    MainMenuMoveUp = 9,
    MainMenuMoveDown = 10,
    MainMenuTransition = 11,
    DirectCommandAccept = 12,
    AwaitNextInputReady = 13,
    AwaitTurnReady = 14,
    Complete = 15,
};

struct BattleCommandSegmentDefinition
{
    BattleCommandSegment segment;
    std::string_view name;
};

inline constexpr std::array<BattleCommandSegmentDefinition, 16>
    kBattleCommandSegments{{
        {BattleCommandSegment::AwaitInputReady, "AwaitInputReady"},
        {BattleCommandSegment::FakeAccept, "FakeAccept"},
        {BattleCommandSegment::FakeBack, "FakeBack"},
        {BattleCommandSegment::AttackAccept, "AttackAccept"},
        {BattleCommandSegment::AttackTargetReady, "AttackTargetReady"},
        {BattleCommandSegment::AttackTargetReadyConfirm, "AttackTargetReadyConfirm"},
        {BattleCommandSegment::AttackTargetDown, "AttackTargetDown"},
        {BattleCommandSegment::AttackTargetReadyBetween, "AttackTargetReadyBetween"},
        {BattleCommandSegment::AttackTargetAccept, "AttackTargetAccept"},
        {BattleCommandSegment::MainMenuMoveUp, "MainMenuMoveUp"},
        {BattleCommandSegment::MainMenuMoveDown, "MainMenuMoveDown"},
        {BattleCommandSegment::MainMenuTransition, "MainMenuTransition"},
        {BattleCommandSegment::DirectCommandAccept, "DirectCommandAccept"},
        {BattleCommandSegment::AwaitNextInputReady, "AwaitNextInputReady"},
        {BattleCommandSegment::AwaitTurnReady, "AwaitTurnReady"},
        {BattleCommandSegment::Complete, "Complete"},
    }};

[[nodiscard]] constexpr std::int64_t BattleCommandSegmentValue(
    BattleCommandSegment segment) noexcept
{
    return static_cast<std::int64_t>(segment);
}

[[nodiscard]] consteval bool BattleCommandSegmentDefinitionsAreComplete()
{
    constexpr auto count = static_cast<std::size_t>(
        BattleCommandSegmentValue(BattleCommandSegment::Complete) + 1);
    if (kBattleCommandSegments.size() != count)
        return false;
    std::array<bool, count> seen{};
    for (const auto& definition : kBattleCommandSegments)
    {
        const auto value = BattleCommandSegmentValue(definition.segment);
        if (value < 0 || static_cast<std::size_t>(value) >= count ||
            definition.name.empty() || seen[static_cast<std::size_t>(value)])
        {
            return false;
        }
        seen[static_cast<std::size_t>(value)] = true;
    }
    for (const bool present : seen)
        if (!present) return false;
    return true;
}

static_assert(BattleCommandSegmentDefinitionsAreComplete());

struct SourceCapabilityPackCatalog
{
    std::vector<TypeSchemaDefinition> schemas;
    std::vector<ActionDescriptor> actions;
    std::vector<ReducerDescriptor> reducers;
    std::vector<CapabilityPackManifest> manifests;
};

[[nodiscard]] RuntimeCompatibility SupportedSoaUsaCompatibility();

[[nodiscard]] CapabilityPackIdentity FieldPackIdentity();
[[nodiscard]] CapabilityPackIdentity BattlePackIdentity();
[[nodiscard]] CapabilityPackIdentity BattleCommandPackIdentity();
[[nodiscard]] CapabilityPackIdentity BattleCompletionPackIdentity();
[[nodiscard]] CapabilityPackIdentity BattleResultsPackIdentity();
[[nodiscard]] CapabilityPackIdentity NavigationPackIdentity();

[[nodiscard]] SchemaIdentity BattleContextSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCaptureContextRequestSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleDerivedFreshnessSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleDerivedQueryRequestSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleDerivedSnapshotSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleTurnExecutionSpecSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCommandStateSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCommandPreparationSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCommandTransitionSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCommandSegmentSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCommandReceiptSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionSnapshotSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionManifestSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionInteractionStateSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionInteractionTransitionSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionInteractionSegmentSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleCompletionInteractionReceiptSchemaIdentity();
[[nodiscard]] SchemaIdentity FieldTransitionContextSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleResultsHandlerInputSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleResultsStateSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleResultsTransitionSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleResultsSegmentSchemaIdentity();
[[nodiscard]] SchemaIdentity BattleResultsReceiptSchemaIdentity();

[[nodiscard]] ExactDependencyIdentity BattleCaptureContextActionIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionCaptureSnapshotActionIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionBuildManifestReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionSemanticEqualReducerIdentity();
[[nodiscard]] ExactDependencyIdentity FieldTransitionBuildContextReducerIdentity();
[[nodiscard]] ExactDependencyIdentity FieldPadStatusToInputFrameReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionInteractionInitializeReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionInteractionAdvanceReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionInteractionCompleteSegmentReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleCompletionInteractionFinalizeReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleResultsInitializeReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleResultsAdvanceReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleResultsCompleteSegmentReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleResultsFinalizeReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleResultsAttachInvariantsReducerIdentity();
[[nodiscard]] ExactDependencyIdentity BattleDerivedTurnEntryActionIdentity();
[[nodiscard]] ExactDependencyIdentity BattleDerivedTurnOrderActionIdentity();
[[nodiscard]] ExactDependencyIdentity BattleDerivedRewardsActionIdentity();
[[nodiscard]] ExactDependencyIdentity BattleDerivedReducerIdentity(
    std::string_view canonical_id);
[[nodiscard]] ExactDependencyIdentity NavigationCaptureContextActionIdentity();

[[nodiscard]] SourceCapabilityPackCatalog BuildSourceCapabilityPackCatalog();

// The registries must refer to each other in the normal order:
// TypeSchemaRegistry -> ActionRegistry -> CapabilityPackRegistry. Definitions
// are registered as schema/action batches before one atomic manifest batch.
[[nodiscard]] RegistryResult RegisterSourceCapabilityPacks(
    TypeSchemaRegistry& schemas,
    ActionRegistry& actions,
    CapabilityPackRegistry& packs);

} // namespace savor::runtime::program::capabilities
