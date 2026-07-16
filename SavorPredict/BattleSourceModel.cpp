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
    if (value == "Provisional") {
        return BattleSourceFieldStatus::Provisional;
    }
    if (value == "Invalid") {
        return BattleSourceFieldStatus::Invalid;
    }
    return BattleSourceFieldStatus::MissingInput;
}

BattleEncounterSourceKind parse_encounter_source_kind(std::string_view value) {
    if (value == "alx_enemy_event" || value == "event_definition") {
        return BattleEncounterSourceKind::EventDefinition;
    }
    if (value == "enemy_encounter" || value == "encounter_definition") {
        return BattleEncounterSourceKind::EncounterDefinition;
    }
    if (value == "random_table") {
        return BattleEncounterSourceKind::RandomTable;
    }
    return BattleEncounterSourceKind::Unknown;
}

BattleSourceProducerKind parse_source_producer_kind(std::string_view value) {
    if (value == "scpt_opcode_112" || value == "scripted_battle_request") {
        return BattleSourceProducerKind::ScriptedBattleRequest;
    }
    if (value == "random_encounter_table") {
        return BattleSourceProducerKind::RandomEncounterTable;
    }
    return BattleSourceProducerKind::Unknown;
}

bool scripted_request_identity_complete(
    const ScriptedBattleRequestIdentity& identity) {
    return !identity.script_identity.empty()
        && !identity.section_identity.empty()
        && identity.instruction_payload_offset >= 0;
}

bool scripted_request_identity_matches(
    const ScriptedBattleRequestIdentity& lhs,
    const ScriptedBattleRequestIdentity& rhs) {
    return lhs.script_identity == rhs.script_identity
        && lhs.section_identity == rhs.section_identity
        && lhs.instruction_payload_offset == rhs.instruction_payload_offset;
}

int signed_low_16(int value) {
    const auto low = static_cast<unsigned>(value) & 0xffffU;
    return low < 0x8000U
        ? static_cast<int>(low)
        : static_cast<int>(low) - 0x10000;
}

std::optional<int> parse_stage_identity(std::string_view value) {
    if (value.size() < 2 || value.front() != 's') {
        return std::nullopt;
    }
    int parsed = 0;
    const auto [end, error] = std::from_chars(
        value.data() + 1, value.data() + value.size(), parsed);
    if (error != std::errc() || end != value.data() + value.size()) {
        return std::nullopt;
    }
    return parsed;
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

std::vector<std::string> list_manifest_keys(const std::filesystem::path& root) {
    std::vector<std::string> keys;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error || !entry.is_directory()) {
            continue;
        }
        const auto candidate = entry.path() / "manifest.json";
        const auto text = read_text_file(candidate);
        if (!text.has_value()) {
            continue;
        }
        const auto key = parse_string(*text, "key");
        if (key.has_value() && !key->empty()) {
            keys.push_back(*key);
        }
    }
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    return keys;
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
        || parse_string(*manifest_text, "schema") != "savor_battle_source_manifest_v2") {
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
    bool scripted_request_fields_complete = false;
    if (const auto request = find_value_span(*manifest_text, "scriptedBattleRequest")) {
        const auto instruction_offset = parse_int(*request, "instructionOffset");
        const auto payload_offset = parse_int(*request, "instructionPayloadOffset");
        const auto event_mode = parse_int(*request, "eventMode");
        const auto event_or_encounter_id = parse_int(*request, "eventOrEncounterId");
        const auto stage_id = parse_int(*request, "stageId");
        const auto transition_selector = parse_int(*request, "transitionSelector");
        scripted_request_fields_complete = instruction_offset.has_value()
            && payload_offset.has_value()
            && event_mode.has_value()
            && event_or_encounter_id.has_value()
            && stage_id.has_value()
            && transition_selector.has_value();
        manifest.producer_kind = parse_source_producer_kind(
            parse_string(*request, "kind").value_or(""));
        manifest.scripted_battle_request = ScriptedBattleRequest{
            .identity = ScriptedBattleRequestIdentity{
                .script_identity = parse_string(
                    *request, "scriptIdentity").value_or(""),
                .section_identity = parse_string(
                    *request, "sectionIdentity").value_or(""),
                .instruction_payload_offset = payload_offset.value_or(-1),
            },
            .instruction_offset = instruction_offset.value_or(-1),
            .event_mode = event_mode.value_or(0),
            .event_or_encounter_id = event_or_encounter_id.value_or(-1),
            .stage_id = stage_id.value_or(-1),
            .transition_selector = transition_selector.value_or(-1),
            .status = parse_status(*request),
            .provenance = parse_string(*request, "provenance").value_or(""),
        };
    }
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
        || !scripted_request_fields_complete
        || manifest.producer_kind != BattleSourceProducerKind::ScriptedBattleRequest
        || !manifest.scripted_battle_request.has_value()
        || !scripted_request_identity_complete(
            manifest.scripted_battle_request->identity)
        || manifest.scripted_battle_request->instruction_offset < 0
        || manifest.scripted_battle_request->status == BattleSourceFieldStatus::MissingInput
        || manifest.scripted_battle_request->status == BattleSourceFieldStatus::Invalid) {
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
        snapshot.encounter_source_kind = parse_encounter_source_kind(
            parse_string(*encounter, "sourceKind").value_or(""));
        snapshot.requested_encounter_id = parse_int(
            *encounter, "requestedEntryId").value_or(-1);
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
    if (snapshot.manifest_key != manifest.key
        || snapshot.encounter_source_kind == BattleEncounterSourceKind::Unknown
        || snapshot.requested_encounter_id < 0
        || snapshot.encounter_id < 0
        || snapshot.stage_id.empty() || snapshot.placements.empty()
        || snapshot.terrain_status != BattleSourceFieldStatus::Exact
        || snapshot.resource_identity_rule != "combatant-side-and-id-v1") {
        result.errors.push_back("battle source snapshot is incomplete or invalid");
        return result;
    }

    const auto& request = *manifest.scripted_battle_request;
    const auto expected_encounter_source = request.event_mode != 0
        ? BattleEncounterSourceKind::EventDefinition
        : BattleEncounterSourceKind::EncounterDefinition;
    const auto snapshot_stage_id = parse_stage_identity(snapshot.stage_id);
    if (snapshot.encounter_source_kind != expected_encounter_source
        || snapshot.requested_encounter_id
            != signed_low_16(request.event_or_encounter_id)
        || !snapshot_stage_id.has_value()
        || *snapshot_stage_id != signed_low_16(request.stage_id)) {
        result.errors.push_back(
            "scripted battle request operands do not match the encounter and stage snapshot");
        return result;
    }

    result.ok = true;
    return result;
}

