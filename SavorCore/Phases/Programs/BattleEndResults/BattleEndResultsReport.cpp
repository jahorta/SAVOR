#include "BattleEndResultsReport.h"

#include <limits>
#include <string_view>

namespace phase::battle::endresults {
namespace {

constexpr std::size_t kMaxActionCount = 256;
constexpr std::size_t kMaxLearnedIdsPerCharacter = 36;
constexpr std::size_t kMaxDiagnosticBytes = 16 * 1024;

void PutU8(std::string& out, std::uint8_t value)
{
    out.push_back(static_cast<char>(value));
}

void PutU16(std::string& out, std::uint16_t value)
{
    PutU8(out, static_cast<std::uint8_t>(value));
    PutU8(out, static_cast<std::uint8_t>(value >> 8));
}

void PutU32(std::string& out, std::uint32_t value)
{
    PutU16(out, static_cast<std::uint16_t>(value));
    PutU16(out, static_cast<std::uint16_t>(value >> 16));
}

void PutU64(std::string& out, std::uint64_t value)
{
    PutU32(out, static_cast<std::uint32_t>(value));
    PutU32(out, static_cast<std::uint32_t>(value >> 32));
}

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}

    bool U8(std::uint8_t& out)
    {
        if (offset_ >= bytes_.size()) return false;
        out = static_cast<std::uint8_t>(bytes_[offset_++]);
        return true;
    }

    bool U16(std::uint16_t& out)
    {
        std::uint8_t lo = 0;
        std::uint8_t hi = 0;
        if (!U8(lo) || !U8(hi)) return false;
        out = static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
        return true;
    }

    bool U32(std::uint32_t& out)
    {
        std::uint16_t lo = 0;
        std::uint16_t hi = 0;
        if (!U16(lo) || !U16(hi)) return false;
        out = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        return true;
    }

    bool U64(std::uint64_t& out)
    {
        std::uint32_t lo = 0;
        std::uint32_t hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        out = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }

    bool Bytes(std::size_t count, std::string& out)
    {
        if (count > bytes_.size() - offset_) return false;
        out.assign(bytes_.substr(offset_, count));
        offset_ += count;
        return true;
    }

    bool done() const noexcept { return offset_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t offset_{0};
};

bool IsValidActionKind(ActionKind kind) noexcept
{
    return static_cast<std::uint8_t>(kind) <= static_cast<std::uint8_t>(ActionKind::Fade);
}

bool IsValidFailure(FailureCode code) noexcept
{
    return static_cast<std::uint32_t>(code) <= static_cast<std::uint32_t>(FailureCode::Cancelled);
}

} // namespace

const char* FailureCodeName(FailureCode code) noexcept
{
    switch (code) {
    case FailureCode::None: return "none";
    case FailureCode::InvalidSource: return "invalid_source";
    case FailureCode::InvalidRequest: return "invalid_request";
    case FailureCode::MemoryReadFailed: return "memory_read_failed";
    case FailureCode::InvalidResultPointer: return "invalid_result_pointer";
    case FailureCode::InvalidResultState: return "invalid_result_state";
    case FailureCode::InvalidRowCount: return "invalid_row_count";
    case FailureCode::Timeout: return "timeout";
    case FailureCode::UnexpectedBreakpoint: return "unexpected_breakpoint";
    case FailureCode::QualificationMismatch: return "qualification_mismatch";
    case FailureCode::GuestNeutralTimeout: return "guest_neutral_timeout";
    case FailureCode::StaleOccurrence: return "stale_occurrence";
    case FailureCode::OccurrenceLimit: return "occurrence_limit";
    case FailureCode::NoProgress: return "no_progress";
    case FailureCode::VictoryInvariantMismatch: return "victory_invariant_mismatch";
    case FailureCode::RewardInvariantMismatch: return "reward_invariant_mismatch";
    case FailureCode::LifecycleInvariantMismatch: return "lifecycle_invariant_mismatch";
    case FailureCode::CompletionInvariantMismatch: return "completion_invariant_mismatch";
    case FailureCode::HostFailure: return "host_failure";
    case FailureCode::RuntimeFailure: return "runtime_failure";
    case FailureCode::Cancelled: return "cancelled";
    }
    return "invalid";
}

