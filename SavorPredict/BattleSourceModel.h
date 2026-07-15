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
    MissingInput,
    Invalid,
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

struct BattleSourceValidationInput {
    std::optional<std::string> savestate_sha256;
    bool require_savestate_match = false;
};

struct BattleSourceValidationResult {
    bool ok = false;
    bool roster_matches = false;
    bool savestate_matches = false;
    std::string observed_roster_fingerprint;
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

const char* battle_source_field_status_name(BattleSourceFieldStatus status);
const char* battle_std_resource_identity_status_name(
    BattleStdResourceIdentityStatus status);

} // namespace savor::predict