BattleSourceBundleResolution resolve_battle_source_bundle(
    const BattleSourceSelection& selection,
    const std::filesystem::path& manifest_root) {
    BattleSourceBundleResolution result;
    result.selection = selection;
    if (selection.producer_kind == BattleSourceProducerKind::Unknown) {
        result.status = BattleSourceResolutionStatus::MissingInput;
        result.errors.push_back(
            "battle source resolution requires a typed source producer");
        return result;
    }
    if (selection.producer_kind == BattleSourceProducerKind::RandomEncounterTable) {
        result.status = BattleSourceResolutionStatus::Unsupported;
        result.errors.push_back(
            "battle source catalog does not yet resolve random-table encounters");
        return result;
    }
    if (selection.producer_kind != BattleSourceProducerKind::ScriptedBattleRequest
        || !scripted_request_identity_complete(selection.scripted_request)) {
        result.status = BattleSourceResolutionStatus::MissingInput;
        result.errors.push_back(
            "scripted battle source resolution requires script, section, and payload offset");
        return result;
    }

    const auto root = manifest_root.empty()
        ? default_battle_source_manifest_root()
        : manifest_root;
    std::error_code root_error;
    if (!std::filesystem::is_directory(root, root_error) || root_error) {
        result.status = BattleSourceResolutionStatus::MissingInput;
        result.errors.push_back("battle source manifest root is unavailable: " + root.string());
        return result;
    }

    std::vector<BattleSourceBundleLoadResult> matches;
    for (const auto& key : list_manifest_keys(root)) {
        auto bundle = load_battle_source_bundle(key, root);
        if (!bundle.ok) {
            continue;
        }
        if (bundle.manifest.producer_kind == selection.producer_kind
            && bundle.manifest.scripted_battle_request.has_value()
            && scripted_request_identity_matches(
                bundle.manifest.scripted_battle_request->identity,
                selection.scripted_request)) {
            result.candidate_manifest_keys.push_back(bundle.manifest.key);
            matches.push_back(std::move(bundle));
        }
    }

    if (matches.empty()) {
        result.status = BattleSourceResolutionStatus::MissingInput;
        result.errors.push_back(
            "no battle source manifest matches scripted request "
            + selection.scripted_request.script_identity + "/"
            + selection.scripted_request.section_identity + " at payload offset "
            + std::to_string(selection.scripted_request.instruction_payload_offset));
        return result;
    }
    if (matches.size() != 1) {
        result.status = BattleSourceResolutionStatus::Ambiguous;
        result.errors.push_back(
            "multiple battle source manifests claim the same scripted battle request identity");
        return result;
    }

    result.bundle = std::move(matches.front());
    const auto& request = *result.bundle.manifest.scripted_battle_request;
    result.provenance = request.provenance;
    switch (request.status) {
    case BattleSourceFieldStatus::Exact:
        result.status = BattleSourceResolutionStatus::Exact;
        break;
    case BattleSourceFieldStatus::Provisional:
        result.status = BattleSourceResolutionStatus::Provisional;
        break;
    case BattleSourceFieldStatus::MissingInput:
        result.status = BattleSourceResolutionStatus::MissingInput;
        result.errors.push_back(
            "matching source manifest does not classify its scripted battle request");
        break;
    case BattleSourceFieldStatus::Invalid:
        result.status = BattleSourceResolutionStatus::Invalid;
        result.errors.push_back(
            "matching source manifest has an invalid scripted battle request");
        break;
    }
    return result;
}

