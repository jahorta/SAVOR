#pragma once
#include "DbResult.h"
#include "DbRetryPolicy.h"
#include "DbService.h"
#include <string>
#include <vector>
#include <cstdint>
#include <future>

namespace simcore {
    namespace db {

        struct DbEventRow {
            int64_t id{};
            std::string kind;
            int64_t created_mono_ns{};
            std::string created_utc;
            std::string coord_boot_id;
            std::string payload;
        };

        struct DbEventsRepo {
            // Async
            static std::future<DbResult<int64_t>> InsertAsync(const std::string& kind,
                int64_t created_mono_ns,
                const std::string& created_utc,
                const std::string& coord_boot_id,
                const std::string& payload,
                RetryPolicy rp = {});
            static std::future<DbResult<void>>    InsertBootEventAsync(const std::string& kind,
                const std::string& payload,
                RetryPolicy rp = {});
            static std::future<DbResult<std::vector<DbEventRow>>> ListRecentAsync(int limit, RetryPolicy rp = {});
            static std::future<DbResult<std::vector<DbEventRow>>> ListSinceAsync(int64_t since_mono_ns,
                const std::string& kind_filter,
                RetryPolicy rp = {});

            // Blocking
            static inline DbResult<int64_t> Insert(const std::string& k, int64_t ns, const std::string& utc, const std::string& boot, const std::string& p) {
                return InsertAsync(k, ns, utc, boot, p).get();
            }
            static inline DbResult<void> InsertBootEvent(const std::string& k, const std::string& p) {
                return InsertBootEventAsync(k, p).get();
            }
            static inline DbResult<std::vector<DbEventRow>> ListRecent(int limit) {
                return ListRecentAsync(limit).get();
            }
            static inline DbResult<std::vector<DbEventRow>> ListSince(int64_t ns, const std::string& kf) {
                return ListSinceAsync(ns, kf).get();
            }
        };

    } // namespace db
} // namespace simcore
