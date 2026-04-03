#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct JobLite {
    int64_t job_id{};
    int64_t job_set_id{};
    std::optional<int64_t> savestate_id{};
    int program_kind{};
    std::string state;
    int priority{};
    int attempts{};
    int64_t queued_at{};
};

struct JobsListScope {
    std::vector<std::string> states;
    std::optional<int> program_kind;
    std::optional<int64_t> job_set_id;
    std::optional<int64_t> since_queued_at;
    std::optional<std::string> tag_key;
};
