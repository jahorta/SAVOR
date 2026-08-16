#pragma once

#include "Phases/Programs/BattleCompletion/BattleCompletionContracts.h"
#include "Phases/Programs/BattleSingleTurn/BattleSingleTurnModule.h"
#include "Runner/Runtime/FullPhase/FullPhaseProgram.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::battlerecord {

inline constexpr std::int32_t ProgramVersion = 1;
inline constexpr std::string_view Entrypoint = "record";
inline constexpr std::string_view ModuleCanonicalId = "soa.battle.record";
inline constexpr std::string_view FullPhaseCanonicalId =
    "savor.full_phase.battle_record";
inline constexpr std::string_view BaselineLineage =
    "soa.battle.record/movie-paired-battle-entry/v1";

struct BattleReplayTurnV1
{
    std::uint32_t turn_index = 0;
    soa::battle::actions::BattleTurnExecutionSpec plan;
    battlesingleturn::BattleSingleTurnOutcomeV1 expected_outcome =
        battlesingleturn::BattleSingleTurnOutcomeV1::ReachedNextTurn;
    std::uint32_t expected_ending_rng = 0;
};

struct BattleReplayPlanV1
{
    battlecompletion::BattleCompletionLineageV1 selected_lineage;
    std::uint64_t battle_completion_id = 0;
    GCInputFrame confirmed_seed_frame;
    std::vector<BattleReplayTurnV1> turns;
    battlecompletion::BattleCompletionManifestV1 expected_completion;
    std::string canonical_sha256;
};

// Coordination binds the source independently from the replay instructions.
// Record and Replay therefore execute byte-identical plans even when Replay
// starts from the BattleSet's movie-inactive entry checkpoint.
struct BattleReplaySourceBindingV1
{
    std::uint64_t source_savestate_id = 0;
    std::optional<std::uint64_t> source_dtm_artifact_id;
    std::optional<std::uint64_t> source_itinerary_artifact_id;
    std::string source_savestate_sha256;
    std::optional<std::string> source_dtm_sha256;
    std::optional<std::string> source_itinerary_sha256;
    std::string canonical_sha256;
};

struct BattleRecordRequestV1
{
    std::string output_dtm_path;
    std::string output_preseed_savestate_path;
};

struct BattleRecordResultV1
{
    battlecompletion::BattleRecordOutcomeV1 outcome =
        battlecompletion::BattleRecordOutcomeV1::ReplayMismatch;
    std::uint32_t mismatch_turn = 0;
    std::uint32_t expected_rng = 0;
    std::uint32_t observed_rng = 0;
    std::optional<battlecompletion::BattleCompletionManifestV1>
        observed_completion;
    std::optional<battlecompletion::FieldTransitionContextV1> transition;
    battlecompletion::BattleTimingAdjustmentAnchorV1 timing_anchor;
    std::uint64_t checkpoint_input_count = 0;
    std::uint64_t final_input_count = 0;
    std::vector<program::ProgramArtifact> artifacts;
};

[[nodiscard]] std::string ComputeBattleReplayPlanHashV1(
    const BattleReplayPlanV1& plan);
[[nodiscard]] bool ValidateBattleReplayPlanV1(
    const BattleReplayPlanV1& plan,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeBattleReplayPlanV1(
    const BattleReplayPlanV1& plan,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeBattleReplayPlanV1(
    std::span<const std::uint8_t> bytes,
    BattleReplayPlanV1& plan,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::string ComputeBattleReplaySourceBindingHashV1(
    const BattleReplaySourceBindingV1& binding);
[[nodiscard]] bool ValidateBattleReplaySourceBindingV1(
    const BattleReplaySourceBindingV1& binding,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeBattleReplaySourceBindingV1(
    const BattleReplaySourceBindingV1& binding,
    std::string* diagnostic = nullptr);
[[nodiscard]] bool DecodeBattleReplaySourceBindingV1(
    std::span<const std::uint8_t> bytes,
    BattleReplaySourceBindingV1& binding,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::vector<std::uint8_t> EncodeBattleRecordExecutionInputV1(
    const BattleRecordRequestV1& request);
[[nodiscard]] bool DecodeBattleRecordExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleRecordRequestV1& request,
    std::string* diagnostic = nullptr);

class IBattleRecordFullPhaseDefinitionV1
    : public fullphase::IFullPhaseProgramDefinition
{
public:
    [[nodiscard]] virtual const BattleReplayPlanV1& replay_plan() const noexcept = 0;
    [[nodiscard]] virtual bool DecodeProgramResult(
        std::span<const program::Byte> encoded,
        BattleRecordResultV1& result,
        std::string* diagnostic = nullptr) const = 0;
};

[[nodiscard]] std::shared_ptr<const IBattleRecordFullPhaseDefinitionV1>
PrepareBattleRecordFullPhaseV1(
    BattleReplayPlanV1 replay_plan,
    std::string* diagnostic = nullptr);
[[nodiscard]] std::shared_ptr<const fullphase::IFullPhaseProgramDefinition>
BattleRecordKindHandlerV1();

} // namespace savor::runtime::battlerecord
