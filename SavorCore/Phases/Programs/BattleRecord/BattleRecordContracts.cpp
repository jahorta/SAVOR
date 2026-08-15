#include "BattleRecordModule.h"

#include "Core/Input/SoaBattle/BattleCommandCodec.h"
#include "Utils/Hash.h"

#include <array>
#include <limits>
#include <set>

namespace savor::runtime::battlerecord {
namespace {

constexpr std::array<std::uint8_t, 4> kPlanMagic{'B','R','P','1'};
constexpr std::array<std::uint8_t, 4> kSourceBindingMagic{'B','S','B','1'};
constexpr std::array<std::uint8_t, 4> kRequestMagic{'B','R','Q','1'};

void Diagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

class Writer
{
public:
    void U8(std::uint8_t value) { bytes.push_back(value); }
    void U32(std::uint32_t value)
    {
        for (unsigned shift = 0; shift != 32; shift += 8) U8(value >> shift);
    }
    void U64(std::uint64_t value)
    {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32u));
    }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void Blob(std::span<const std::uint8_t> value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> bytes;
};

class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}
    bool U8(std::uint8_t& value)
    {
        if (offset_ == bytes_.size()) return false;
        value = bytes_[offset_++]; return true;
    }
    bool U32(std::uint32_t& value)
    {
        value = 0;
        for (unsigned shift = 0; shift != 32; shift += 8)
        {
            std::uint8_t byte = 0;
            if (!U8(byte)) return false;
            value |= static_cast<std::uint32_t>(byte) << shift;
        }
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        std::uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        value = lo | (static_cast<std::uint64_t>(hi) << 32u); return true;
    }
    bool Text(std::string& value, std::uint32_t maximum = 65536)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > maximum || size > bytes_.size() - offset_)
            return false;
        value.assign(reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size; return true;
    }
    bool Blob(std::vector<std::uint8_t>& value, std::uint32_t maximum)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > maximum || size > bytes_.size() - offset_)
            return false;
        value.assign(bytes_.begin() + offset_, bytes_.begin() + offset_ + size);
        offset_ += size; return true;
    }
    bool Done() const noexcept { return offset_ == bytes_.size(); }
private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
};

std::vector<std::uint8_t> EncodePlanBody(const BattleReplayPlanV1& plan)
{
    Writer writer;
    for (const auto byte : kPlanMagic) writer.U8(byte);
    writer.U64(plan.selected_lineage.battle_set_id);
    writer.U64(plan.selected_lineage.wave_id);
    writer.U64(plan.selected_lineage.turn_job_id);
    writer.U64(plan.selected_lineage.execution_job_id);
    writer.U64(plan.battle_completion_id);
    writer.U8(static_cast<std::uint8_t>(plan.confirmed_seed_frame.buttons));
    writer.U8(static_cast<std::uint8_t>(plan.confirmed_seed_frame.buttons >> 8u));
    writer.U8(plan.confirmed_seed_frame.main_x);
    writer.U8(plan.confirmed_seed_frame.main_y);
    writer.U8(plan.confirmed_seed_frame.c_x);
    writer.U8(plan.confirmed_seed_frame.c_y);
    writer.U8(plan.confirmed_seed_frame.trig_l);
    writer.U8(plan.confirmed_seed_frame.trig_r);
    writer.U32(static_cast<std::uint32_t>(plan.turns.size()));
    for (const auto& turn : plan.turns)
    {
        writer.U32(turn.turn_index);
        writer.U32(turn.plan.fake_attack_count);
        std::vector<std::uint8_t> commands;
        soa::battle::actions::encode_battle_turn_commands_to_buffer(
            turn.plan.commands, commands);
        writer.Blob(commands);
        writer.U8(static_cast<std::uint8_t>(turn.expected_outcome));
        writer.U32(turn.expected_ending_rng);
    }
    std::vector<std::uint8_t> manifest;
    battlecompletion::EncodeBattleCompletionManifestV1(
        plan.expected_completion, manifest);
    writer.Blob(manifest);
    return std::move(writer.bytes);
}