std::vector<BattleSourceBundleLoadResult> load_battle_source_bundles_for_encounter(
    const BattleEncounterIdentity& encounter,
    const std::filesystem::path& manifest_root) {
    std::vector<BattleSourceBundleLoadResult> matches;
    if (encounter.source_kind == BattleEncounterSourceKind::Unknown
        || encounter.encounter_id < 0) {
        return matches;
    }
    const auto root = manifest_root.empty()
        ? default_battle_source_manifest_root()
        : manifest_root;
    std::error_code root_error;
    if (!std::filesystem::is_directory(root, root_error) || root_error) {
        return matches;
    }
    for (const auto& key : list_manifest_keys(root)) {
        auto bundle = load_battle_source_bundle(key, root);
        if (bundle.ok
            && bundle.snapshot.encounter_source_kind == encounter.source_kind
            && bundle.snapshot.encounter_id == encounter.encounter_id) {
            matches.push_back(std::move(bundle));
        }
    }
    return matches;
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

    if (validation.source_selection.has_value()
        && bundle.manifest.scripted_battle_request.has_value()) {
        result.source_selection_matches =
            validation.source_selection->producer_kind
                == bundle.manifest.producer_kind
            && scripted_request_identity_matches(
                validation.source_selection->scripted_request,
                bundle.manifest.scripted_battle_request->identity);
        if (!result.source_selection_matches) {
            result.errors.push_back(
                "selected source producer does not match the resolved source manifest");
        }
    } else {
        result.errors.push_back(
            "source validation requires the producer selection used for resolution");
    }

    result.encounter_matches = true;
    if (validation.expected_encounter.has_value()) {
        result.expected_encounter_provided = true;
        result.encounter_matches =
            validation.expected_encounter->source_kind
                == bundle.snapshot.encounter_source_kind
            && validation.expected_encounter->encounter_id
                == bundle.snapshot.encounter_id;
        if (!result.encounter_matches) {
            result.errors.push_back(
                "expected encounter does not match the scripted battle request output");
        }
    }

    if (validation.savestate_sha256.has_value()) {
        result.savestate_fingerprint_provided = true;
        const auto observed_hash = lowercase_ascii(*validation.savestate_sha256);
        result.savestate_fingerprint_recognized = std::find(
            bundle.manifest.accepted_savestate_sha256.begin(),
            bundle.manifest.accepted_savestate_sha256.end(),
            observed_hash) != bundle.manifest.accepted_savestate_sha256.end();
        if (!result.savestate_fingerprint_recognized) {
            result.diagnostics.push_back(
                "captured savestate SHA-256 is not listed as known provenance for this source manifest");
        }
    }
    result.ok = result.roster_matches
        && result.source_selection_matches
        && result.encounter_matches;
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

const char* battle_encounter_source_kind_name(BattleEncounterSourceKind kind) {
    switch (kind) {
    case BattleEncounterSourceKind::Unknown: return "Unknown";
    case BattleEncounterSourceKind::EventDefinition: return "EventDefinition";
    case BattleEncounterSourceKind::EncounterDefinition: return "EncounterDefinition";
    case BattleEncounterSourceKind::RandomTable: return "RandomTable";
    }
    return "Unknown";
}

const char* battle_source_producer_kind_name(BattleSourceProducerKind kind) {
    switch (kind) {
    case BattleSourceProducerKind::Unknown: return "Unknown";
    case BattleSourceProducerKind::ScriptedBattleRequest:
        return "ScriptedBattleRequest";
    case BattleSourceProducerKind::RandomEncounterTable:
        return "RandomEncounterTable";
    }
    return "Unknown";
}

const char* battle_source_resolution_status_name(BattleSourceResolutionStatus status) {
    switch (status) {
    case BattleSourceResolutionStatus::Exact: return "Exact";
    case BattleSourceResolutionStatus::Provisional: return "Provisional";
    case BattleSourceResolutionStatus::MissingInput: return "MissingInput";
    case BattleSourceResolutionStatus::Unsupported: return "Unsupported";
    case BattleSourceResolutionStatus::Ambiguous: return "Ambiguous";
    case BattleSourceResolutionStatus::Invalid: return "Invalid";
    }
    return "Invalid";
}

const char* battle_source_field_status_name(BattleSourceFieldStatus status) {
    switch (status) {
    case BattleSourceFieldStatus::Exact: return "Exact";
    case BattleSourceFieldStatus::Provisional: return "Provisional";
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
