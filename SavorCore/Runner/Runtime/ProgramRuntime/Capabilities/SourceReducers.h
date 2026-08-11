#pragma once

#include "SourceCapabilityPacks.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"

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

// Returns nullopt only for an unknown reducer or malformed typed input.
[[nodiscard]] std::optional<ProgramValueGraph> InvokeSourceReducer(
    const ExactDependencyIdentity& identity,
    std::span<const ProgramValueGraph> inputs,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::program::capabilities