const char* ActionKindName(ActionKind kind) noexcept
{
    switch (kind) {
    case ActionKind::Intro: return "intro";
    case ActionKind::Gold: return "gold";
    case ActionKind::NormalExp: return "normal_exp";
    case ActionKind::StatWave: return "stat_wave";
    case ActionKind::MagicEntry: return "magic_entry";
    case ActionKind::MagicExp: return "magic_exp";
    case ActionKind::LearnedMagicWave: return "learned_magic_wave";
    case ActionKind::ItemPopup: return "item_popup";
    case ActionKind::MandatoryConfirm: return "mandatory_confirm";
    case ActionKind::Fade: return "fade";
    }
    return "invalid";
}

bool IsValidPolicy(AccelerationPolicy policy) noexcept
{
    return policy == AccelerationPolicy::RequiredOnly
        || policy == AccelerationPolicy::FullAdaptive;
}

bool EncodeReport(const Report& report, std::string& out)
{
    if (!IsValidPolicy(report.policy)
        || !IsValidFailure(report.failure)
        || report.actions.size() > kMaxActionCount
        || report.diagnostic.size() > kMaxDiagnosticBytes) {
        return false;
    }
    for (const auto& character : report.expected.characters) {
        if (character.learned_magic_ids.size() > kMaxLearnedIdsPerCharacter) return false;
    }

    out.clear();
    PutU32(out, ReportMagic);
    PutU16(out, ReportVersion);
    PutU16(out, 0);
    PutU32(out, static_cast<std::uint32_t>(report.policy));
    PutU32(out, static_cast<std::uint32_t>(report.outcome));
    PutU32(out, static_cast<std::uint32_t>(report.failure));
    PutU32(out, report.expected.expected_stat_waves);
    PutU32(out, report.expected.expected_learned_waves);
    PutU8(out, report.expected.expected_item_popup ? 1u : 0u);
    PutU8(out, report.observed_item_popup ? 1u : 0u);
    PutU16(out, 0);
    PutU32(out, report.observed_stat_waves);
    PutU32(out, report.observed_learned_waves);
    PutU32(out, report.mismatch_flags);
    PutU32(out, report.invariant_flags);
    PutU64(out, report.start_vi);
    PutU64(out, report.end_vi);

    for (const auto& character : report.expected.characters) {
        PutU8(out, character.character_id);
        PutU8(out, character.level_before);
        PutU8(out, character.level_after);
        PutU8(out, static_cast<std::uint8_t>(character.learned_magic_ids.size()));
        PutU32(out, character.experience_before);
        PutU32(out, character.experience_after);
        for (const auto& element : character.elements) {
            PutU32(out, element.xp_before);
            PutU32(out, element.xp_after);
            PutU8(out, element.rank_before);
            PutU8(out, element.rank_after);
            PutU16(out, 0);
        }
        for (const auto id : character.learned_magic_ids) PutU8(out, id);
    }

    PutU32(out, static_cast<std::uint32_t>(report.actions.size()));
    for (const auto& action : report.actions) {
        if (!IsValidActionKind(action.kind)) return false;
        PutU8(out, static_cast<std::uint8_t>(action.kind));
        PutU8(out, action.state);
        PutU8(out, action.substate);
        std::uint8_t flags = 0;
        if (action.input_acknowledged) flags |= 1u << 0;
        if (action.release_observed) flags |= 1u << 1;
        if (action.progress_observed) flags |= 1u << 2;
        PutU8(out, flags);
        PutU64(out, action.occurrence_token);
        PutU32(out, action.ready_key);
        PutU32(out, action.accepted_key);
        PutU32(out, action.ready_pc);
        PutU32(out, action.accepted_pc);
        PutU64(out, action.neutral_epoch);
        PutU64(out, action.input_epoch);
        PutU64(out, action.release_epoch);
        PutU32(out, action.input_poll_count);
    }

    PutU32(out, static_cast<std::uint32_t>(report.diagnostic.size()));
    out.append(report.diagnostic);
    return true;
}

