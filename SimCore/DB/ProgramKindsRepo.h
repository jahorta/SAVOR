// SimCore/DB/ProgramKindsRepo.h
#pragma once
#include "DBCore/DbResult.h"
#include "DBCore/DbRetryPolicy.h"
#include "DBCore/DbService.h"
#include <string>
#include <vector>
#include <future>
#include <optional>
#include <cstdint>

namespace simcore::db {

    struct ProgramKindRow {
        int id{};
        std::string name;
        int base_priority{};
        int spawn_ms{};
    };

    class ProgramKindsRepo {
    public:
        static std::future<DbResult<std::vector<ProgramKindRow>>> ListAllAsync(RetryPolicy rp = {});
        static inline DbResult<std::vector<ProgramKindRow>> ListAll(RetryPolicy rp = {}) { return ListAllAsync(rp).get(); }

        static std::future<DbResult<std::optional<std::string>>> GetNameAsync(int id, RetryPolicy rp = {});
        static inline DbResult<std::optional<std::string>> GetName(int id, RetryPolicy rp = {}) { return GetNameAsync(id, rp).get(); }
    };

} // namespace simcore::db
