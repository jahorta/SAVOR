// SimCore/DB/JobSetsRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include <string>
#include <optional>
#include <cstdint>
#include <future>

namespace simcore::db {

    struct JobSetRow {
        int64_t job_set_id{};
        std::optional<std::string> purpose{};
        int program_kind{};
        std::optional<std::string> created_by{};
        int64_t created_at{};
        std::optional<std::string> domain_ref_kind{};
        std::optional<int64_t> domain_ref_id{};
        std::optional<std::string> meta_text{};
        std::optional<int64_t> expected_total{};
    };

    class JobSetsRepo {
    public:
        static std::future<DbResult<int64_t>> CreateAsync(
            std::optional<std::string> purpose, int program_kind,
            std::optional<std::string> created_by,
            std::optional<std::string> domain_ref_kind,
            std::optional<int64_t> domain_ref_id,
            std::optional<std::string> meta_text,
            std::optional<int64_t> expected_total,
            RetryPolicy rp = {});

        static std::future<DbResult<JobSetRow>> GetAsync(int64_t job_set_id, RetryPolicy rp = {});

        static inline DbResult<int64_t> Create(
            std::optional<std::string> purpose, int program_kind,
            std::optional<std::string> created_by,
            std::optional<std::string> domain_ref_kind,
            std::optional<int64_t> domain_ref_id,
            std::optional<std::string> meta_text,
            std::optional<int64_t> expected_total) {
            return CreateAsync(std::move(purpose), program_kind, std::move(created_by), std::move(domain_ref_kind), std::move(domain_ref_id), std::move(meta_text), std::move(expected_total)).get();
        }
        static inline DbResult<JobSetRow> Get(int64_t job_set_id) { return GetAsync(job_set_id).get(); }
    };

} // namespace simcore::db
