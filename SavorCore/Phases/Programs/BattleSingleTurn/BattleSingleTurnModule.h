#pragma once

#include "Core/Input/GCInputFrame.h"
#include "Core/Input/SoaBattle/ActionTypes.h"
#include "Core/Memory/Soa/Battle/BattleContext.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"
#include "Runner/Runtime/Predicates/PredicateBundle.h"
#include "Runner/Runtime/ProgramRuntime/Composition/InteractionComposition.h"

#include <array>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace savor::runtime::battlesingleturn {

inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view ModuleCanonicalId = "soa.battle.single_turn";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.battle_single_turn";
inline constexpr std::string_view Entrypoint = "execute";
inline constexpr std::uint32_t BeforeRandSeedSetPc = 0x80101E48u;
inline constexpr std::uint32_t AfterRandSeedSetPc = 0x8000A1DCu;
inline constexpr std::uint32_t TurnInputsPc = 0x80071740u;
inline constexpr std::uint32_t TurnIsReadyPc = 0x800715ECu;
inline constexpr std::uint32_t StartTurnPc = 0x800715DCu;
inline constexpr std::uint32_t VictoryPc = 0x800706D8u;
inline constexpr std::uint32_t DefeatPc = 0x8007066Cu;
inline constexpr std::uint32_t RngSeedAddress = 0x803469A8u;
inline constexpr std::uint32_t FakeAttackTargetNeutralFrames = 7;
inline constexpr std::uint32_t MaximumFakeAttackMemoryPolls = 120;

enum class BattleSingleTurnOutcomeV1 : std::int64_t
{
    ReachedNextTurn = 0,
    Victory = 1,
    Defeat = 2,
    PredicateRejected = 3,
};

struct BattleSingleTurnOutcomeDefinitionV1
{
    BattleSingleTurnOutcomeV1 outcome;
    std::string_view name;
};

inline constexpr std::array<BattleSingleTurnOutcomeDefinitionV1, 4>
    BattleSingleTurnOutcomeDefinitionsV1{{
        {BattleSingleTurnOutcomeV1::ReachedNextTurn, "ReachedNextTurn"},
        {BattleSingleTurnOutcomeV1::Victory, "Victory"},
        {BattleSingleTurnOutcomeV1::Defeat, "Defeat"},
        {BattleSingleTurnOutcomeV1::PredicateRejected, "PredicateRejected"},
    }};

[[nodiscard]] constexpr const BattleSingleTurnOutcomeDefinitionV1*
FindBattleSingleTurnOutcomeDefinitionV1(std::int64_t value) noexcept
{
    for (const auto& definition : BattleSingleTurnOutcomeDefinitionsV1)
    {
        if (static_cast<std::int64_t>(definition.outcome) == value)
            return &definition;
    }
    return nullptr;
}

[[nodiscard]] consteval bool
BattleSingleTurnOutcomeDefinitionsAreCompleteV1()
{
    if (BattleSingleTurnOutcomeDefinitionsV1.size() != 4) return false;
    for (std::size_t index = 0;
         index < BattleSingleTurnOutcomeDefinitionsV1.size(); ++index)
    {
        const auto& definition =
            BattleSingleTurnOutcomeDefinitionsV1[index];
        if (definition.name.empty()) return false;
        for (std::size_t other = 0; other < index; ++other)
        {
            if (BattleSingleTurnOutcomeDefinitionsV1[other].outcome ==
                    definition.outcome ||
                BattleSingleTurnOutcomeDefinitionsV1[other].name ==
                    definition.name)
            {
                return false;
            }
        }
    }
    return FindBattleSingleTurnOutcomeDefinitionV1(0) != nullptr &&
        FindBattleSingleTurnOutcomeDefinitionV1(1) != nullptr &&
        FindBattleSingleTurnOutcomeDefinitionV1(2) != nullptr &&
        FindBattleSingleTurnOutcomeDefinitionV1(3) != nullptr;
}

static_assert(BattleSingleTurnOutcomeDefinitionsAreCompleteV1());

struct BattleSingleTurnRequestV1
{
    std::uint32_t turn_index = 1;
    std::uint32_t cumulative_fake_attacks_before = 0;
    soa::battle::actions::BattleTurnExecutionSpec plan;
    std::optional<savor::GCInputFrame> confirmed_seed_frame;
    std::string output_savestate_path;
};

struct BattleSingleTurnResultV1
{
    BattleSingleTurnOutcomeV1 outcome = BattleSingleTurnOutcomeV1::Defeat;
    std::uint32_t ending_rng = 0;
    std::uint64_t vi_start = 0;
    std::uint64_t vi_end = 0;
    std::uint32_t pred_passed = 0;
    std::uint32_t pred_total = 0;
    std::uint32_t cumulative_fake_attacks = 0;
    bool has_battle_context = false;
    soa::battle::ctx::BattleContext battle_context;
    std::int64_t predicate_bundle_revision_id = 0;
    std::string predicate_bundle_sha256;
    std::string predicate_binding_sha256;
    std::vector<program::ProgramEmission> predicate_evidence;
    std::vector<program::ProgramArtifact> artifacts;
};

[[nodiscard]] std::vector<std::uint8_t> EncodeBattleSingleTurnCommonInputV1(
    bool first_turn,
    const predicates::PredicateBundleExecutionPackageV1& predicate_package);

[[nodiscard]] std::vector<std::uint8_t> EncodeBattleSingleTurnExecutionInputV1(
    const BattleSingleTurnRequestV1& request);
[[nodiscard]] bool DecodeBattleSingleTurnExecutionInputV1(
    std::span<const std::uint8_t> input,
    BattleSingleTurnRequestV1& request,
    std::string* diagnostic = nullptr);

class IBattleSingleTurnFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual const predicates::PredicateBundleExecutionPackageV1&
    predicate_package() const noexcept = 0;
    [[nodiscard]] virtual bool first_turn() const noexcept = 0;
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded_result,
        BattleSingleTurnResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

// Preparation is coordination-owned and cached by the structural bundle and
// entry-path identity. The returned package is self-contained for workers.
[[nodiscard]] std::shared_ptr<const IBattleSingleTurnFullPhaseDefinitionV1>
PrepareBattleSingleTurnFullPhaseV1(
    bool first_turn,
    predicates::PredicateBundleExecutionPackageV1 predicate_package,
    std::string* diagnostic = nullptr);

// Static per-kind codec/handler used by worker admission. It never accesses a
// database and accepts only a package produced by the preparer above.
[[nodiscard]] std::shared_ptr<const fullphase::IFullPhaseProgramDefinition>
BattleSingleTurnKindHandlerV1();

// Shared adaptive Battle command entry contract. Battle recording reuses this
// exact interaction rather than carrying a second command implementation.
[[nodiscard]] program::composition::InteractionDefinition
BattleCommandInteractionV3();

} // namespace savor::runtime::battlesingleturn
