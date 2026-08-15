#pragma once

#include "BattleRecordModule.h"

namespace savor::runtime::battlereplay {

inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view Entrypoint = "replay";
inline constexpr std::string_view ModuleCanonicalId = "soa.battle.replay";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.battle_replay";
inline constexpr std::string_view BaselineLineage =
    "soa.battle.replay/exact-battle-set-entry/v1";

enum class BattleReplayOutcomeV1 : std::uint8_t
{
    Matched = 0,
    ReplayMismatch = 1,
};

struct BattleReplayRequestV1 {};

struct BattleReplayResultV1
{
    BattleReplayOutcomeV1 outcome = BattleReplayOutcomeV1::ReplayMismatch;
    std::uint32_t mismatch_turn = 0;
    std::uint32_t expected_rng = 0;
    std::uint32_t observed_rng = 0;
    std::optional<battlecompletion::BattleCompletionManifestV1>
        observed_completion;
    std::optional<battlecompletion::FieldTransitionContextV1> transition;
    std::vector<program::ProgramArtifact> artifacts;
};

[[nodiscard]] std::vector<std::uint8_t> EncodeBattleReplayExecutionInputV1(
    const BattleReplayRequestV1& request = {});
[[nodiscard]] bool DecodeBattleReplayExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleReplayRequestV1& request,
    std::string* diagnostic = nullptr);

class IBattleReplayFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual const battlerecord::BattleReplayPlanV1&
        replay_plan() const noexcept = 0;
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        BattleReplayResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::shared_ptr<const IBattleReplayFullPhaseDefinitionV1>
PrepareBattleReplayFullPhaseV1(
    battlerecord::BattleReplayPlanV1 replay_plan,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::shared_ptr<const fullphase::IFullPhaseProgramDefinition>
BattleReplayKindHandlerV1();

} // namespace savor::runtime::battlereplay
