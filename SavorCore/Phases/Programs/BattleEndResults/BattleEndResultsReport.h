#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../../../Runner/Breakpoints/BpRegistry.h"

namespace phase::battle::endresults {

// Encoded little-endian; the first four artifact bytes are exactly "BERB".
inline constexpr std::uint32_t ReportMagic = 0x42524542u;
inline constexpr std::uint16_t ReportVersion = 1;
inline constexpr std::size_t CharacterCount = 6;
inline constexpr std::size_t ElementCount = 6;

enum class AccelerationPolicy : std::uint32_t {
    RequiredOnly = 0,
    FullAdaptive = 1,
};

enum class Outcome : std::uint32_t {
    Completed = 0,
    Failed = 1,
};

enum class FailureCode : std::uint32_t {
    None = 0,
    InvalidSource = 1,
    InvalidRequest = 2,
    MemoryReadFailed = 3,
    InvalidResultPointer = 4,
    InvalidResultState = 5,
    InvalidRowCount = 6,
    ReservedLegacyTimeout = 7,
    UnexpectedBreakpoint = 8,
    QualificationMismatch = 9,
    GuestNeutralUnacknowledged = 10,
    StaleOccurrence = 11,
    OccurrenceLimit = 12,
    NoProgress = 13,
    VictoryInvariantMismatch = 14,
    RewardInvariantMismatch = 15,
    LifecycleInvariantMismatch = 16,
    CompletionInvariantMismatch = 17,
    HostFailure = 18,
    RuntimeFailure = 19,
    Cancelled = 20,
};

enum class ActionKind : std::uint8_t {
    Intro = 0,
    Gold = 1,
    NormalExp = 2,
    StatWave = 3,
    MagicEntry = 4,
    MagicExp = 5,
    LearnedMagicWave = 6,
    ItemPopup = 7,
    MandatoryConfirm = 8,
    Fade = 9,
};

enum MismatchFlag : std::uint32_t {
    MismatchNone = 0,
    MismatchStatWaveCount = 1u << 0,
    MismatchLearnedWaveCount = 1u << 1,
    MismatchItemPopup = 1u << 2,
    MismatchLearnedQueue = 1u << 3,
    MismatchPresentationSkipped = 1u << 4,
    MismatchRngChanged = 1u << 5,
};

enum InvariantFlag : std::uint32_t {
    InvariantNone = 0,
    InvariantSourcePc = 1u << 0,
    InvariantVictoryState = 1u << 1,
    InvariantRewardPhase = 1u << 2,
    InvariantLifecycleState = 1u << 3,
    InvariantCompletionState = 1u << 4,
    InvariantCompletionPublished = 1u << 5,
    InvariantResultPointerCleared = 1u << 6,
    InvariantFieldMode = 1u << 7,
    InvariantRngPreserved = 1u << 8,
};

struct ElementProgress {
    std::uint32_t xp_before{0};
    std::uint32_t xp_after{0};
    std::uint8_t rank_before{0};
    std::uint8_t rank_after{0};
};

struct CharacterProgress {
    std::uint8_t character_id{0};
    std::uint8_t level_before{0};
    std::uint8_t level_after{0};
    std::uint32_t experience_before{0};
    std::uint32_t experience_after{0};
    std::array<ElementProgress, ElementCount> elements{};
    std::vector<std::uint8_t> learned_magic_ids;
};

struct ExpectedView {
    std::array<CharacterProgress, CharacterCount> characters{};
    std::uint32_t expected_stat_waves{0};
    std::uint32_t expected_learned_waves{0};
    bool expected_item_popup{false};
};

struct ActionTrace {
    ActionKind kind{ActionKind::Intro};
    std::uint8_t state{0};
    std::uint8_t substate{0};
    std::uint64_t occurrence_token{0};
    BPKey ready_key{0};
    BPKey accepted_key{0};
    std::uint32_t ready_pc{0};
    std::uint32_t accepted_pc{0};
    std::uint64_t neutral_epoch{0};
    std::uint64_t input_epoch{0};
    std::uint64_t release_epoch{0};
    std::uint32_t input_poll_count{0};
    bool input_acknowledged{false};
    bool release_observed{false};
    bool progress_observed{false};
};

struct Report {
    AccelerationPolicy policy{AccelerationPolicy::FullAdaptive};
    Outcome outcome{Outcome::Failed};
    FailureCode failure{FailureCode::None};
    ExpectedView expected;
    std::vector<ActionTrace> actions;
    std::uint32_t observed_stat_waves{0};
    std::uint32_t observed_learned_waves{0};
    bool observed_item_popup{false};
    std::uint32_t mismatch_flags{MismatchNone};
    std::uint32_t invariant_flags{InvariantNone};
    std::uint64_t start_vi{0};
    std::uint64_t end_vi{0};
    std::string diagnostic;
};

const char* FailureCodeName(FailureCode code) noexcept;
const char* ActionKindName(ActionKind kind) noexcept;
bool IsValidPolicy(AccelerationPolicy policy) noexcept;

bool EncodeReport(const Report& report, std::string& out);
bool DecodeReport(const std::string& bytes, Report& out);

} // namespace phase::battle::endresults
