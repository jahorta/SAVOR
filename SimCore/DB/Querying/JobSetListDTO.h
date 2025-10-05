#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct JobSetLite {
    int64_t job_set_id{};
    int program_kind{};
    std::string purpose;
    int64_t created_at{}; // if not present in schema, set to 0 on read
};

struct JobSetsListScope {
    std::optional<int> program_kind;
    std::optional<int64_t> min_job_set_id;
};
