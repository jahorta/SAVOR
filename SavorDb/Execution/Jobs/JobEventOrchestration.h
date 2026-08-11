#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct sqlite3;

namespace savor::db::execution::jobs {

enum class JobLifecycleEventKind {
    JobSetCreated = 0,
    JobQueued,
    JobClaimed,
    JobStarted,
    JobLeaseRenewed,
    JobCompleted,
    JobEventArchived,
    JobRestored,
};

const char* ToEventType(JobLifecycleEventKind kind);

struct JobLifecycleEventCommand {
    JobLifecycleEventKind kind = JobLifecycleEventKind::JobQueued;
    std::int64_t job_set_id = 0;
    std::int64_t job_id = 0;
    std::optional<std::string> message;
    std::optional<std::int64_t> artifact_id;
    std::optional<std::string> claimed_by_token;
    std::optional<std::int64_t> lease_expires_at_utc;
    std::optional<std::string> terminal_state;
    std::optional<std::string> requested_by;
    std::optional<std::string> causation_id;
};

struct IJobEventCommandService {
    virtual ~IJobEventCommandService() = default;
    virtual bool AppendLifecycleEvent(const JobLifecycleEventCommand& command, std::string* error_out) = 0;
};

class SqliteJobEventCommandService final : public IJobEventCommandService {
public:
    explicit SqliteJobEventCommandService(sqlite3* sqlite_db);

    bool AppendLifecycleEvent(const JobLifecycleEventCommand& command, std::string* error_out) override;

private:
    sqlite3* db_ = nullptr;
};

} // namespace savor::db::execution::jobs
