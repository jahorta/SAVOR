#pragma once
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

enum class WorkerStateKind {
    Spawning, Idle, Leasing, Running, Renewing, Paused, Draining, Exiting, Stopping, Dead
};

enum class WorkerStartupPhase {
    None,
    PendingFilesystem,
    PreparingFilesystem,
    WaitingToOpen,
    Launching,
    OpeningSession,
    Failed,
};

enum class WorkerEventKind {
    Spawned, Claimed, MarkRunning, RenewLease, Heartbeat, Progress, Finished,
    Draining, Exiting, Error, Crash
};

struct WorkerEvent {
    int64_t mono_ns{};
    WorkerEventKind kind{};
    std::optional<int64_t> job_id{};
    std::string note;
};

struct WorkerSnapshot {
    int64_t worker_id{};
    std::string host;
    int pid{};
    std::string boot_uuid;
    std::uint64_t process_generation{};
    std::string log_path;
    WorkerStartupPhase startup_phase{ WorkerStartupPhase::None };

    WorkerStateKind state{ WorkerStateKind::Spawning };
    std::optional<int64_t> job_id{};
    std::optional<int> program_kind{};
    std::optional<int64_t> lease_expires_at{};
    int attempts{};
    int max_attempts{};

    int64_t start_time_utc{};                // epoch ns
    int64_t last_state_change_mono_ns{};     // monotonic ns
    int64_t last_heartbeat_mono_ns{};        // monotonic ns
    int64_t last_successful_db_call_mono_ns{};
    int consecutive_failures{};
    std::string last_error;
    int64_t last_error_mono_ns{};
    std::string last_progress;
    int64_t last_progress_mono_ns{};

    std::vector<WorkerEvent> recent_events;
};
