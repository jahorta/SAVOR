#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "../BattleEndResults/BattleEndResultsReport.h"

namespace phase::battle::completion {

// Encoded little-endian; the first four bytes are exactly "BCMB".
inline constexpr std::uint32_t ManifestMagic = 0x424D4342u;
inline constexpr std::uint16_t ManifestVersion = 1;
inline constexpr std::size_t CharacterCount =
    phase::battle::endresults::CharacterCount;
inline constexpr std::size_t CharacterRecordSize = 0x5Cu;
inline constexpr std::size_t RewardItemCount = 3;

enum ManifestInvariant : std::uint32_t {
    ManifestInvariantNone = 0,
    ManifestInvariantPreCaptured = 1u << 0,
    ManifestInvariantPostCaptured = 1u << 1,
    ManifestInvariantLevelNondecreasing = 1u << 2,
    ManifestInvariantExperienceNondecreasing = 1u << 3,
    ManifestInvariantElementXpNondecreasing = 1u << 4,
    ManifestInvariantElementRankNondecreasing = 1u << 5,
    ManifestInvariantRewardItemsCaptured = 1u << 6,
    ManifestInvariantExpectedViewDerived = 1u << 7,
};

inline constexpr std::uint32_t RequiredManifestInvariants =
    ManifestInvariantPreCaptured
    | ManifestInvariantPostCaptured
    | ManifestInvariantLevelNondecreasing
    | ManifestInvariantExperienceNondecreasing
    | ManifestInvariantElementXpNondecreasing
    | ManifestInvariantElementRankNondecreasing
    | ManifestInvariantRewardItemsCaptured
    | ManifestInvariantExpectedViewDerived;

struct RewardItem {
    std::int16_t item_id{-1};
    std::uint8_t quantity{0};
    std::uint8_t opaque{0};
};

struct ExpectedPresentation {
    std::uint32_t gold_pages{0};
    std::uint32_t normal_exp_pages{0};
    std::uint32_t level_panels{0};
    std::uint32_t stat_waves{0};
    std::uint32_t magic_exp_pages{0};
    std::uint32_t magic_rank_events{0};
    std::uint32_t learned_magic_waves{0};
    std::uint32_t item_popups{0};
};

struct Manifest {
    std::array<std::array<std::uint8_t, CharacterRecordSize>, CharacterCount>
        pre_character_records{};
    std::array<std::array<std::uint8_t, CharacterRecordSize>, CharacterCount>
        post_character_records{};
    phase::battle::endresults::ExpectedView expected;
    std::uint32_t normal_experience_reward{0};
    std::uint32_t magic_experience_reward{0};
    std::uint32_t gold_reward{0};
    std::array<RewardItem, RewardItemCount> reward_items{};
    ExpectedPresentation presentation;
    std::uint32_t invariant_flags{ManifestInvariantNone};
    std::uint64_t start_vi{0};
    std::uint64_t end_vi{0};
};

// Builds the semantic view solely from the raw persistent records. Known
// magic is rank-derived: element E contributes IDs E*6 through E*6+rank-1.
bool DeriveExpectedView(Manifest& manifest, std::string* error = nullptr);
bool EncodeManifest(const Manifest& manifest, std::string& out);
bool DecodeManifest(const std::string& bytes, Manifest& out);

} // namespace phase::battle::completion
