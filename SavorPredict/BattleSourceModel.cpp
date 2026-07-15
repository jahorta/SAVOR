#include "BattleSourceModel.h"

#include <Utils/Hash.h>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace savor::predict {
namespace {

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::nullopt;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::optional<std::string_view> find_value_span(
    std::string_view object,
    std::string_view key) {
    const auto quoted_key = '"' + std::string(key) + '"';
    const auto key_pos = object.find(quoted_key);
    if (key_pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto colon = object.find(':', key_pos + quoted_key.size());
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    auto begin = colon + 1;
    while (begin < object.size()
        && std::isspace(static_cast<unsigned char>(object[begin]))) {
        ++begin;
    }
    if (begin >= object.size()) {
        return std::nullopt;
    }

    if (object[begin] == '"') {
        bool escaped = false;
        for (std::size_t end = begin + 1; end < object.size(); ++end) {
            if (escaped) {
                escaped = false;
            } else if (object[end] == '\\') {
                escaped = true;
            } else if (object[end] == '"') {
                return object.substr(begin, end - begin + 1);
            }
        }
        return std::nullopt;
    }

    if (object[begin] == '{' || object[begin] == '[') {
        const char open = object[begin];
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t end = begin; end < object.size(); ++end) {
            const char c = object[end];
            if (escaped) {
                escaped = false;
                continue;
            }
            if (in_string) {
                if (c == '\\') {
                    escaped = true;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == open) {
                ++depth;
            } else if (c == close && --depth == 0) {
                return object.substr(begin, end - begin + 1);
            }
        }
        return std::nullopt;
    }

    auto end = begin;
    while (end < object.size() && object[end] != ',' && object[end] != '}') {
        ++end;
    }
    while (end > begin
        && std::isspace(static_cast<unsigned char>(object[end - 1]))) {
        --end;
    }
    return object.substr(begin, end - begin);
}

std::optional<std::string> parse_string(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value() || value->size() < 2
        || value->front() != '"' || value->back() != '"') {
        return std::nullopt;
    }
    std::string out;
    bool escaped = false;
    for (std::size_t index = 1; index + 1 < value->size(); ++index) {
        const char c = (*value)[index];
        if (escaped) {
            switch (c) {
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(c); break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::optional<int> parse_int(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (!value.has_value()) {
        return std::nullopt;
    }
    int parsed = 0;
    const auto [end, error] = std::from_chars(
        value->data(), value->data() + value->size(), parsed);
    if (error != std::errc() || end != value->data() + value->size()) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<bool> parse_bool(std::string_view object, std::string_view key) {
    const auto value = find_value_span(object, key);
    if (value == "true") {
        return true;
    }
    if (value == "false") {
        return false;
    }
    return std::nullopt;
}

std::vector<std::string_view> split_top_level_objects(std::string_view array) {
    std::vector<std::string_view> objects;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    std::size_t start = std::string_view::npos;
    for (std::size_t index = 0; index < array.size(); ++index) {
        const char c = array[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            if (depth++ == 0) {
                start = index;
            }
        } else if (c == '}' && --depth == 0 && start != std::string_view::npos) {
            objects.push_back(array.substr(start, index - start + 1));
            start = std::string_view::npos;
        }
    }
    return objects;
}

std::vector<std::string> parse_string_array(
    std::string_view object,
    std::string_view key) {
    std::vector<std::string> values;
    const auto array = find_value_span(object, key);
    if (!array.has_value() || array->size() < 2
        || array->front() != '[' || array->back() != ']') {
        return values;
    }

    bool in_string = false;
    bool escaped = false;
    std::string value;
    for (std::size_t index = 1; index + 1 < array->size(); ++index) {
        const char c = (*array)[index];
        if (!in_string) {
            if (c == '"') {
                in_string = true;
                value.clear();
            }
            continue;
        }
        if (escaped) {
            value.push_back(c);
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            values.push_back(value);
            in_string = false;
        } else {
            value.push_back(c);
        }
    }
    if (in_string || escaped) {
        values.clear();
    }
    return values;
}

std::string lowercase_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

BattleSourceFieldStatus parse_status(std::string_view object) {
    const auto value = parse_string(object, "status");
    if (value == "Exact") {
        return BattleSourceFieldStatus::Exact;
    }
    if (value == "Invalid") {
        return BattleSourceFieldStatus::Invalid;
    }
    return BattleSourceFieldStatus::MissingInput;
}

std::optional<std::array<std::uint8_t, 81>> parse_terrain(
    std::string_view terrain_object) {
    const auto rows = find_value_span(terrain_object, "source9x9Rows");
    if (!rows.has_value()) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 81> terrain{};
    std::size_t count = 0;
    for (std::size_t index = 1; index + 1 < rows->size();) {
        if (!std::isdigit(static_cast<unsigned char>((*rows)[index]))) {
            ++index;
            continue;
        }
        unsigned parsed = 0;
        const auto [end, error] = std::from_chars(
            rows->data() + index, rows->data() + rows->size(), parsed);
        if (error != std::errc() || parsed > 0xffU || count >= terrain.size()) {
            return std::nullopt;
        }
        terrain[count++] = static_cast<std::uint8_t>(parsed);
        index = static_cast<std::size_t>(end - rows->data());
    }
    return count == terrain.size()
        ? std::optional<std::array<std::uint8_t, 81>>{terrain}
        : std::nullopt;
}

std::filesystem::path find_manifest(
    const std::filesystem::path& root,
    std::string_view key) {
    const auto direct = root / std::string(key) / "manifest.json";
    if (std::filesystem::exists(direct)) {
        return direct;
    }
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }
        const auto candidate = entry.path() / "manifest.json";
        const auto text = read_text_file(candidate);
        if (text.has_value() && parse_string(*text, "key") == key) {
            return candidate;
        }
    }
    return {};
}

std::string context_roster_fingerprint(
    const soa::battle::ctx::BattleContext& context) {
    std::ostringstream out;
    bool first = true;
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto& combatant = context.slots_[slot];
        if (combatant.present == 0) {
            continue;
        }
        if (!first) {
            out << '|';
        }
        first = false;
        out << slot << ':' << (combatant.is_player != 0 ? "pc" : "enemy")
            << ':' << combatant.id;
    }
    return out.str();
}

std::string indexed_resource_stem(std::string_view prefix, int value) {
    std::ostringstream out;
    out << prefix << std::setw(3) << std::setfill('0') << value;
    return out.str();
}

} // namespace

std::filesystem::path default_battle_source_manifest_root() {
    const std::array<std::filesystem::path, 3> candidates = {{
        std::filesystem::current_path() / "SavorPredict" / "Data" / "BattleSources",
        std::filesystem::current_path() / "Data" / "BattleSources",
        std::filesystem::current_path() / ".." / ".." / "SavorPredict" / "Data" / "BattleSources",
    }};
    for (const auto& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::is_directory(candidate, error) && !error) {
            return std::filesystem::weakly_canonical(candidate, error);
        }
    }
    return candidates.front();
}

BattleSourceBundleLoadResult load_battle_source_bundle(
    std::string_view manifest_key,
    const std::filesystem::path& manifest_root) {
    BattleSourceBundleLoadResult result;
    const auto root = manifest_root.empty()
        ? default_battle_source_manifest_root()
        : manifest_root;
    result.manifest_path = find_manifest(root, manifest_key);
    if (result.manifest_path.empty()) {
        result.errors.push_back(
            "battle source manifest not found for key " + std::string(manifest_key)
            + " under " + root.string());
        return result;
    }
    const auto manifest_text = read_text_file(result.manifest_path);
    if (!manifest_text.has_value()
        || parse_string(*manifest_text, "schema") != "savor_battle_source_manifest_v1") {
        result.errors.push_back("battle source manifest schema is missing or unsupported");
        return result;
    }
    result.manifest_sha256 = lowercase_ascii(
        hash::sha256_of_file(result.manifest_path.string()));
    if (result.manifest_sha256.empty()) {
        result.errors.push_back("battle source manifest SHA-256 could not be computed");
        return result;
    }

    auto& manifest = result.manifest;
    manifest.key = parse_string(*manifest_text, "key").value_or("");
    manifest.snapshot_file = parse_string(*manifest_text, "snapshotFile").value_or("");
    manifest.snapshot_sha256 = lowercase_ascii(
        parse_string(*manifest_text, "snapshotSha256").value_or(""));
    manifest.encounter_identity = parse_string(
        *manifest_text, "encounterIdentity").value_or("");
    manifest.stage_identity = parse_string(*manifest_text, "stageIdentity").value_or("");
    manifest.provenance = result.manifest_path.string();
    if (const auto validation = find_value_span(*manifest_text, "validation")) {
        manifest.roster_fingerprint = parse_string(
            *validation, "rosterFingerprint").value_or("");
        manifest.accepted_savestate_sha256 = parse_string_array(
            *validation, "acceptedSavestateSha256");
        if (const auto legacy_hash = parse_string(*validation, "savestateSha256");
            legacy_hash.has_value()) {
            manifest.accepted_savestate_sha256.push_back(*legacy_hash);
        }
        for (auto& hash : manifest.accepted_savestate_sha256) {
            hash = lowercase_ascii(std::move(hash));
        }
        std::sort(
            manifest.accepted_savestate_sha256.begin(),
            manifest.accepted_savestate_sha256.end());
        manifest.accepted_savestate_sha256.erase(
            std::unique(
                manifest.accepted_savestate_sha256.begin(),
                manifest.accepted_savestate_sha256.end()),
            manifest.accepted_savestate_sha256.end());
    }
    if (const auto sources = find_value_span(*manifest_text, "sources")) {
        for (const auto object : split_top_level_objects(*sources)) {
            manifest.sources.push_back(BattleSourceReference{
                .kind = parse_string(object, "kind").value_or(""),
                .identity = parse_string(object, "identity").value_or(""),
                .sha256 = lowercase_ascii(parse_string(object, "sha256").value_or("")),
            });
        }
    }
    if (manifest.key != manifest_key || manifest.snapshot_file.empty()
        || manifest.snapshot_sha256.empty()
        || manifest.roster_fingerprint.empty()
        || manifest.accepted_savestate_sha256.empty()) {
        result.errors.push_back("battle source manifest is incomplete or its key does not match");
        return result;
    }

    result.snapshot_path = result.manifest_path.parent_path() / manifest.snapshot_file;
    const auto actual_snapshot_hash = lowercase_ascii(
        hash::sha256_of_file(result.snapshot_path.string()));
    if (actual_snapshot_hash.empty() || actual_snapshot_hash != manifest.snapshot_sha256) {
        result.errors.push_back("battle source snapshot SHA-256 does not match the manifest");
        return result;
    }
    const auto snapshot_text = read_text_file(result.snapshot_path);
    if (!snapshot_text.has_value()
        || parse_string(*snapshot_text, "schema") != "savor_battle_source_snapshot_v1") {
        result.errors.push_back("battle source snapshot schema is missing or unsupported");
        return result;
    }

    auto& snapshot = result.snapshot;
    snapshot.manifest_key = parse_string(*snapshot_text, "manifestKey").value_or("");
    snapshot.provenance = result.snapshot_path.string();
    if (const auto encounter = find_value_span(*snapshot_text, "encounter")) {
        snapshot.encounter_id = parse_int(*encounter, "entryId").value_or(-1);
        snapshot.encounter_initiative = parse_int(*encounter, "initiative").value_or(0);
    }
    if (const auto stage = find_value_span(*snapshot_text, "stage")) {
        snapshot.stage_id = parse_string(*stage, "stageId").value_or("");
        snapshot.sst_record_index = parse_int(*stage, "sstRecordIndex").value_or(-1);
    }
    if (const auto placements = find_value_span(*snapshot_text, "placements")) {
        for (const auto object : split_top_level_objects(*placements)) {
            if (!parse_bool(object, "present").value_or(false)) {
                continue;
            }
            snapshot.placements.push_back(BattleSourcePlacement{
                .slot = parse_int(object, "slot").value_or(-1),
                .is_player = parse_string(object, "side") == "pc",
                .combatant_id = parse_int(object, "id").value_or(-1),
                .combatant_name = parse_string(object, "name").value_or(""),
                .grid_x = parse_int(object, "gridX").value_or(-1),
                .grid_z = parse_int(object, "gridZ").value_or(-1),
                .status = parse_status(object),
                .provenance = parse_string(object, "provenance").value_or(""),
            });
        }
    }
    if (const auto terrain = find_value_span(*snapshot_text, "terrain")) {
        const auto parsed = parse_terrain(*terrain);
        snapshot.terrain_status = parsed.has_value()
            ? parse_status(*terrain)
            : BattleSourceFieldStatus::Invalid;
        if (parsed.has_value()) {
            snapshot.terrain_source_9x9 = *parsed;
        }
        snapshot.terrain_provenance = parse_string(
            *terrain, "provenance").value_or("");
    }
    if (const auto rule = find_value_span(*snapshot_text, "resourceIdentityRule")) {
        snapshot.resource_identity_rule = parse_string(*rule, "id").value_or("");
        snapshot.resource_identity_status = parse_status(*rule);
    }
    if (snapshot.manifest_key != manifest.key || snapshot.encounter_id < 0
        || snapshot.stage_id.empty() || snapshot.placements.empty()
        || snapshot.terrain_status != BattleSourceFieldStatus::Exact
        || snapshot.resource_identity_rule != "combatant-side-and-id-v1") {
        result.errors.push_back("battle source snapshot is incomplete or invalid");
        return result;
    }

    result.ok = true;
    return result;
}

BattleSourceValidationResult validate_battle_source_bundle(
    const BattleSourceBundleLoadResult& bundle,
    const soa::battle::ctx::BattleContext& context,
    const BattleSourceValidationInput& validation) {
    BattleSourceValidationResult result;
    if (!bundle.ok) {
        result.errors = bundle.errors;
        return result;
    }
    result.observed_roster_fingerprint = context_roster_fingerprint(context);
    result.roster_matches = result.observed_roster_fingerprint
        == bundle.manifest.roster_fingerprint;
    if (!result.roster_matches) {
        result.errors.push_back(
            "captured roster fingerprint does not match the source manifest: observed="
            + result.observed_roster_fingerprint + "; expected="
            + bundle.manifest.roster_fingerprint);
    }

    if (validation.savestate_sha256.has_value()) {
        const auto observed_hash = lowercase_ascii(*validation.savestate_sha256);
        result.savestate_matches = std::find(
            bundle.manifest.accepted_savestate_sha256.begin(),
            bundle.manifest.accepted_savestate_sha256.end(),
            observed_hash) != bundle.manifest.accepted_savestate_sha256.end();
        if (!result.savestate_matches) {
            result.errors.push_back(
                "captured savestate SHA-256 is not an accepted source-manifest fingerprint");
        }
    } else if (validation.require_savestate_match) {
        result.errors.push_back("historical job source has no savestate SHA-256 for manifest validation");
    } else {
        result.savestate_matches = true;
    }
    result.ok = result.roster_matches && result.savestate_matches;
    return result;
}

std::optional<BattleSourcePlacement> battle_source_placement_for_slot(
    const BattleSourceSnapshot& snapshot,
    int slot) {
    const auto found = std::find_if(
        snapshot.placements.begin(), snapshot.placements.end(),
        [slot](const BattleSourcePlacement& item) { return item.slot == slot; });
    return found == snapshot.placements.end()
        ? std::nullopt
        : std::optional<BattleSourcePlacement>{*found};
}

BattleStdResourceIdentity derive_battle_std_resource_identity(
    int slot,
    bool is_player,
    int combatant_id) {
    if (slot < 0 || combatant_id < 0) {
        return {
            .status = BattleStdResourceIdentityStatus::MissingInput,
            .provenance = "slot and combatant ID are required",
        };
    }
    if (is_player) {
        if (slot >= 4) {
            return {
                .status = BattleStdResourceIdentityStatus::Unsupported,
                .provenance = "player ownership conflicts with the MA slot branch",
            };
        }
        return {
            .status = BattleStdResourceIdentityStatus::Exact,
            .stem = indexed_resource_stem("MA", combatant_id),
            .provenance = "LoadMovementStdResourceById player-slot MA%03d branch",
        };
    }
    if (slot < 4) {
        return {
            .status = BattleStdResourceIdentityStatus::Unsupported,
            .provenance = "enemy ownership conflicts with the MB/MG slot branch",
        };
    }
    if (combatant_id < 0x80) {
        return {
            .status = BattleStdResourceIdentityStatus::Exact,
            .stem = indexed_resource_stem("MB", combatant_id),
            .provenance = "LoadMovementStdResourceById enemy ID below 0x80 MB%03d branch",
        };
    }
    if (combatant_id <= 0x9d) {
        return {
            .status = BattleStdResourceIdentityStatus::Exact,
            .stem = indexed_resource_stem("MG", combatant_id - 0x80),
            .provenance = "LoadMovementStdResourceById MG branch; FUN_80008274 is identity through ID 0x9d",
        };
    }
    return {
        .status = BattleStdResourceIdentityStatus::MissingInput,
        .provenance = "MG IDs above 0x9d require the source-data remap table consumed by FUN_80008274",
    };
}

const char* battle_source_field_status_name(BattleSourceFieldStatus status) {
    switch (status) {
    case BattleSourceFieldStatus::Exact: return "Exact";
    case BattleSourceFieldStatus::MissingInput: return "MissingInput";
    case BattleSourceFieldStatus::Invalid: return "Invalid";
    }
    return "Invalid";
}

const char* battle_std_resource_identity_status_name(
    BattleStdResourceIdentityStatus status) {
    switch (status) {
    case BattleStdResourceIdentityStatus::Exact: return "Exact";
    case BattleStdResourceIdentityStatus::MissingInput: return "MissingInput";
    case BattleStdResourceIdentityStatus::Unsupported: return "Unsupported";
    }
    return "Unsupported";
}

} // namespace savor::predict
