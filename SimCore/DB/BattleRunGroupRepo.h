#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <vector>
#include <cstdint>
#include <optional>

namespace simcore {
    namespace db {

        struct BattleRunGroupRow {
            int64_t group_id{};
            int64_t settings_id{};
            int64_t seed_probe_id{};
            std::string name;
            std::string description;
            std::optional<std::string> predicate_vector_fingerprint;
            std::optional<std::string> plan_vector_fingerprint;
            int64_t created_at{};
        };

        struct BattleRunGroupRepo {
            // Async
            static std::future<DbResult<int64_t>> CreateAsync(
                int64_t settings_id, int64_t seed_probe_id,
                std::string name, std::string description,
                std::optional<std::string> predicate_vec_fp = std::nullopt,
                std::optional<std::string> plan_vec_fp = std::nullopt,
                RetryPolicy rp = {});

            static std::future<DbResult<BattleRunGroupRow>> GetAsync(int64_t group_id, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<int64_t>>> ListRunIdsAsync(int64_t group_id, RetryPolicy rp = {});
            static std::future<DbResult<bool>> IsCompleteAsync(int64_t group_id, RetryPolicy rp = {});

            // Blocking convenience
            static inline DbResult<int64_t> Create(
                int64_t settings_id, int64_t seed_probe_id,
                std::string name, std::string description,
                std::optional<std::string> predicate_vec_fp = std::nullopt,
                std::optional<std::string> plan_vec_fp = std::nullopt) {
                return CreateAsync(settings_id, seed_probe_id, std::move(name), std::move(description),
                    std::move(predicate_vec_fp), std::move(plan_vec_fp)).get();
            }

            static inline DbResult<BattleRunGroupRow> Get(int64_t group_id) { return GetAsync(group_id).get(); }
            static inline DbResult<std::vector<int64_t>> ListRunIds(int64_t group_id) { return ListRunIdsAsync(group_id).get(); }
            static inline DbResult<bool> IsComplete(int64_t group_id) { return IsCompleteAsync(group_id).get(); }
        };

    }
} // namespace
