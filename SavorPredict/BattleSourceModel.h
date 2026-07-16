#pragma once

#include <Core/Memory/Soa/Battle/BattleContext.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace savor::predict {

enum class BattleSourceFieldStatus {
    Exact,
    Provisional,
    MissingInput,
    Invalid,
};

enum class BattleEncounterSourceKind {
    Unknown,
    EventDefinition,
    EncounterDefinition,
    RandomTable,
};

struct BattleEncounterIdentity {
    BattleEncounterSourceKind source_kind = BattleEncounterSourceKind::Unknown;
    int encounter_id = -1;
};

enum class BattleSourceProducerKind {
    Unknown,
    ScriptedBattleRequest,
    RandomEncounterTable,
};

struct ScriptedBattleRequestIdentity {
    std::string script_identity;
    std::string section_identity;
    int instruction_payload_offset = -1;
};

struct ScriptedBattleRequest {
    ScriptedBattleRequestIdentity identity;
    int instruction_offset = -1;
    int event_mode = 0;
    int event_or_encounter_id = -1;
    int stage_id = -1;
    int transition_selector = -1;
    BattleSourceFieldStatus status = BattleSourceFieldStatus::MissingInput;
    std::string provenance;
};

struct BattleSourceSelection {
    BattleSourceProducerKind producer_kind = BattleSourceProducerKind::Unknown;
    ScriptedBattleRequestIdentity scripted_request;
};

struct BattleSourceReference {
    std::string kind;
    std::string identity;
    std::string sha256;
};

struct BattleSourceManifest {
    std::string key;
    std::string encounter_identity;
    std::string stage_identity;
    std::filesystem::path snapshot_file;
    std::string snapshot_sha256;
    std::string roster_fingerprint;
    std::vector<std::string> accepted_savestate_sha256;
    std::vector<BattleSourceReference> sources;
    BattleSourceProducerKind producer_kind = BattleSourceProducerKind::Unknown;
    std::optional<ScriptedBattleRequest> scripted_battle_request;
    std::string provenance;
};

struct BattleSourcePlacement {
    int slot = -1;
    bool is_player = false;
    int combatant_id = -1;
    std::string combatant_name;
    int grid_x = -1;
    int grid_z = -1;
    BattleSourceFieldStatus status = BattleSourceFieldStatus::MissingInput;
    std::string provenance;
};

struct BattleSourceSnapshot {
    std::string manifest_key;
    BattleEncounterSourceKind encounter_source_kind =
        BattleEncounterSourceKind::Unknown;
    int requested_encounter_id = -1;
    int encounter_id = -1;
    int encounter_initiative = 0;
    std::string stage_id;
    int sst_record_index = -1;
    std::array<std::uint8_t, 81> terrain_source_9x9{};
    BattleSourceFieldStatus terrain_status = BattleSourceFieldStatus::MissingInput;
    std::string terrain_provenance;
    std::vector<BattleSourcePlacement> placements;
    std::string resource_identity_rule;
    BattleSourceFieldStatus resource_identity_status =
        BattleSourceFieldStatus::MissingInput;
    std::string provenance;
};

struct BattleSourceBundleLoadResult {
    bool ok = false;
    BattleSourceManifest manifest;
    BattleSourceSnapshot snapshot;
    std::filesystem::path manifest_path;
    std::filesystem::path snapshot_path;
    std::string manifest_sha256;
    std::vector<std::string> errors;
};

enum class BattleSourceResolutionStatus {
    Exact,
    Provisional,
    MissingInput,
    Unsupported,
    Ambiguous,
    Invalid,
};

struct BattleSourceBundleResolution {
    BattleSourceResolutionStatus status =
        BattleSourceResolutionStatus::MissingInput;
    BattleSourceSelection selection;
    BattleSourceBundleLoadResult bundle;
    std::vector<std::string> candidate_manifest_keys;
    std::string provenance;
    std::vector<std::string> errors;
};

struct BattleSourceValidationInput {
    std::optional<BattleSourceSelection> source_selection;
    std::optional<BattleEncounterIdentity> expected_encounter;
    std::optional<std::string> savestate_sha256;
};

struct BattleSourceValidationResult {
    bool ok = false;
    bool roster_matches = false;
    bool source_selection_matches = false;
    bool expected_encounter_provided = false;
    bool encounter_matches = false;
    bool savestate_fingerprint_provided = false;
    bool savestate_fingerprint_recognized = false;
    std::string observed_roster_fingerprint;
    std::vector<std::string> diagnostics;
    std::vector<std::string> errors;
};

enum class BattleStdResourceIdentityStatus {
    Exact,
    MissingInput,
    Unsupported,
};

struct BattleStdResourceIdentity {
    BattleStdResourceIdentityStatus status =
        BattleStdResourceIdentityStatus::MissingInput;
    std::string stem;
    std::string provenance;
};

std::filesystem::path default_battle_source_manifest_root();

BattleSourceBundleLoadResult load_battle_source_bundle(
    std::string_view manifest_key,
    const std::filesystem::path& manifest_root = {});

BattleSourceBundleResolution resolve_battle_source_bundle(
    const BattleSourceSelection& selection,
    const std::filesystem::path& manifest_root = {});

std::vector<BattleSourceBundleLoadResult> load_battle_source_bundles_for_encounter(
    const BattleEncounterIdentity& encounter,
    const std::filesystem::path& manifest_root = {});

BattleSourceValidationResult validate_battle_source_bundle(
    const BattleSourceBundleLoadResult& bundle,
    const soa::battle::ctx::BattleContext& context,
    const BattleSourceValidationInput& validation = {});

std::optional<BattleSourcePlacement> battle_source_placement_for_slot(
    const BattleSourceSnapshot& snapshot,
    int slot);

BattleStdResourceIdentity derive_battle_std_resource_identity(
    int slot,
    bool is_player,
    int combatant_id);

const char* battle_encounter_source_kind_name(BattleEncounterSourceKind kind);
const char* battle_source_producer_kind_name(BattleSourceProducerKind kind);
const char* battle_source_resolution_status_name(BattleSourceResolutionStatus status);
const char* battle_source_field_status_name(BattleSourceFieldStatus status);
const char* battle_std_resource_identity_status_name(
    BattleStdResourceIdentityStatus status);

} // namespace savor::predict
