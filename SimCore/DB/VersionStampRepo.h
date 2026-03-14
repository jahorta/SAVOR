#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <future>
#include <string>
#include <cstdint>

namespace simcore {
    namespace db {

        struct VersionStampRow {
            int64_t id{};
            std::string dolphin_build_hash;
            std::string wrapper_commit;
            std::string simcore_commit;
            std::string vm_opcode_hash;
        };

        struct VersionStampRepo {
            // Async
            static std::future<DbResult<int64_t>> EnsureAsync(const std::string& dolphin_build_hash,
                const std::string& wrapper_commit,
                const std::string& simcore_commit,
                const std::string& vm_opcode_hash,
                RetryPolicy rp = {});
            static std::future<DbResult<VersionStampRow>> GetAsync(int64_t id, RetryPolicy rp = {});

            // Blocking
            static inline DbResult<int64_t> Ensure(const std::string& a, const std::string& b, const std::string& c, const std::string& d) {
                return EnsureAsync(a, b, c, d).get();
            }
            static inline DbResult<VersionStampRow> Get(int64_t id) {
                return GetAsync(id).get();
            }
        };

    } // namespace db
} // namespace simcore
