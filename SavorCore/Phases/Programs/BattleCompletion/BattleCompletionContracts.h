#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace savor::runtime::battlecompletion {

inline constexpr std::uint16_t ManifestVersion = 1;
inline constexpr std::uint16_t TimingAnchorVersion = 1;
inline constexpr std::string_view ManifestExtension = ".bcmb";
inline constexpr std::string_view ManifestArtifactKind = "BATTLE_COMPLETION";
inline constexpr std::size_t CharacterCount = 4;
inline constexpr std::size_t CharacterRecordSize = 0x5c;
inline constexpr std::size_t RewardItemCount = 3;

struct BattleStopProvenanceV1
{
    std::uint32_t pc = 0;
    std::uint64_t vi_count = 0;
    std::uint64_t workset_epoch = 0;

    auto operator<=>(const BattleStopProvenanceV1&) const = default;
};

struct BattleRewardItemV1
{
    std::int16_t item_id = -1;
    std::uint8_t quantity = 0;
    std::uint8_t opaque = 0;

    auto operator<=>(const BattleRewardItemV1&) const = default;
};

struct BattleResultsPresentationV1
{
    std::uint32_t gold_pages = 0;
    std::uint32_t normal_exp_pages = 0;
    std::uint32_t level_panels = 0;
    std::uint32_t stat_waves = 0;
    std::uint32_t magic_exp_pages = 0;
    std::uint32_t magic_rank_events = 0;
    std::uint32_t learned_magic_waves = 0;
    std::uint32_t item_popups = 0;

    auto operator<=>(const BattleResultsPresentationV1&) const = default;
};

struct BattleCompletionLineageV1
{
    std::uint64_t battle_set_id = 0;
    std::uint64_t wave_id = 0;
    std::uint64_t turn_job_id = 0;
    std::uint64_t execution_job_id = 0;

    auto operator<=>(const BattleCompletionLineageV1&) const = default;
};

struct BattleCompletionSnapshotV1
{
    std::array<std::array<std::uint8_t, CharacterRecordSize>, CharacterCount>
        character_records{};
    std::uint32_t normal_experience_reward = 0;
    std::uint32_t magic_experience_reward = 0;
    std::uint32_t gold_reward = 0;
    std::array<BattleRewardItemV1, RewardItemCount> reward_items{};
    std::uint32_t rng_seed = 0;
    std::uint32_t battle_input_state = 0;
    std::uint32_t reward_phase = 0;
};

struct FieldTransitionContextV1
{
    std::uint32_t area = 0;
    std::uint8_t raw_suffix = 0;
    std::uint8_t effective_suffix = 0;
    bool used_area_99_suffix = false;
    std::string sct_filename;
    std::uint32_t rng_seed = 0;
    BattleStopProvenanceV1 provenance;

    auto operator<=>(const FieldTransitionContextV1&) const = default;
};

struct BattleCompletionManifestV1
{
    BattleCompletionLineageV1 lineage;
    std::array<std::array<std::uint8_t, CharacterRecordSize>, CharacterCount>
        pre_character_records{};
    std::array<std::array<std::uint8_t, CharacterRecordSize>, CharacterCount>
        post_character_records{};
    std::uint32_t normal_experience_reward = 0;
    std::uint32_t magic_experience_reward = 0;
    std::uint32_t gold_reward = 0;
    std::array<BattleRewardItemV1, RewardItemCount> reward_items{};
    BattleResultsPresentationV1 presentation;
    std::uint32_t entry_rng_seed = 0;
    std::uint32_t completion_rng_seed = 0;
    BattleStopProvenanceV1 entry;
    BattleStopProvenanceV1 reward_entry;
    BattleStopProvenanceV1 reward_commit;
    FieldTransitionContextV1 transition;
};

struct BattleTimingAdjustmentAnchorV1
{
    std::uint32_t turn_index = 0;
    std::uint8_t actor_slot = 0;
    std::uint32_t command_ordinal = 0;
    std::string semantic_role;
    std::uint64_t dtm_input_index = 0;

    auto operator<=>(const BattleTimingAdjustmentAnchorV1&) const = default;
};

enum class BattleRecordOutcomeV1 : std::uint8_t
{
    Recorded = 0,
    ReplayMismatch = 1,
};

[[nodiscard]] bool BuildBattleCompletionManifestV1(
    BattleCompletionLineageV1 lineage,
    const BattleCompletionSnapshotV1& before,
    const BattleCompletionSnapshotV1& after,
    BattleStopProvenanceV1 entry,
    BattleStopProvenanceV1 reward_entry,
    BattleStopProvenanceV1 reward_commit,
    FieldTransitionContextV1 transition,
    BattleCompletionManifestV1& output,
    std::string* diagnostic = nullptr);

[[nodiscard]] bool EncodeBattleCompletionSnapshotV1(
    const BattleCompletionSnapshotV1& value,
    std::vector<std::uint8_t>& output);
[[nodiscard]] bool DecodeBattleCompletionSnapshotV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionSnapshotV1& output);
[[nodiscard]] bool EncodeBattleCompletionManifestV1(
    const BattleCompletionManifestV1& value,
    std::vector<std::uint8_t>& output);
[[nodiscard]] bool DecodeBattleCompletionManifestV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionManifestV1& output);
[[nodiscard]] bool EncodeFieldTransitionContextV1(
    const FieldTransitionContextV1& value,
    std::vector<std::uint8_t>& output);
[[nodiscard]] bool DecodeFieldTransitionContextV1(
    std::span<const std::uint8_t> bytes,
    FieldTransitionContextV1& output);
[[nodiscard]] bool EncodeBattleTimingAdjustmentAnchorV1(
    const BattleTimingAdjustmentAnchorV1& value,
    std::vector<std::uint8_t>& output);
[[nodiscard]] bool DecodeBattleTimingAdjustmentAnchorV1(
    std::span<const std::uint8_t> bytes,
    BattleTimingAdjustmentAnchorV1& output);

[[nodiscard]] std::string SemanticBattleCompletionManifestHashV1(
    const BattleCompletionManifestV1& value);
[[nodiscard]] bool SemanticallyEqualBattleCompletionManifestV1(
    const BattleCompletionManifestV1& lhs,
    const BattleCompletionManifestV1& rhs) noexcept;

[[nodiscard]] bool ReconstructFieldSctFilenameV1(
    std::uint32_t area,
    std::uint8_t raw_suffix,
    std::optional<std::uint8_t> area_99_suffix_index,
    std::uint8_t& effective_suffix,
    std::string& filename,
    std::string* diagnostic = nullptr);

} // namespace savor::runtime::battlecompletion
