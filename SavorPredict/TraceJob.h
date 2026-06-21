#pragma once

#include <filesystem>
#include <iosfwd>
#include <optional>

namespace savor::predict {

struct TraceJobOptions {
    std::filesystem::path db_root = "D:/SavorPredictDB";
    std::optional<long long> turn_job_id;
    std::optional<long long> exec_job_id;
    int max_distance = 5000;
    bool json = false;
};

int run_trace_job(const TraceJobOptions& options, std::ostream& out, std::ostream& err);

} // namespace savor::predict
