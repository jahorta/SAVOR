#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

#include "DbConfigPaths.h"

namespace savor::db::core {
class DBService;
}

namespace savor::db::bootstrap {

enum class DatabaseBootstrapProfileId : std::uint8_t {
    Empty = 0,
    Standard = 1,
};

struct DatabaseBootstrapProfileInfo {
    DatabaseBootstrapProfileId id = DatabaseBootstrapProfileId::Empty;
    std::string key;
    std::string display_name;
    std::string description;
};

struct DatabaseBootstrapRecordCounts {
    int seed_probe_specs = 0;
    int predicate_definitions = 0;
    int predicate_bindings = 0;
    int predicate_groups = 0;
    int battle_action_presets = 0;
    int battle_plans = 0;
    int workflow_graphs = 0;

    [[nodiscard]] int Total() const noexcept;
};

struct DatabaseRootBootstrapRequest {
    std::filesystem::path target_root;
    DatabaseBootstrapProfileId profile_id = DatabaseBootstrapProfileId::Empty;
};

struct DatabaseRootBootstrapResult {
    bool success = false;
    DatabaseBootstrapProfileId profile_id = DatabaseBootstrapProfileId::Empty;
    DatabaseBootstrapRecordCounts created;
    std::string diagnostic;
};

[[nodiscard]] std::span<const DatabaseBootstrapProfileInfo>
DatabaseBootstrapProfiles();

[[nodiscard]] const DatabaseBootstrapProfileInfo*
FindDatabaseBootstrapProfile(DatabaseBootstrapProfileId id);

[[nodiscard]] DbConfigPaths MakeDatabaseRootConfigPaths(
    const std::filesystem::path& root);

class DatabaseRootBootstrapService final {
public:
    bool RecreateRoot(
        core::DBService& active_service,
        const DatabaseRootBootstrapRequest& request,
        DatabaseRootBootstrapResult* result_out = nullptr,
        std::string* error_out = nullptr) const;
};

} // namespace savor::db::bootstrap
