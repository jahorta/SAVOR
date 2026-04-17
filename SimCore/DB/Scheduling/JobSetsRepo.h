// SimCore/DB/JobSetsRepo.h
#pragma once
#include "../DBCore/DbResult.h"
#include "../DBCore/DbRetryPolicy.h"
#include "../DBCore/DbService.h"
#include "../Querying/Paging.h"
#include "../Querying/JobSetListDTO.h"
#include <string>
#include <optional>
#include <cstdint>
#include <future>
#include <vector>

namespace simcore::db {

    struct JobSetPageWithFamilies {
        Page<JobSetLite> page;
        std::vector<JobSetLite> family_items;
    };

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
        // Async functions

        static std::future<DbResult<int64_t>> CreateAsync(
            std::optional<std::string> purpose, int program_kind,
            std::optional<std::string> created_by,
            std::optional<std::string> domain_ref_kind,
            std::optional<int64_t> domain_ref_id,
            std::optional<std::string> meta_text,
            std::optional<int64_t> expected_total,
            RetryPolicy rp = {});

        static std::future<DbResult<JobSetRow>> GetAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<JobSetLite>> GetLiteAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetMetaTextAsync(int64_t job_set_id, std::optional<std::string> meta_text, RetryPolicy rp = {});
        static std::future<DbResult<void>> SetExpectedTotalAsync(int64_t job_set_id, std::optional<int64_t> expected_total, RetryPolicy rp = {});
        static std::future<DbResult<Page<JobSetLite>>> ListRecentAsync(
            const JobSetsListScope& scope,
            std::optional<KeysetCursor> before, // created_at DESC, job_set_id DESC
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<JobSetPageWithFamilies>> ListRecentWithFamiliesAsync(
            const JobSetsListScope& scope,
            std::optional<KeysetCursor> before, // created_at DESC, job_set_id DESC
            int limit,
            RetryPolicy rp = {}
        );
        static std::future<DbResult<int64_t>> CreateChildAsync(
            int64_t parent_job_set_id,
            std::optional<std::string> purpose,
            int program_kind,
            std::optional<std::string> created_by,
            std::optional<std::string> domain_ref_kind,
            std::optional<int64_t> domain_ref_id,
            std::optional<std::string> meta_text,
            std::optional<int64_t> expected_total,
            RetryPolicy rp = {});
        static std::future<DbResult<std::optional<int64_t>>> GetParentAsync(int64_t job_set_id);
        static std::future<DbResult<int64_t>> GetRootJobSetIdAsync(int64_t job_set_id, RetryPolicy rp = {});
        static std::future<DbResult<std::vector<JobSetLite>>> ListFamiliesForSeedsAsync(const std::vector<int64_t>& seed_job_set_ids, RetryPolicy rp = {});
        static std::future<DbResult<void>> DeleteTreeAsync(int64_t job_set_id, RetryPolicy rp = {});

        // Blocking Helpers

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
        static inline DbResult<JobSetLite> GetLite(int64_t job_set_id) { return GetLiteAsync(job_set_id).get(); }
        static inline DbResult<void> SetMetaText(int64_t job_set_id, std::optional<std::string> meta_text) {
            return SetMetaTextAsync(job_set_id, std::move(meta_text)).get();
        }
        static inline DbResult<void> SetExpectedTotal(int64_t job_set_id, std::optional<int64_t> expected_total) {
            return SetExpectedTotalAsync(job_set_id, std::move(expected_total)).get();
        }
        static inline DbResult<int64_t> CreateChild(
            int64_t parent_job_set_id,
            std::optional<std::string> purpose,
            int program_kind,
            std::optional<std::string> created_by,
            std::optional<std::string> domain_ref_kind,
            std::optional<int64_t> domain_ref_id,
            std::optional<std::string> meta_text,
            std::optional<int64_t> expected_total) {
            return CreateChildAsync(parent_job_set_id, std::move(purpose), program_kind,
                std::move(created_by), std::move(domain_ref_kind), std::move(domain_ref_id),
                std::move(meta_text), std::move(expected_total)).get();
        }
        static inline DbResult<std::optional<int64_t>> GetParent(int64_t job_set_id) {
            return GetParentAsync(job_set_id).get();
        }
        static inline DbResult<int64_t> GetRootJobSetId(int64_t job_set_id) {
            return GetRootJobSetIdAsync(job_set_id).get();
        }
        static inline DbResult<std::vector<JobSetLite>> ListFamiliesForSeeds(const std::vector<int64_t>& seed_job_set_ids) {
            return ListFamiliesForSeedsAsync(seed_job_set_ids).get();
        }
        static inline DbResult<void> DeleteTree(int64_t job_set_id) {
            return DeleteTreeAsync(job_set_id).get();
        }

    };

} // namespace simcore::db
