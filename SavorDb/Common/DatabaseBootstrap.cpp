#include "DatabaseBootstrap.h"

#include "DatabaseBootstrapProfiles.h"
#include "DbService.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <ranges>
#include <string_view>
#include <system_error>
#include <utility>

namespace savor::db::bootstrap {
namespace {

const std::array<DatabaseBootstrapProfileInfo, 2> kProfiles{{
    {DatabaseBootstrapProfileId::Empty, "empty", "Empty",
     "Create only the migrated database root and storage directories."},
    {DatabaseBootstrapProfileId::Standard, "standard", "Standard",
     "Create the common first-battle authoring presets and workflow graphs."},
}};

void SetFailure(
    DatabaseRootBootstrapResult* result_out,
    std::string* error_out,
    DatabaseBootstrapProfileId profile_id,
    std::string message)
{
    if (result_out != nullptr) {
        result_out->success = false;
        result_out->profile_id = profile_id;
        result_out->diagnostic = message;
    }
    if (error_out != nullptr) *error_out = std::move(message);
}

std::filesystem::path UniqueSibling(
    const std::filesystem::path& target,
    std::string_view role)
{
    static std::atomic<std::uint64_t> sequence{0};
    const auto ticks = std::chrono::system_clock::now().time_since_epoch().count();
    return target.parent_path()
        / (target.filename().string() + "." + std::string(role) + "-"
            + std::to_string(ticks) + "-"
            + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)));
}

bool EnsureRootLayout(const std::filesystem::path& root, std::string* error_out)
{
    std::error_code ec;
    std::filesystem::create_directories(root / "object_store", ec);
    if (!ec) std::filesystem::create_directories(root / "archive_store", ec);
    if (!ec) return true;
    if (error_out != nullptr) {
        *error_out = "failed creating bootstrap root layout: " + ec.message();
    }
    return false;
}

void RemoveTree(const std::filesystem::path& path) noexcept
{
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

} // namespace

int DatabaseBootstrapRecordCounts::Total() const noexcept
{
    return seed_probe_specs + predicate_definitions + predicate_bindings
        + predicate_groups + battle_action_presets + battle_plans
        + workflow_graphs;
}

std::span<const DatabaseBootstrapProfileInfo> DatabaseBootstrapProfiles()
{
    return kProfiles;
}

const DatabaseBootstrapProfileInfo* FindDatabaseBootstrapProfile(
    const DatabaseBootstrapProfileId id)
{
    const auto found = std::ranges::find(
        kProfiles, id, &DatabaseBootstrapProfileInfo::id);
    return found == kProfiles.end() ? nullptr : &*found;
}

DbConfigPaths MakeDatabaseRootConfigPaths(const std::filesystem::path& root)
{
    return {
        .execution_db_path = root / "execution.db",
        .state_db_path = root / "state.db",
        .analysis_db_path = root / "analysis.db",
        .authoring_db_path = root / "authoring.db",
        .ui_read_db_path = root / "ui_read.db",
        .archive_db_path = root / "archive.db",
        .object_store_root = root / "object_store",
        .archive_store_root = root / "archive_store",
    };
}

bool DatabaseRootBootstrapService::RecreateRoot(
    core::DBService& active_service,
    const DatabaseRootBootstrapRequest& request,
    DatabaseRootBootstrapResult* result_out,
    std::string* error_out) const
{
    if (result_out != nullptr) {
        *result_out = {};
        result_out->profile_id = request.profile_id;
    }
    if (request.target_root.empty()) {
        SetFailure(result_out, error_out, request.profile_id,
            "database bootstrap target root is empty");
        return false;
    }
    if (!active_service.IsRunning()) {
        SetFailure(result_out, error_out, request.profile_id,
            "active database service must be running before recreation");
        return false;
    }

    std::error_code ec;
    const auto target = std::filesystem::absolute(request.target_root, ec);
    if (ec || !std::filesystem::exists(target)) {
        SetFailure(result_out, error_out, request.profile_id,
            "active database root does not exist");
        return false;
    }
    const auto staging = UniqueSibling(target, "bootstrap");
    const auto backup = UniqueSibling(target, "backup");
    std::string error;
    if (!EnsureRootLayout(staging, &error)) {
        RemoveTree(staging);
        SetFailure(result_out, error_out, request.profile_id, std::move(error));
        return false;
    }

    core::DBService staging_service(MakeDatabaseRootConfigPaths(staging));
    if (!staging_service.Start(&error)) {
        RemoveTree(staging);
        SetFailure(result_out, error_out, request.profile_id,
            "failed migrating bootstrap root: " + error);
        return false;
    }
    DatabaseBootstrapRecordCounts counts{};
    if (!ApplyDatabaseBootstrapProfile(
            staging_service, request.profile_id, &counts, &error)) {
        staging_service.Stop();
        RemoveTree(staging);
        SetFailure(result_out, error_out, request.profile_id,
            "failed applying database bootstrap profile: " + error);
        return false;
    }
    staging_service.Stop();

    active_service.Stop();
    std::filesystem::rename(target, backup, ec);
    if (ec) {
        std::string restart_error;
        active_service.Start(&restart_error);
        RemoveTree(staging);
        std::string message = "failed preserving active database root: " + ec.message();
        if (!restart_error.empty()) message += "; failed restarting active root: " + restart_error;
        SetFailure(result_out, error_out, request.profile_id, std::move(message));
        return false;
    }

    ec.clear();
    std::filesystem::rename(staging, target, ec);
    if (ec) {
        std::error_code restore_ec;
        std::filesystem::rename(backup, target, restore_ec);
        std::string restart_error;
        active_service.Start(&restart_error);
        RemoveTree(staging);
        std::string message = "failed publishing bootstrap root: " + ec.message();
        if (restore_ec) message += "; failed restoring prior root: " + restore_ec.message();
        if (!restart_error.empty()) message += "; failed restarting prior root: " + restart_error;
        SetFailure(result_out, error_out, request.profile_id, std::move(message));
        return false;
    }

    std::string start_error;
    if (!active_service.Start(&start_error)) {
        active_service.Stop();
        std::error_code remove_ec;
        std::filesystem::remove_all(target, remove_ec);
        std::error_code restore_ec;
        if (!remove_ec) std::filesystem::rename(backup, target, restore_ec);
        std::string restart_error;
        const bool restarted = !remove_ec && !restore_ec
            && active_service.Start(&restart_error);
        std::string message = "replacement database root failed to start: " + start_error;
        if (remove_ec) message += "; failed removing replacement root: " + remove_ec.message();
        if (restore_ec) message += "; failed restoring prior root: " + restore_ec.message();
        if (!restarted) message += "; failed restarting prior root: " + restart_error;
        SetFailure(result_out, error_out, request.profile_id, std::move(message));
        return false;
    }

    std::filesystem::remove_all(backup, ec);
    if (result_out != nullptr) {
        result_out->success = true;
        result_out->profile_id = request.profile_id;
        result_out->created = counts;
        if (ec) {
            result_out->diagnostic =
                "database recreated, but backup cleanup failed: " + ec.message();
        }
    }
    return true;
}

} // namespace savor::db::bootstrap
