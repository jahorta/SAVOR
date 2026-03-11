#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct JobSetLite {
    int64_t job_set_id{};
    std::optional<int64_t> parent_job_set_id{};
    int program_kind{};
    std::string purpose;
    int64_t created_at{}; // if not present in schema, set to 0 on read
    int64_t total_jobs{};
    int64_t completed_jobs{};
    int64_t succeeded_jobs{};
    int64_t failed_jobs{};
    std::optional<int64_t> expected_total{};
};

enum class JobSetStateFilter {
    Completed,
    Incomplete,
    HasFailures,
};

struct JobSetsListScope {
    std::optional<int> program_kind;
    std::optional<int64_t> min_job_set_id;
    std::optional<JobSetStateFilter> state_filter;
};
