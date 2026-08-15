#include "BattleCompletionContracts.h"

#include "Utils/Hash.h"

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <limits>
#include <utility>

namespace savor::runtime::battlecompletion {
namespace {

constexpr std::array<std::uint8_t, 4> kSnapshotMagic{'B','C','S','1'};
constexpr std::array<std::uint8_t, 4> kManifestMagic{'B','C','M','1'};
constexpr std::array<std::uint8_t, 4> kTransitionMagic{'F','T','C','1'};
constexpr std::array<std::uint8_t, 4> kTimingAnchorMagic{'B','T','A','1'};

void SetDiagnostic(std::string* output, std::string value)
{
    if (output) *output = std::move(value);
}

class Writer
{
public:
    explicit Writer(std::array<std::uint8_t, 4> magic)
    {
        bytes_.insert(bytes_.end(), magic.begin(), magic.end());
        U16(1);
        U16(0);
    }

    void U8(std::uint8_t value) { bytes_.push_back(value); }
    void U16(std::uint16_t value)
    {
        U8(static_cast<std::uint8_t>(value));
        U8(static_cast<std::uint8_t>(value >> 8u));
    }
    void U32(std::uint32_t value)
    {
        U16(static_cast<std::uint16_t>(value));
        U16(static_cast<std::uint16_t>(value >> 16u));
    }
    void U64(std::uint64_t value)
    {
        U32(static_cast<std::uint32_t>(value));
        U32(static_cast<std::uint32_t>(value >> 32u));
    }
    void Bool(bool value) { U8(value ? 1u : 0u); }
    void Bytes(std::span<const std::uint8_t> value)
    {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void Text(std::string_view value)
    {
        U32(static_cast<std::uint32_t>(value.size()));
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    std::vector<std::uint8_t> Finish() { return std::move(bytes_); }

private:
    std::vector<std::uint8_t> bytes_;
};

class Reader
{
public:
    Reader(std::span<const std::uint8_t> bytes,
           std::array<std::uint8_t, 4> magic)
        : bytes_(bytes)
    {
        if (bytes_.size() < 8 ||
            !std::equal(magic.begin(), magic.end(), bytes_.begin()))
            return;
        offset_ = 4;
        std::uint16_t version = 0, reserved = 0;
        valid_ = U16(version) && version == 1 && U16(reserved) && reserved == 0;
    }

    bool U8(std::uint8_t& value)
    {
        if (!valid_ || offset_ == bytes_.size()) return false;
        value = bytes_[offset_++];
        return true;
    }
    bool U16(std::uint16_t& value)
    {
        std::uint8_t lo = 0, hi = 0;
        if (!U8(lo) || !U8(hi)) return false;
        value = static_cast<std::uint16_t>(lo) |
            (static_cast<std::uint16_t>(hi) << 8u);
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        std::uint16_t lo = 0, hi = 0;
        if (!U16(lo) || !U16(hi)) return false;
        value = static_cast<std::uint32_t>(lo) |
            (static_cast<std::uint32_t>(hi) << 16u);
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        std::uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        value = static_cast<std::uint64_t>(lo) |
            (static_cast<std::uint64_t>(hi) << 32u);
        return true;
    }
    bool Bool(bool& value)
    {
        std::uint8_t encoded = 0;
        if (!U8(encoded) || encoded > 1) return false;
        value = encoded != 0;
        return true;
    }
    bool Bytes(std::span<std::uint8_t> output)
    {
        if (!valid_ || output.size() > bytes_.size() - offset_) return false;
        std::copy_n(bytes_.begin() + offset_, output.size(), output.begin());
        offset_ += output.size();
        return true;
    }
    bool Text(std::string& output, std::size_t maximum = 64)
    {
        std::uint32_t size = 0;
        if (!U32(size) || size > maximum || size > bytes_.size() - offset_)
            return false;
        output.assign(
            reinterpret_cast<const char*>(bytes_.data() + offset_), size);
        offset_ += size;
        return true;
    }
    bool Done() const noexcept { return valid_ && offset_ == bytes_.size(); }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t offset_ = 0;
    bool valid_ = true;
};

std::uint32_t U32BE(
    const std::array<std::uint8_t, CharacterRecordSize>& record,
    std::size_t offset)
{
    return (static_cast<std::uint32_t>(record[offset]) << 24u) |
        (static_cast<std::uint32_t>(record[offset + 1]) << 16u) |
        (static_cast<std::uint32_t>(record[offset + 2]) << 8u) |
        static_cast<std::uint32_t>(record[offset + 3]);
}

void EncodeProvenance(Writer& writer, const BattleStopProvenanceV1& value)
{
    writer.U32(value.pc);
    writer.U64(value.vi_count);
    writer.U64(value.workset_epoch);
}

bool DecodeProvenance(Reader& reader, BattleStopProvenanceV1& value)
{
    return reader.U32(value.pc) && reader.U64(value.vi_count) &&
        reader.U64(value.workset_epoch);
}

void EncodeSnapshotFields(Writer& writer, const BattleCompletionSnapshotV1& value)
{
    for (const auto& record : value.character_records) writer.Bytes(record);
    writer.U32(value.normal_experience_reward);
    writer.U32(value.magic_experience_reward);
    writer.U32(value.gold_reward);
    for (const auto& item : value.reward_items)
    {
        writer.U16(static_cast<std::uint16_t>(item.item_id));
        writer.U8(item.quantity);
        writer.U8(item.opaque);
    }
    writer.U32(value.rng_seed);
    writer.U32(value.battle_input_state);
    writer.U32(value.reward_phase);
}

bool DecodeSnapshotFields(Reader& reader, BattleCompletionSnapshotV1& value)
{
    for (auto& record : value.character_records)
        if (!reader.Bytes(record)) return false;
    if (!reader.U32(value.normal_experience_reward) ||
        !reader.U32(value.magic_experience_reward) ||
        !reader.U32(value.gold_reward)) return false;
    for (auto& item : value.reward_items)
    {
        std::uint16_t raw = 0;
        if (!reader.U16(raw) || !reader.U8(item.quantity) ||
            !reader.U8(item.opaque)) return false;
        item.item_id = static_cast<std::int16_t>(raw);
    }
    return reader.U32(value.rng_seed) &&
        reader.U32(value.battle_input_state) &&
        reader.U32(value.reward_phase);
}

std::vector<std::uint8_t> SemanticBytes(
    const BattleCompletionManifestV1& value)
{
    Writer writer(kManifestMagic);
    for (const auto& record : value.pre_character_records) writer.Bytes(record);
    for (const auto& record : value.post_character_records) writer.Bytes(record);
    writer.U32(value.normal_experience_reward);
    writer.U32(value.magic_experience_reward);
    writer.U32(value.gold_reward);
    for (const auto& item : value.reward_items)
    {
        writer.U16(static_cast<std::uint16_t>(item.item_id));
        writer.U8(item.quantity);
        writer.U8(item.opaque);
    }
    writer.U32(value.presentation.gold_pages);
    writer.U32(value.presentation.normal_exp_pages);
    writer.U32(value.presentation.level_panels);
    writer.U32(value.presentation.stat_waves);
    writer.U32(value.presentation.magic_exp_pages);
    writer.U32(value.presentation.magic_rank_events);
    writer.U32(value.presentation.learned_magic_waves);
    writer.U32(value.presentation.item_popups);
    writer.U32(value.entry_rng_seed);
    writer.U32(value.completion_rng_seed);
    writer.U32(value.transition.area);
    writer.U8(value.transition.raw_suffix);
    writer.U8(value.transition.effective_suffix);
    writer.Bool(value.transition.used_area_99_suffix);
    writer.Text(value.transition.sct_filename);
    writer.U32(value.transition.rng_seed);
    return writer.Finish();
}

} // namespace

bool BuildBattleCompletionManifestV1(
    BattleCompletionLineageV1 lineage,
    const BattleCompletionSnapshotV1& before,
    const BattleCompletionSnapshotV1& after,
    BattleStopProvenanceV1 entry,
    BattleStopProvenanceV1 reward_entry,
    BattleStopProvenanceV1 reward_commit,
    FieldTransitionContextV1 transition,
    BattleCompletionManifestV1& output,
    std::string* diagnostic)
{
    if (entry.pc != 0x800706d8u || reward_entry.pc != 0x8006f598u ||
        reward_commit.pc != 0x8006fd58u ||
        (transition.provenance.pc != 0x80101894u &&
         transition.provenance.pc != 0x801018acu))
    {
        SetDiagnostic(diagnostic, "Battle completion provenance is not at the canonical points");
        return false;
    }
    if (after.battle_input_state != 2 || after.reward_phase != 6)
    {
        SetDiagnostic(diagnostic, "Battle completion state invariants are not satisfied");
        return false;
    }
    if (lineage.battle_set_id == 0 || lineage.wave_id == 0 ||
        lineage.turn_job_id == 0 || lineage.execution_job_id == 0)
    {
        SetDiagnostic(diagnostic, "Battle completion lineage is incomplete");
        return false;
    }
    BattleCompletionManifestV1 result{};
    result.lineage = lineage;
    result.pre_character_records = before.character_records;
    result.post_character_records = after.character_records;
    result.normal_experience_reward = after.normal_experience_reward;
    result.magic_experience_reward = after.magic_experience_reward;
    result.gold_reward = after.gold_reward;
    result.reward_items = after.reward_items;
    result.entry_rng_seed = before.rng_seed;
    result.completion_rng_seed = after.rng_seed;
    result.entry = entry;
    result.reward_entry = reward_entry;
    result.reward_commit = reward_commit;
    result.transition = std::move(transition);

    std::uint32_t learned_pages = 0;
    for (std::size_t character = 0; character < CharacterCount; ++character)
    {
        const auto& pre = result.pre_character_records[character];
        const auto& post = result.post_character_records[character];
        if (post[0x0b] < pre[0x0b] || U32BE(post, 0x24) < U32BE(pre, 0x24))
        {
            SetDiagnostic(diagnostic, "Battle rewards decreased persistent character progression");
            return false;
        }
        result.presentation.level_panels +=
            static_cast<std::uint32_t>(post[0x0b] - pre[0x0b]);
        std::uint32_t learned = 0;
        for (std::size_t element = 0; element < 6; ++element)
        {
            const auto pre_rank = pre[0x34 + element];
            const auto post_rank = post[0x34 + element];
            if (pre_rank > 6 || post_rank > 6 || post_rank < pre_rank ||
                U32BE(post, 0x44 + element * 4) <
                    U32BE(pre, 0x44 + element * 4))
            {
                SetDiagnostic(diagnostic, "Battle rewards produced malformed element progression");
                return false;
            }
            result.presentation.magic_rank_events +=
                static_cast<std::uint32_t>(post_rank - pre_rank);
            learned += static_cast<std::uint32_t>(post_rank - pre_rank);
        }
        learned_pages = std::max(learned_pages, (learned + 1u) / 2u);
    }
    result.presentation.gold_pages = result.gold_reward != 0 ? 1u : 0u;
    result.presentation.normal_exp_pages =
        result.normal_experience_reward != 0 ? 1u : 0u;
    result.presentation.magic_exp_pages =
        result.magic_experience_reward != 0 ? 1u : 0u;
    result.presentation.stat_waves =
        result.presentation.level_panels != 0 ? 3u : 0u;
    result.presentation.learned_magic_waves = learned_pages;
    const bool has_items = std::ranges::any_of(
        result.reward_items,
        [](const BattleRewardItemV1& item) { return item.item_id >= 0; });
    result.presentation.item_popups = has_items &&
        (result.normal_experience_reward != 0 ||
         result.magic_experience_reward != 0) ? 1u : 0u;
    output = std::move(result);
    SetDiagnostic(diagnostic, {});
    return true;
}

bool EncodeBattleCompletionSnapshotV1(
    const BattleCompletionSnapshotV1& value,
    std::vector<std::uint8_t>& output)
{
    Writer writer(kSnapshotMagic);
    EncodeSnapshotFields(writer, value);
    output = writer.Finish();
    return true;
}

bool DecodeBattleCompletionSnapshotV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionSnapshotV1& output)
{
    Reader reader(bytes, kSnapshotMagic);
    BattleCompletionSnapshotV1 decoded{};
    if (!DecodeSnapshotFields(reader, decoded) || !reader.Done()) return false;
    output = std::move(decoded);
    return true;
}

bool EncodeBattleCompletionManifestV1(
    const BattleCompletionManifestV1& value,
    std::vector<std::uint8_t>& output)
{
    Writer writer(kManifestMagic);
    const auto semantic = SemanticBytes(value);
    writer.U32(static_cast<std::uint32_t>(semantic.size()));
    writer.Bytes(semantic);
    writer.U64(value.lineage.battle_set_id);
    writer.U64(value.lineage.wave_id);
    writer.U64(value.lineage.turn_job_id);
    writer.U64(value.lineage.execution_job_id);
    EncodeProvenance(writer, value.entry);
    EncodeProvenance(writer, value.reward_entry);
    EncodeProvenance(writer, value.reward_commit);
    EncodeProvenance(writer, value.transition.provenance);
    output = writer.Finish();
    return true;
}

bool DecodeBattleCompletionManifestV1(
    std::span<const std::uint8_t> bytes,
    BattleCompletionManifestV1& output)
{
    Reader reader(bytes, kManifestMagic);
    std::uint32_t semantic_size = 0;
    if (!reader.U32(semantic_size) || semantic_size > 4096) return false;
    std::vector<std::uint8_t> semantic(semantic_size);
    if (!reader.Bytes(semantic)) return false;
    Reader body(semantic, kManifestMagic);
    BattleCompletionManifestV1 decoded{};
    for (auto& record : decoded.pre_character_records)
        if (!body.Bytes(record)) return false;
    for (auto& record : decoded.post_character_records)
        if (!body.Bytes(record)) return false;
    if (!body.U32(decoded.normal_experience_reward) ||
        !body.U32(decoded.magic_experience_reward) ||
        !body.U32(decoded.gold_reward)) return false;
    for (auto& item : decoded.reward_items)
    {
        std::uint16_t raw = 0;
        if (!body.U16(raw) || !body.U8(item.quantity) || !body.U8(item.opaque))
            return false;
        item.item_id = static_cast<std::int16_t>(raw);
    }
    auto& presentation = decoded.presentation;
    if (!body.U32(presentation.gold_pages) ||
        !body.U32(presentation.normal_exp_pages) ||
        !body.U32(presentation.level_panels) ||
        !body.U32(presentation.stat_waves) ||
        !body.U32(presentation.magic_exp_pages) ||
        !body.U32(presentation.magic_rank_events) ||
        !body.U32(presentation.learned_magic_waves) ||
        !body.U32(presentation.item_popups) ||
        !body.U32(decoded.entry_rng_seed) ||
        !body.U32(decoded.completion_rng_seed) ||
        !body.U32(decoded.transition.area) ||
        !body.U8(decoded.transition.raw_suffix) ||
        !body.U8(decoded.transition.effective_suffix) ||
        !body.Bool(decoded.transition.used_area_99_suffix) ||
        !body.Text(decoded.transition.sct_filename) ||
        !body.U32(decoded.transition.rng_seed) || !body.Done() ||
        !reader.U64(decoded.lineage.battle_set_id) ||
        !reader.U64(decoded.lineage.wave_id) ||
        !reader.U64(decoded.lineage.turn_job_id) ||
        !reader.U64(decoded.lineage.execution_job_id) ||
        !DecodeProvenance(reader, decoded.entry) ||
        !DecodeProvenance(reader, decoded.reward_entry) ||
        !DecodeProvenance(reader, decoded.reward_commit) ||
        !DecodeProvenance(reader, decoded.transition.provenance) ||
        !reader.Done()) return false;
    output = std::move(decoded);
    return true;
}

bool EncodeFieldTransitionContextV1(
    const FieldTransitionContextV1& value,
    std::vector<std::uint8_t>& output)
{
    Writer writer(kTransitionMagic);
    writer.U32(value.area);
    writer.U8(value.raw_suffix);
    writer.U8(value.effective_suffix);
    writer.Bool(value.used_area_99_suffix);
    writer.Text(value.sct_filename);
    writer.U32(value.rng_seed);
    EncodeProvenance(writer, value.provenance);
    output = writer.Finish();
    return true;
}

bool DecodeFieldTransitionContextV1(
    std::span<const std::uint8_t> bytes,
    FieldTransitionContextV1& output)
{
    Reader reader(bytes, kTransitionMagic);
    FieldTransitionContextV1 decoded{};
    if (!reader.U32(decoded.area) || !reader.U8(decoded.raw_suffix) ||
        !reader.U8(decoded.effective_suffix) ||
        !reader.Bool(decoded.used_area_99_suffix) ||
        !reader.Text(decoded.sct_filename) || !reader.U32(decoded.rng_seed) ||
        !DecodeProvenance(reader, decoded.provenance) || !reader.Done())
        return false;
    output = std::move(decoded);
    return true;
}

bool EncodeBattleTimingAdjustmentAnchorV1(
    const BattleTimingAdjustmentAnchorV1& value,
    std::vector<std::uint8_t>& output)
{
    if (value.turn_index == 0 || value.actor_slot > 3 ||
        value.command_ordinal == 0 || value.semantic_role.empty() ||
        value.semantic_role.size() > 128 || value.dtm_input_index == 0)
        return false;
    Writer writer(kTimingAnchorMagic);
    writer.U32(value.turn_index);
    writer.U8(value.actor_slot);
    writer.U32(value.command_ordinal);
    writer.Text(value.semantic_role);
    writer.U64(value.dtm_input_index);
    output = writer.Finish();
    return true;
}

bool DecodeBattleTimingAdjustmentAnchorV1(
    std::span<const std::uint8_t> bytes,
    BattleTimingAdjustmentAnchorV1& output)
{
    Reader reader(bytes, kTimingAnchorMagic);
    BattleTimingAdjustmentAnchorV1 decoded{};
    if (!reader.U32(decoded.turn_index) || !reader.U8(decoded.actor_slot) ||
        !reader.U32(decoded.command_ordinal) ||
        !reader.Text(decoded.semantic_role, 128) ||
        !reader.U64(decoded.dtm_input_index) || !reader.Done() ||
        decoded.turn_index == 0 || decoded.actor_slot > 3 ||
        decoded.command_ordinal == 0 || decoded.semantic_role.empty() ||
        decoded.dtm_input_index == 0)
        return false;
    output = std::move(decoded);
    return true;
}

std::string SemanticBattleCompletionManifestHashV1(
    const BattleCompletionManifestV1& value)
{
    const auto bytes = SemanticBytes(value);
    return hash::sha256(bytes.data(), bytes.size());
}

bool SemanticallyEqualBattleCompletionManifestV1(
    const BattleCompletionManifestV1& lhs,
    const BattleCompletionManifestV1& rhs) noexcept
{
    return SemanticBytes(lhs) == SemanticBytes(rhs);
}

bool ReconstructFieldSctFilenameV1(
    std::uint32_t area,
    std::uint8_t raw_suffix,
    std::optional<std::uint8_t> area_99_suffix_index,
    std::uint8_t& effective_suffix,
    std::string& filename,
    std::string* diagnostic)
{
    if (area > 999)
    {
        SetDiagnostic(diagnostic, "Field area lies outside 000 through 999");
        return false;
    }
    effective_suffix = raw_suffix;
    if (area == 99)
    {
        if (!area_99_suffix_index || *area_99_suffix_index > 25)
        {
            SetDiagnostic(diagnostic, "Area 99 requires a suffix index in 0 through 25");
            return false;
        }
        effective_suffix = static_cast<std::uint8_t>('a' + *area_99_suffix_index);
    }
    if (effective_suffix < 0x20 || effective_suffix > 0x7e)
    {
        SetDiagnostic(diagnostic, "Field suffix is not a printable ASCII character");
        return false;
    }
    char buffer[16]{};
    const int count = std::snprintf(
        buffer, sizeof(buffer), "me%03u%c.sct", area,
        static_cast<char>(effective_suffix));
    if (count <= 0 || static_cast<std::size_t>(count) >= sizeof(buffer))
    {
        SetDiagnostic(diagnostic, "Field SCT filename could not be reconstructed");
        return false;
    }
    filename.assign(buffer, static_cast<std::size_t>(count));
    SetDiagnostic(diagnostic, {});
    return true;
}

std::optional<FieldContinuationKindV1> ClassifyFieldContinuationV1(
    std::string_view filename,
    std::string* diagnostic)
{
    if (filename.size() != 10 || !filename.starts_with("me") ||
        !filename.ends_with(".sct"))
    {
        SetDiagnostic(diagnostic, "Field SCT filename is not canonical meNNNx.sct");
        return std::nullopt;
    }
    unsigned area = 0;
    const auto converted = std::from_chars(
        filename.data() + 2, filename.data() + 5, area);
    if (converted.ec != std::errc{} || converted.ptr != filename.data() + 5)
    {
        SetDiagnostic(diagnostic, "Field SCT filename has a malformed area number");
        return std::nullopt;
    }
    SetDiagnostic(diagnostic, {});
    if (area == 99) return FieldContinuationKindV1::OverworldNavigation;
    if (area < 200) return FieldContinuationKindV1::FieldNavigation;
    if (area < 500) return FieldContinuationKindV1::Cutscene;
    return FieldContinuationKindV1::ShipRuntime;
}

} // namespace savor::runtime::battlecompletion