std::vector<std::uint8_t> EncodeSourceBindingBody(
    const BattleReplaySourceBindingV1& binding)
{
    Writer writer;
    for (const auto byte : kSourceBindingMagic) writer.U8(byte);
    writer.U64(binding.source_savestate_id);
    writer.Text(binding.source_savestate_sha256);
    writer.U8(binding.source_dtm_artifact_id ? 1u : 0u);
    if (binding.source_dtm_artifact_id)
    {
        writer.U64(*binding.source_dtm_artifact_id);
        writer.Text(binding.source_dtm_sha256.value_or(std::string{}));
    }
    writer.U8(binding.source_itinerary_artifact_id ? 1u : 0u);
    if (binding.source_itinerary_artifact_id)
    {
        writer.U64(*binding.source_itinerary_artifact_id);
        writer.Text(binding.source_itinerary_sha256.value_or(std::string{}));
    }
    return std::move(writer.bytes);
}

} // namespace

std::string ComputeBattleReplayPlanHashV1(const BattleReplayPlanV1& plan)
{
    const auto bytes = EncodePlanBody(plan);
    return hash::sha256(bytes.data(), bytes.size());
}

bool ValidateBattleReplayPlanV1(
    const BattleReplayPlanV1& plan,
    std::string* diagnostic)
{
    const auto& lineage = plan.selected_lineage;
    if (!lineage.battle_set_id || !lineage.wave_id || !lineage.turn_job_id ||
        !lineage.execution_job_id || !plan.battle_completion_id ||
        plan.turns.empty() ||
        plan.turns.size() > 256)
    {
        Diagnostic(diagnostic, "Battle replay plan identity or source evidence is incomplete");
        return false;
    }
    for (std::size_t index = 0; index < plan.turns.size(); ++index)
    {
        const auto& turn = plan.turns[index];
        const auto expected_index = static_cast<std::uint32_t>(index + 1);
        if (turn.turn_index != expected_index || turn.plan.commands.empty() ||
            turn.plan.commands.size() > 4 ||
            (index + 1 == plan.turns.size()
                ? turn.expected_outcome !=
                    battlesingleturn::BattleSingleTurnOutcomeV1::Victory
                : turn.expected_outcome !=
                    battlesingleturn::BattleSingleTurnOutcomeV1::ReachedNextTurn))
        {
            Diagnostic(diagnostic, "Battle replay turns are not a contiguous Victory lineage");
            return false;
        }
    }
    std::vector<std::uint8_t> manifest;
    if (!battlecompletion::EncodeBattleCompletionManifestV1(
            plan.expected_completion, manifest))
    {
        Diagnostic(diagnostic, "Battle replay completion manifest is malformed");
        return false;
    }
    if (plan.expected_completion.lineage != plan.selected_lineage)
    {
        Diagnostic(diagnostic, "Battle replay completion lineage does not match the selected Victory");
        return false;
    }
    const auto actual = ComputeBattleReplayPlanHashV1(plan);
    if (!plan.canonical_sha256.empty() && plan.canonical_sha256 != actual)
    {
        Diagnostic(diagnostic, "Battle replay plan hash does not match its content");
        return false;
    }
    Diagnostic(diagnostic, {});
    return true;
}

std::vector<std::uint8_t> EncodeBattleReplayPlanV1(
    const BattleReplayPlanV1& plan,
    std::string* diagnostic)
{
    BattleReplayPlanV1 normalized = plan;
    normalized.canonical_sha256 = ComputeBattleReplayPlanHashV1(normalized);
    if (!ValidateBattleReplayPlanV1(normalized, diagnostic)) return {};
    auto bytes = EncodePlanBody(normalized);
    Writer writer;
    writer.Blob(bytes);
    writer.Text(normalized.canonical_sha256);
    Diagnostic(diagnostic, {});
    return std::move(writer.bytes);
}