bool DecodeReport(const std::string& bytes, Report& out)
{
    Reader reader(bytes);
    Report decoded{};
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t reserved16 = 0;
    std::uint32_t policy = 0;
    std::uint32_t outcome = 0;
    std::uint32_t failure = 0;
    std::uint8_t expected_item = 0;
    std::uint8_t observed_item = 0;
    if (!reader.U32(magic) || magic != ReportMagic
        || !reader.U16(version) || version != ReportVersion
        || !reader.U16(reserved16) || reserved16 != 0
        || !reader.U32(policy)
        || !reader.U32(outcome)
        || !reader.U32(failure)
        || !reader.U32(decoded.expected.expected_stat_waves)
        || !reader.U32(decoded.expected.expected_learned_waves)
        || !reader.U8(expected_item)
        || !reader.U8(observed_item)
        || !reader.U16(reserved16) || reserved16 != 0
        || !reader.U32(decoded.observed_stat_waves)
        || !reader.U32(decoded.observed_learned_waves)
        || !reader.U32(decoded.mismatch_flags)
        || !reader.U32(decoded.invariant_flags)
        || !reader.U64(decoded.start_vi)
        || !reader.U64(decoded.end_vi)) {
        return false;
    }
    decoded.policy = static_cast<AccelerationPolicy>(policy);
    decoded.outcome = static_cast<Outcome>(outcome);
    decoded.failure = static_cast<FailureCode>(failure);
    decoded.expected.expected_item_popup = expected_item != 0;
    decoded.observed_item_popup = observed_item != 0;
    if (!IsValidPolicy(decoded.policy)
        || outcome > static_cast<std::uint32_t>(Outcome::Failed)
        || !IsValidFailure(decoded.failure)
        || expected_item > 1
        || observed_item > 1) {
        return false;
    }

    for (std::size_t i = 0; i < decoded.expected.characters.size(); ++i) {
        auto& character = decoded.expected.characters[i];
        std::uint8_t learned_count = 0;
        if (!reader.U8(character.character_id)
            || !reader.U8(character.level_before)
            || !reader.U8(character.level_after)
            || !reader.U8(learned_count)
            || learned_count > kMaxLearnedIdsPerCharacter
            || !reader.U32(character.experience_before)
            || !reader.U32(character.experience_after)) {
            return false;
        }
        for (auto& element : character.elements) {
            if (!reader.U32(element.xp_before)
                || !reader.U32(element.xp_after)
                || !reader.U8(element.rank_before)
                || !reader.U8(element.rank_after)
                || !reader.U16(reserved16)
                || reserved16 != 0) {
                return false;
            }
        }
        character.learned_magic_ids.resize(learned_count);
        for (auto& id : character.learned_magic_ids) {
            if (!reader.U8(id) || id > 35) return false;
        }
    }

    std::uint32_t action_count = 0;
    if (!reader.U32(action_count) || action_count > kMaxActionCount) return false;
    decoded.actions.resize(action_count);
    for (auto& action : decoded.actions) {
        std::uint8_t kind = 0;
        std::uint8_t flags = 0;
        std::uint32_t ready_key = 0;
        std::uint32_t accepted_key = 0;
        if (!reader.U8(kind)
            || !reader.U8(action.state)
            || !reader.U8(action.substate)
            || !reader.U8(flags)
            || (flags & ~0x07u) != 0
            || !reader.U64(action.occurrence_token)
            || !reader.U32(ready_key)
            || !reader.U32(accepted_key)
            || !reader.U32(action.ready_pc)
            || !reader.U32(action.accepted_pc)
            || !reader.U64(action.neutral_epoch)
            || !reader.U64(action.input_epoch)
            || !reader.U64(action.release_epoch)
            || !reader.U32(action.input_poll_count)) {
            return false;
        }
        action.kind = static_cast<ActionKind>(kind);
        action.ready_key = static_cast<BPKey>(ready_key);
        action.accepted_key = static_cast<BPKey>(accepted_key);
        if (!IsValidActionKind(action.kind)) return false;
        action.input_acknowledged = (flags & (1u << 0)) != 0;
        action.release_observed = (flags & (1u << 1)) != 0;
        action.progress_observed = (flags & (1u << 2)) != 0;
    }

    std::uint32_t diagnostic_size = 0;
    if (!reader.U32(diagnostic_size)
        || diagnostic_size > kMaxDiagnosticBytes
        || !reader.Bytes(diagnostic_size, decoded.diagnostic)
        || !reader.done()) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

} // namespace phase::battle::endresults
