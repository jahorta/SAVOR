#include "BattleCompletionManifest.h"

#include <algorithm>
#include <limits>
#include <span>
#include <string_view>

namespace phase::battle::completion {
namespace {

void PutU8(std::string& out, std::uint8_t value) { out.push_back(static_cast<char>(value)); }
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

std::uint32_t U32BE(const std::array<std::uint8_t, CharacterRecordSize>& record,
                    std::size_t offset)
{
    return (static_cast<std::uint32_t>(record[offset]) << 24)
        | (static_cast<std::uint32_t>(record[offset + 1]) << 16)
        | (static_cast<std::uint32_t>(record[offset + 2]) << 8)
        | static_cast<std::uint32_t>(record[offset + 3]);
}

class Reader {
public:
    explicit Reader(std::string_view bytes) : bytes_(bytes) {}
    bool U8(std::uint8_t& value)
    {
        if (offset_ == bytes_.size()) return false;
        value = static_cast<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    bool U16(std::uint16_t& value)
    {
        std::uint8_t lo = 0, hi = 0;
        if (!U8(lo) || !U8(hi)) return false;
        value = static_cast<std::uint16_t>(lo | (static_cast<std::uint16_t>(hi) << 8));
        return true;
    }
    bool U32(std::uint32_t& value)
    {
        std::uint16_t lo = 0, hi = 0;
        if (!U16(lo) || !U16(hi)) return false;
        value = static_cast<std::uint32_t>(lo) | (static_cast<std::uint32_t>(hi) << 16);
        return true;
    }
    bool U64(std::uint64_t& value)
    {
        std::uint32_t lo = 0, hi = 0;
        if (!U32(lo) || !U32(hi)) return false;
        value = static_cast<std::uint64_t>(lo) | (static_cast<std::uint64_t>(hi) << 32);
        return true;
    }
    bool Bytes(std::span<std::uint8_t> out)
    {
        if (out.size() > bytes_.size() - offset_) return false;
        std::copy_n(
            reinterpret_cast<const std::uint8_t*>(bytes_.data() + offset_),
            out.size(), out.begin());
        offset_ += out.size();
        return true;
    }
    bool done() const noexcept { return offset_ == bytes_.size(); }

private:
    std::string_view bytes_;
    std::size_t offset_{0};
};

} // namespace

bool DeriveExpectedView(Manifest& manifest, std::string* error)
{
    manifest.expected = {};
    manifest.presentation = {};
    std::uint32_t invariant_flags = manifest.invariant_flags
        & (ManifestInvariantPreCaptured
            | ManifestInvariantPostCaptured
            | ManifestInvariantRewardItemsCaptured);
    bool levels_ok = true;
    bool experience_ok = true;
    bool element_xp_ok = true;
    bool ranks_ok = true;
    std::uint32_t maximum_learned_pages = 0;

    for (std::size_t character_index = 0;
         character_index < CharacterCount;
         ++character_index) {
        const auto& before = manifest.pre_character_records[character_index];
        const auto& after = manifest.post_character_records[character_index];
        auto& progress = manifest.expected.characters[character_index];
        progress.character_id = static_cast<std::uint8_t>(character_index);
        progress.level_before = before[0x0B];
        progress.level_after = after[0x0B];
        progress.experience_before = U32BE(before, 0x24);
        progress.experience_after = U32BE(after, 0x24);
        levels_ok = levels_ok && progress.level_after >= progress.level_before;
        experience_ok = experience_ok
            && progress.experience_after >= progress.experience_before;
        if (progress.level_after >= progress.level_before) {
            manifest.presentation.level_panels +=
                static_cast<std::uint32_t>(progress.level_after - progress.level_before);
        }

        for (std::size_t element = 0;
             element < phase::battle::endresults::ElementCount;
             ++element) {
            auto& element_progress = progress.elements[element];
            element_progress.rank_before = before[0x34 + element];
            element_progress.rank_after = after[0x34 + element];
            element_progress.xp_before = U32BE(before, 0x44 + element * 4);
            element_progress.xp_after = U32BE(after, 0x44 + element * 4);
            if (element_progress.rank_before > 6 || element_progress.rank_after > 6) {
                if (error) *error = "element rank lies outside 0..6";
                return false;
            }
            ranks_ok = ranks_ok && element_progress.rank_after >= element_progress.rank_before;
            element_xp_ok = element_xp_ok
                && element_progress.xp_after >= element_progress.xp_before;
            if (element_progress.rank_after >= element_progress.rank_before) {
                manifest.presentation.magic_rank_events +=
                    static_cast<std::uint32_t>(
                        element_progress.rank_after - element_progress.rank_before);
            }
            for (std::uint8_t rank = element_progress.rank_before;
                 rank < element_progress.rank_after;
                 ++rank) {
                progress.learned_magic_ids.push_back(
                    static_cast<std::uint8_t>(element * 6 + rank));
            }
        }
        maximum_learned_pages = std::max(
            maximum_learned_pages,
            static_cast<std::uint32_t>((progress.learned_magic_ids.size() + 1) / 2));
    }

    manifest.presentation.gold_pages = manifest.gold_reward != 0 ? 1u : 0u;
    manifest.presentation.normal_exp_pages =
        manifest.normal_experience_reward != 0 ? 1u : 0u;
    manifest.presentation.magic_exp_pages =
        manifest.magic_experience_reward != 0 ? 1u : 0u;
    manifest.presentation.stat_waves =
        manifest.presentation.level_panels != 0 ? 3u : 0u;
    manifest.presentation.learned_magic_waves = maximum_learned_pages;
    const bool has_items = std::any_of(
        manifest.reward_items.begin(), manifest.reward_items.end(),
        [](const RewardItem& item) { return item.item_id >= 0; });
    manifest.presentation.item_popups = has_items
        && (manifest.normal_experience_reward != 0
            || manifest.magic_experience_reward != 0) ? 1u : 0u;
    manifest.expected.expected_stat_waves = manifest.presentation.stat_waves;
    manifest.expected.expected_learned_waves = manifest.presentation.learned_magic_waves;
    manifest.expected.expected_item_popup = manifest.presentation.item_popups != 0;

    if (levels_ok) invariant_flags |= ManifestInvariantLevelNondecreasing;
    if (experience_ok) invariant_flags |= ManifestInvariantExperienceNondecreasing;
    if (element_xp_ok) invariant_flags |= ManifestInvariantElementXpNondecreasing;
    if (ranks_ok) invariant_flags |= ManifestInvariantElementRankNondecreasing;
    invariant_flags |= ManifestInvariantExpectedViewDerived;
    manifest.invariant_flags = invariant_flags;
    if ((manifest.invariant_flags & RequiredManifestInvariants)
        != RequiredManifestInvariants) {
        if (error) *error = "persistent progression invariants failed";
        return false;
    }
    return true;
}

bool EncodeManifest(const Manifest& manifest, std::string& out)
{
    if ((manifest.invariant_flags & RequiredManifestInvariants)
        != RequiredManifestInvariants) return false;
    out.clear();
    PutU32(out, ManifestMagic);
    PutU16(out, ManifestVersion);
    PutU16(out, 0);
    PutU32(out, manifest.invariant_flags);
    PutU64(out, manifest.start_vi);
    PutU64(out, manifest.end_vi);
    for (const auto& record : manifest.pre_character_records)
        out.append(reinterpret_cast<const char*>(record.data()), record.size());
    for (const auto& record : manifest.post_character_records)
        out.append(reinterpret_cast<const char*>(record.data()), record.size());
    PutU32(out, manifest.normal_experience_reward);
    PutU32(out, manifest.magic_experience_reward);
    PutU32(out, manifest.gold_reward);
    for (const auto& item : manifest.reward_items) {
        PutU16(out, static_cast<std::uint16_t>(item.item_id));
        PutU8(out, item.quantity);
        PutU8(out, item.opaque);
    }
    PutU32(out, manifest.presentation.gold_pages);
    PutU32(out, manifest.presentation.normal_exp_pages);
    PutU32(out, manifest.presentation.level_panels);
    PutU32(out, manifest.presentation.stat_waves);
    PutU32(out, manifest.presentation.magic_exp_pages);
    PutU32(out, manifest.presentation.magic_rank_events);
    PutU32(out, manifest.presentation.learned_magic_waves);
    PutU32(out, manifest.presentation.item_popups);
    return true;
}

bool DecodeManifest(const std::string& bytes, Manifest& out)
{
    Reader reader(bytes);
    Manifest decoded{};
    std::uint32_t magic = 0;
    std::uint16_t version = 0, reserved = 0;
    if (!reader.U32(magic) || magic != ManifestMagic
        || !reader.U16(version) || version != ManifestVersion
        || !reader.U16(reserved) || reserved != 0
        || !reader.U32(decoded.invariant_flags)
        || !reader.U64(decoded.start_vi)
        || !reader.U64(decoded.end_vi)) return false;
    for (auto& record : decoded.pre_character_records)
        if (!reader.Bytes(record)) return false;
    for (auto& record : decoded.post_character_records)
        if (!reader.Bytes(record)) return false;
    if (!reader.U32(decoded.normal_experience_reward)
        || !reader.U32(decoded.magic_experience_reward)
        || !reader.U32(decoded.gold_reward)) return false;
    for (auto& item : decoded.reward_items) {
        std::uint16_t id = 0;
        if (!reader.U16(id) || !reader.U8(item.quantity) || !reader.U8(item.opaque))
            return false;
        item.item_id = static_cast<std::int16_t>(id);
    }
    if (!reader.U32(decoded.presentation.gold_pages)
        || !reader.U32(decoded.presentation.normal_exp_pages)
        || !reader.U32(decoded.presentation.level_panels)
        || !reader.U32(decoded.presentation.stat_waves)
        || !reader.U32(decoded.presentation.magic_exp_pages)
        || !reader.U32(decoded.presentation.magic_rank_events)
        || !reader.U32(decoded.presentation.learned_magic_waves)
        || !reader.U32(decoded.presentation.item_popups)
        || !reader.done()) return false;

    const auto encoded_presentation = decoded.presentation;
    std::string error;
    if (!DeriveExpectedView(decoded, &error)
        || decoded.presentation.gold_pages != encoded_presentation.gold_pages
        || decoded.presentation.normal_exp_pages != encoded_presentation.normal_exp_pages
        || decoded.presentation.level_panels != encoded_presentation.level_panels
        || decoded.presentation.stat_waves != encoded_presentation.stat_waves
        || decoded.presentation.magic_exp_pages != encoded_presentation.magic_exp_pages
        || decoded.presentation.magic_rank_events != encoded_presentation.magic_rank_events
        || decoded.presentation.learned_magic_waves != encoded_presentation.learned_magic_waves
        || decoded.presentation.item_popups != encoded_presentation.item_popups) {
        return false;
    }
    out = std::move(decoded);
    return true;
}

} // namespace phase::battle::completion
