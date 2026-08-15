#pragma once

#include "SourceCapabilityPacks.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"
#include "Runner/Runtime/DerivedState/BattleCoreDerivedState.h"
#include "Phases/Programs/BattleCompletion/BattleCompletionContracts.h"

#include <optional>
#include <span>
#include <string>

namespace savor::runtime::program::capabilities {

[[nodiscard]] ProgramValueGraph EncodeBattleContextValue(
    const soa::battle::ctx::BattleContext& context);

[[nodiscard]] bool DecodeBattleContextValue(
    const ProgramValueGraph& graph,
    soa::battle::ctx::BattleContext& context);

[[nodiscard]] ProgramValueGraph EncodeNavigationContextValue(
    const soa::navigation::ctx::NavigationContext& context);

[[nodiscard]] ProgramValueGraph EncodeBattleTurnExecutionSpecValue(
    const soa::battle::actions::BattleTurnExecutionSpec& specification);

[[nodiscard]] ProgramValueGraph EncodeBattleCompletionSnapshotValue(
    const battlecompletion::BattleCompletionSnapshotV1& snapshot);

[[nodiscard]] bool DecodeBattleDerivedQueryValue(
    const ProgramValueGraph& graph,
    derived::DerivedStateQueryV1& query);

[[nodiscard]] ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleTurnEntrySnapshotV1& snapshot);
[[nodiscard]] ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleTurnOrderSnapshotV1& snapshot);
[[nodiscard]] ProgramValueGraph EncodeBattleDerivedSnapshotValue(
    const derived::BattleRewardsSnapshotV1& snapshot);

// Returns nullopt only for an unknown reducer or malformed typed input.
[[nodiscard]] std::optional<ProgramValueGraph> InvokeSourceReducer(
    const ExactDependencyIdentity& identity,
    std::span<const ProgramValueGraph> inputs,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::program::capabilities