bool DecodeBattleReplayPlanV1(
    std::span<const std::uint8_t> bytes,
    BattleReplayPlanV1& output,
    std::string* diagnostic)
{
    Reader envelope(bytes);
    std::vector<std::uint8_t> body;
    BattleReplayPlanV1 plan;
    if (!envelope.Blob(body, 1024 * 1024) ||
        !envelope.Text(plan.canonical_sha256, 64) || !envelope.Done())
    {
        Diagnostic(diagnostic, "Battle replay plan envelope is malformed");
        return false;
    }
    Reader reader(body);
    for (const auto expected : kPlanMagic)
    {
        std::uint8_t actual = 0;
        if (!reader.U8(actual) || actual != expected)
        {
            Diagnostic(diagnostic, "Battle replay plan magic is invalid");
            return false;
        }
    }
    if (!reader.U64(plan.selected_lineage.battle_set_id) ||
        !reader.U64(plan.selected_lineage.wave_id) ||
        !reader.U64(plan.selected_lineage.turn_job_id) ||
        !reader.U64(plan.selected_lineage.execution_job_id) ||
        !reader.U64(plan.battle_completion_id))
        return false;
    std::uint8_t lo = 0, hi = 0;
    if (!reader.U8(lo) || !reader.U8(hi) ||
        !reader.U8(plan.confirmed_seed_frame.main_x) ||
        !reader.U8(plan.confirmed_seed_frame.main_y) ||
        !reader.U8(plan.confirmed_seed_frame.c_x) ||
        !reader.U8(plan.confirmed_seed_frame.c_y) ||
        !reader.U8(plan.confirmed_seed_frame.trig_l) ||
        !reader.U8(plan.confirmed_seed_frame.trig_r)) return false;
    plan.confirmed_seed_frame.buttons = lo | (static_cast<std::uint16_t>(hi) << 8u);
    std::uint32_t turn_count = 0;
    if (!reader.U32(turn_count) || turn_count == 0 || turn_count > 256)
        return false;
    plan.turns.reserve(turn_count);
    for (std::uint32_t index = 0; index < turn_count; ++index)
    {
        BattleReplayTurnV1 turn;
        std::vector<std::uint8_t> commands;
        std::uint8_t outcome = 0;
        if (!reader.U32(turn.turn_index) ||
            !reader.U32(turn.plan.fake_attack_count) ||
            !reader.Blob(commands, 4096) ||
            !soa::battle::actions::decode_battle_turn_commands_from_buffer(
                commands, turn.plan.commands) ||
            !reader.U8(outcome) || outcome > 3 ||
            !reader.U32(turn.expected_ending_rng)) return false;
        turn.expected_outcome = static_cast<
            battlesingleturn::BattleSingleTurnOutcomeV1>(outcome);
        plan.turns.push_back(std::move(turn));
    }
    std::vector<std::uint8_t> manifest;
    if (!reader.Blob(manifest, 65536) || !reader.Done() ||
        !battlecompletion::DecodeBattleCompletionManifestV1(
            manifest, plan.expected_completion) ||
        plan.canonical_sha256 != hash::sha256(body.data(), body.size()) ||
        !ValidateBattleReplayPlanV1(plan, diagnostic)) return false;
    output = std::move(plan);
    Diagnostic(diagnostic, {});
    return true;
}

std::string ComputeBattleReplaySourceBindingHashV1(
    const BattleReplaySourceBindingV1& binding)
{
    const auto bytes = EncodeSourceBindingBody(binding);
    return hash::sha256(bytes.data(), bytes.size());
}

bool ValidateBattleReplaySourceBindingV1(
    const BattleReplaySourceBindingV1& binding,
    std::string* diagnostic)
{
    const bool dtm_complete =
        binding.source_dtm_artifact_id.has_value() ==
            binding.source_dtm_sha256.has_value();
    const bool itinerary_complete =
        binding.source_itinerary_artifact_id.has_value() ==
            binding.source_itinerary_sha256.has_value();
    if (!binding.source_savestate_id ||
        binding.source_savestate_sha256.size() != 64 || !dtm_complete ||
        !itinerary_complete ||
        (binding.source_dtm_artifact_id &&
            (!*binding.source_dtm_artifact_id ||
             binding.source_dtm_sha256->size() != 64)) ||
        (binding.source_itinerary_artifact_id &&
            (!*binding.source_itinerary_artifact_id ||
             binding.source_itinerary_sha256->size() != 64)))
    {
        Diagnostic(diagnostic,
            "Battle replay source binding evidence is incomplete");
        return false;
    }
    const auto actual = ComputeBattleReplaySourceBindingHashV1(binding);
    if (!binding.canonical_sha256.empty() &&
        binding.canonical_sha256 != actual)
    {
        Diagnostic(diagnostic,
            "Battle replay source binding hash does not match its content");
        return false;
    }
    Diagnostic(diagnostic, {});
    return true;
}

