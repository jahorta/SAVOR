#pragma once
#include <cstdint>
#include <optional>
#include <string>

struct JobEventLite {
    int64_t event_id{};
    int64_t job_id{};
    int64_t ts{};
    std::string event_kind;
    std::optional<std::string> payload_preview;
};

struct JobEventsListScope {
    std::optional<std::string> event_kind;
    std::optional<int64_t> job_set_id;
    std::optional<int64_t> job_id;
    std::optional<int64_t> since_ts;
};
