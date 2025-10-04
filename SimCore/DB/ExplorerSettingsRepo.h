#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <optional>
#include <cstdint>

namespace simcore {
    namespace db {

        struct ExplorerSettingsRow {
            int64_t id{};
            std::string name;
            std::string description;
            std::string fingerprint;
        };

        struct ExplorerSettingsRepo {
            // Async
            static std::future<DbResult<std::optional<int64_t>>> FindByFingerprintAsync(const std::string& fingerprint, RetryPolicy rp = {});
            static std::future<DbResult<int64_t>> EnsureByFingerprintAsync(const std::string& name, const std::string& description, const std::string& fingerprint, RetryPolicy rp = {});
            static std::future<DbResult<ExplorerSettingsRow>> GetAsync(int64_t id, RetryPolicy rp = {});
            static std::future<DbResult<void>> SetSeedProbeIdAsync(int64_t settings_id, int64_t seed_probe_id, RetryPolicy rp = {});

            // Blocking conveniences
            static inline DbResult<std::optional<int64_t>> FindByFingerprint(const std::string& fp) { return FindByFingerprintAsync(fp).get(); }
            static inline DbResult<int64_t> EnsureByFingerprint(const std::string& name, const std::string& desc, const std::string& fp) { return EnsureByFingerprintAsync(name, desc, fp).get(); }
            static inline DbResult<ExplorerSettingsRow> Get(int64_t id) { return GetAsync(id).get(); }
            static inline DbResult<void> SetSeedProbeId(int64_t settings_id, int64_t seed_probe_id) { return SetSeedProbeIdAsync(settings_id, seed_probe_id).get(); }
        };

    } // namespace db
} // namespace simcore
