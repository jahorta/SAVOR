#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <future>

namespace simcore::db {

    struct TriggerRow {
        int64_t trigger_id{};
        std::string scope;     // "job" | "job_set"
        int64_t scope_id{};
        int action_kind{};     // PK_* (INTEGER FK to program_kinds.kind_id)
        std::string condition; // INI
        std::string action_args; // INI
        int active{};          // 1/0
    };

    class TriggersRepo {
    public:
        static std::future<DbResult<std::vector<TriggerRow>>> ListActiveByJobAsync(int64_t job_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<TriggerRow>>> ListActiveByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});

        static std::future<DbResult<bool>> TryDeactivateAsync(int64_t trigger_id, RetryPolicy rp = {});

        static inline DbResult<std::vector<TriggerRow>> ListActiveByJob(int64_t job_id) {
            return ListActiveByJobAsync(job_id).get();
        }
        static inline DbResult<std::vector<TriggerRow>> ListActiveByJobSet(int64_t job_set_id) {
            return ListActiveByJobSetAsync(job_set_id).get();
        }
        static inline DbResult<bool> TryDeactivate(int64_t trigger_id) {
            return TryDeactivateAsync(trigger_id).get();
        }

        static std::future<DbResult<int64_t>> AddAsync(
            const std::string& scope,            // "job" | "job_set"
            int64_t scope_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1,
            RetryPolicy rp = {}
        );

        static std::future<DbResult<int64_t>> AddForJobAsync(
            int64_t job_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1,
            RetryPolicy rp = {}
        );

        static std::future<DbResult<int64_t>> AddForJobSetAsync(
            int64_t job_set_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1,
            RetryPolicy rp = {}
        );

        // Synchronous convenience wrappers
        static inline DbResult<int64_t> Add(
            const std::string& scope,
            int64_t scope_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1
        ) {
            return AddAsync(scope, scope_id, action_kind, std::move(condition_ini), std::move(action_args_ini), active).get();
        }

        static inline DbResult<int64_t> AddForJob(
            int64_t job_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1
        ) {
            return AddForJobAsync(job_id, action_kind, std::move(condition_ini), std::move(action_args_ini), active).get();
        }

        static inline DbResult<int64_t> AddForJobSet(
            int64_t job_set_id,
            int action_kind,
            std::optional<std::string> condition_ini = std::nullopt,
            std::optional<std::string> action_args_ini = std::nullopt,
            int active = 1
        ) {
            return AddForJobSetAsync(job_set_id, action_kind, std::move(condition_ini), std::move(action_args_ini), active).get();
        }
    };

} // namespace simcore::db
