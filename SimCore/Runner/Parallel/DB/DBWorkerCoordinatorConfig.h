// SimCore/Runner/Scheduling/DBWorkerCoordinatorConfig.h
#pragma once
#include <string>
#include <cstdint>

struct WorkerCoordinatorConfig {
    size_t  max_concurrent_processes{ 1 };
    size_t  desired_workers{ 1 };
    uint32_t child_launch_timeout_ms{ 30000 };
    uint32_t child_shutdown_grace_ms{ 3000 };
    uint32_t heartbeat_interval_ms{ 1000 };
    uint32_t controller_sleep_ms{ 5 };
    uint32_t lease_seconds{ 30 };
    double   aging_factor{ 0.0 };
    uint32_t idle_keepalive_ms{ 300000 };
    bool start_to_paused{ true };

    std::string worker_exe_path;
    std::string iso_path;
    std::string dolphin_base_dir;
    std::string worker_dir_root;
};
