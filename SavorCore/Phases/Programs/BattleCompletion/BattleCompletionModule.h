#pragma once

#include "BattleCompletionContracts.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::battlecompletion {

inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view Entrypoint = "complete";
inline constexpr std::string_view ModuleCanonicalId = "soa.battle.completion";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.battle_completion";
inline constexpr std::string_view BaselineLineage =
    "soa.battle.completion/victory-entry/v1";

struct BattleCompletionRequestV1
{
    BattleCompletionLineageV1 lineage;
    std::string output_savestate_path;
};

struct BattleCompletionResultV1
{
    BattleCompletionManifestV1 manifest;
    FieldTransitionContextV1 transition;
    std::vector<program::ProgramArtifact> artifacts;
};

[[nodiscard]] std::vector<std::uint8_t> EncodeBattleCompletionExecutionInputV1(
    const BattleCompletionRequestV1& request);
[[nodiscard]] bool DecodeBattleCompletionExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionRequestV1& request,
    std::string* diagnostic = nullptr);

class IBattleCompletionFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        BattleCompletionResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::shared_ptr<const IBattleCompletionFullPhaseDefinitionV1>
BattleCompletionFullPhaseDefinitionV1();

} // namespace savor::runtime::battlecompletion
