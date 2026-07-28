#pragma once

#include "SourceCapabilityPacks.h"
#include "Core/Input/InputPlan.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Core/Memory/Soa/Navigation/NavigationContext.h"

#include <optional>
#include <span>
#include <string>

namespace savor::runtime::program::capabilities {

[[nodiscard]] ProgramValueGraph EncodeBattleContextValue(
    const soa::battle::ctx::BattleContext& context);

[[nodiscard]] ProgramValueGraph EncodeNavigationContextValue(
    const soa::navigation::ctx::NavigationContext& context);

[[nodiscard]] ProgramValueGraph EncodeBattleTurnExecutionSpecValue(
    const soa::battle::actions::BattleTurnExecutionSpec& specification);

[[nodiscard]] bool DecodeTurnInputMaterializationValue(
    const ProgramValueGraph& graph,
    bool& success,
    savor::ControllerInputSequence& sequence,
    std::string* domain_diagnostic = nullptr);

// Returns nullopt only for an unknown reducer or malformed typed input.
// Domain-negative materialization remains a valid typed result whose success
// field is false.
[[nodiscard]] std::optional<ProgramValueGraph> InvokeSourceReducer(
    const ExactDependencyIdentity& identity,
    std::span<const ProgramValueGraph> inputs,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::program::capabilities
