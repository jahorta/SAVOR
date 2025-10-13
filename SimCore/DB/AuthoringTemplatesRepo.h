#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>
#include <future>

namespace simcore::db {

    struct AuthoringTemplateRow {
        int64_t     id{};
        std::string name;
        std::string description;
        std::optional<int64_t> seed_probe_id;
        std::string ui_config_ini;
        std::string predicate_specs_ini;
        int32_t     fake_attack_budget{};
        std::optional<int64_t> last_materialized_settings_id;
        std::optional<int32_t> last_codec_version_seen;
        int64_t     created_at{};
        int64_t     updated_at{};
    };

    struct AuthoringTemplateLite { int64_t id{}; std::string name; };

    struct AuthoringTemplatesRepo {
        // Async
        static std::future<DbResult<int64_t>> InsertAsync(const AuthoringTemplateRow& r, RetryPolicy rp = {});
        static std::future<DbResult<void>>    UpdateAsync(const AuthoringTemplateRow& r, RetryPolicy rp = {});
        static std::future<DbResult<void>>    DeleteAsync(int64_t id, RetryPolicy rp = {});
        static std::future<DbResult<AuthoringTemplateRow>> GetAsync(int64_t id, RetryPolicy rp = {});
        static std::future<DbResult<std::optional<AuthoringTemplateRow>>> FindByNameAsync(const std::string& name, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<AuthoringTemplateLite>>> ListLiteAsync(const std::string& search, int32_t limit, RetryPolicy rp = {});

        // Blocking
        static inline DbResult<int64_t> Insert(const AuthoringTemplateRow& r) { return InsertAsync(r).get(); }
        static inline DbResult<void>    Update(const AuthoringTemplateRow& r) { return UpdateAsync(r).get(); }
        static inline DbResult<void>    Delete(int64_t id) { return DeleteAsync(id).get(); }
        static inline DbResult<AuthoringTemplateRow> Get(int64_t id) { return GetAsync(id).get(); }
        static inline DbResult<std::optional<AuthoringTemplateRow>> FindByName(const std::string& n) { return FindByNameAsync(n).get(); }
        static inline DbResult<std::vector<AuthoringTemplateLite>> ListLite(const std::string& s, int32_t lim) { return ListLiteAsync(s, lim).get(); }
    };

} // namespace simcore::db
