// SimCore/DB/SeedProbeWinnersRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include <future>
#include <string>
#include <cstdint>
#include <optional>

namespace simcore {
    namespace db {

        struct SeedProbeWinnerRow {
            int64_t job_set_id{};
            int32_t result_delta{};
            int64_t winner_job_id{};
            std::optional<int32_t> expected_delta{};
            std::optional<std::string> expected_tag{};
            std::optional<std::string> stage{};
            std::optional<std::string> frame_hex{};
            std::optional<std::string> metrics_json{};
        };

        struct InsertWinnerResult {
            bool inserted{};
            std::optional<int64_t> existing_winner_job_id{};
        };

        struct SeedProbeWinnersRepo {
            static std::future<DbResult<InsertWinnerResult>> TryInsertWinnerAsync(
                int64_t job_set_id,
                int32_t result_delta,
                int64_t winner_job_id,
                std::optional<int32_t> expected_delta = std::nullopt,
                std::optional<std::string> expected_tag = std::nullopt,
                std::optional<std::string> stage = std::nullopt,
                std::optional<std::string> frame_hex = std::nullopt,
                std::optional<std::string> metrics_json = std::nullopt,
                RetryPolicy rp = {}
            );

            static inline DbResult<InsertWinnerResult> TryInsertWinner(
                int64_t job_set_id,
                int32_t result_delta,
                int64_t winner_job_id,
                std::optional<int32_t> expected_delta = std::nullopt,
                std::optional<std::string> expected_tag = std::nullopt,
                std::optional<std::string> stage = std::nullopt,
                std::optional<std::string> frame_hex = std::nullopt,
                std::optional<std::string> metrics_json = std::nullopt
            ) {
                return TryInsertWinnerAsync(job_set_id, result_delta, winner_job_id, expected_delta, expected_tag, stage, frame_hex, metrics_json).get();
            }

            static std::future<DbResult<bool>> ExistsAsync(int64_t job_set_id, int32_t result_delta, RetryPolicy rp = {});
            static inline DbResult<bool> Exists(int64_t job_set_id, int32_t result_delta) { return ExistsAsync(job_set_id, result_delta).get(); }

            static std::future<DbResult<int64_t>> CountByJobSetAsync(int64_t job_set_id, RetryPolicy rp = {});
            static inline DbResult<int64_t> CountByJobSet(int64_t job_set_id) { return CountByJobSetAsync(job_set_id).get(); }
        };

    } // namespace db
} // namespace simcore