std::vector<std::uint8_t> EncodeBattleReplaySourceBindingV1(
    const BattleReplaySourceBindingV1& binding,
    std::string* diagnostic)
{
    BattleReplaySourceBindingV1 normalized = binding;
    normalized.canonical_sha256 =
        ComputeBattleReplaySourceBindingHashV1(normalized);
    if (!ValidateBattleReplaySourceBindingV1(normalized, diagnostic))
        return {};
    Writer writer;
    writer.Blob(EncodeSourceBindingBody(normalized));
    writer.Text(normalized.canonical_sha256);
    Diagnostic(diagnostic, {});
    return std::move(writer.bytes);
}

bool DecodeBattleReplaySourceBindingV1(
    std::span<const std::uint8_t> bytes,
    BattleReplaySourceBindingV1& output,
    std::string* diagnostic)
{
    Reader envelope(bytes);
    std::vector<std::uint8_t> body;
    BattleReplaySourceBindingV1 binding;
    if (!envelope.Blob(body, 4096) ||
        !envelope.Text(binding.canonical_sha256, 64) || !envelope.Done())
    {
        Diagnostic(diagnostic,
            "Battle replay source binding envelope is malformed");
        return false;
    }
    Reader reader(body);
    for (const auto expected : kSourceBindingMagic)
    {
        std::uint8_t actual = 0;
        if (!reader.U8(actual) || actual != expected)
        {
            Diagnostic(diagnostic,
                "Battle replay source binding magic is invalid");
            return false;
        }
    }
    std::uint8_t has_dtm = 0;
    std::uint8_t has_itinerary = 0;
    if (!reader.U64(binding.source_savestate_id) ||
        !reader.Text(binding.source_savestate_sha256, 64) ||
        !reader.U8(has_dtm) || has_dtm > 1)
        return false;
    if (has_dtm)
    {
        std::uint64_t id = 0;
        std::string sha;
        if (!reader.U64(id) || !reader.Text(sha, 64)) return false;
        binding.source_dtm_artifact_id = id;
        binding.source_dtm_sha256 = std::move(sha);
    }
    if (!reader.U8(has_itinerary) || has_itinerary > 1)
        return false;
    if (has_itinerary)
    {
        std::uint64_t id = 0;
        std::string sha;
        if (!reader.U64(id) || !reader.Text(sha, 64)) return false;
        binding.source_itinerary_artifact_id = id;
        binding.source_itinerary_sha256 = std::move(sha);
    }
    if (!reader.Done() ||
        binding.canonical_sha256 != hash::sha256(body.data(), body.size()) ||
        !ValidateBattleReplaySourceBindingV1(binding, diagnostic))
        return false;
    output = std::move(binding);
    Diagnostic(diagnostic, {});
    return true;
}

std::vector<std::uint8_t> EncodeBattleRecordExecutionInputV1(
    const BattleRecordRequestV1& request)
{
    Writer writer;
    for (const auto byte : kRequestMagic) writer.U8(byte);
    writer.Text(request.output_dtm_path);
    writer.Text(request.output_preseed_savestate_path);
    return std::move(writer.bytes);
}

bool DecodeBattleRecordExecutionInputV1(
    std::span<const std::uint8_t> bytes,
    BattleRecordRequestV1& output,
    std::string* diagnostic)
{
    Reader reader(bytes);
    for (const auto expected : kRequestMagic)
    {
        std::uint8_t actual = 0;
        if (!reader.U8(actual) || actual != expected)
        {
            Diagnostic(diagnostic, "battle.record request magic is invalid");
            return false;
        }
    }
    BattleRecordRequestV1 request;
    if (!reader.Text(request.output_dtm_path) ||
        !reader.Text(request.output_preseed_savestate_path) || !reader.Done())
    {
        Diagnostic(diagnostic, "battle.record request is malformed");
        return false;
    }
    output = std::move(request);
    Diagnostic(diagnostic, {});
    return true;
}

} // namespace savor::runtime::battlerecord
