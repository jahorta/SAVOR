#pragma once

#include <condition_variable>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace savor::db::core {

struct DbLatencySnapshot {
    std::uint64_t count = 0;
    std::uint64_t total_ms = 0;
    std::uint64_t p50_ms = 0;
    std::uint64_t p95_ms = 0;
    std::uint64_t p99_ms = 0;
    std::uint64_t max_ms = 0;
};

struct DbOperationTelemetrySnapshot {
    std::string operation_name;
    std::uint64_t enqueued = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t sqlite_busy = 0;
    std::uint64_t sqlite_locked = 0;
    DbLatencySnapshot queue_wait;
    DbLatencySnapshot execution;
    DbLatencySnapshot end_to_end;
};

struct QueuedDbLaneTelemetrySnapshot {
    std::string name;
    std::size_t depth = 0;
    std::size_t capacity = 0;
    std::size_t high_water_depth = 0;
    std::uint64_t oldest_queued_age_ms = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
    std::uint64_t near_saturation_events = 0;
    std::uint64_t at_capacity_rejections = 0;
    std::uint64_t sqlite_busy = 0;
    std::uint64_t sqlite_locked = 0;
    std::vector<DbOperationTelemetrySnapshot> operations;
};

struct QueuedDbConfig {
    std::size_t write_capacity = 4096;
    std::size_t read_capacity = 4096;
};

struct QueuedDbTelemetrySnapshot {
    QueuedDbLaneTelemetrySnapshot write_lane;
    QueuedDbLaneTelemetrySnapshot read_lane;
    std::size_t write_depth = 0;
    std::size_t read_depth = 0;
    std::size_t write_capacity = 0;
    std::size_t read_capacity = 0;
    std::size_t write_high_water_depth = 0;
    std::size_t read_high_water_depth = 0;
    std::uint64_t write_oldest_queued_age_ms = 0;
    std::uint64_t read_oldest_queued_age_ms = 0;
    std::uint64_t write_enqueued = 0;
    std::uint64_t read_enqueued = 0;
    std::uint64_t write_rejected = 0;
    std::uint64_t read_rejected = 0;
    std::uint64_t write_completed = 0;
    std::uint64_t read_completed = 0;
    std::uint64_t write_failed = 0;
    std::uint64_t read_failed = 0;
    std::uint64_t write_near_saturation_events = 0;
    std::uint64_t read_near_saturation_events = 0;
    std::uint64_t write_at_capacity_rejections = 0;
    std::uint64_t read_at_capacity_rejections = 0;
    std::uint64_t sqlite_busy = 0;
    std::uint64_t sqlite_locked = 0;
};

class QueuedDbLane final {
public:
    QueuedDbLane(std::string name, std::size_t capacity);
    ~QueuedDbLane();

    QueuedDbLane(const QueuedDbLane&) = delete;
    QueuedDbLane& operator=(const QueuedDbLane&) = delete;

    bool Start(std::string* error_out = nullptr);
    void Stop();
    [[nodiscard]] bool IsRunning() const;
    [[nodiscard]] bool Enqueue(std::string_view operation_name, std::function<void()> work);
    [[nodiscard]] bool Enqueue(std::function<void()> work);
    void RecordErrorText(std::string_view operation_name, std::string_view error_text);
    [[nodiscard]] QueuedDbLaneTelemetrySnapshot GetTelemetrySnapshot() const;

    struct LatencyAccumulator {
        std::uint64_t count = 0;
        std::uint64_t total_ms = 0;
        std::uint64_t max_ms = 0;
        std::array<std::uint64_t, 18> buckets{};
    };

private:
    struct OperationTelemetry {
        std::uint64_t enqueued = 0;
        std::uint64_t rejected = 0;
        std::uint64_t completed = 0;
        std::uint64_t failed = 0;
        std::uint64_t sqlite_busy = 0;
        std::uint64_t sqlite_locked = 0;
        LatencyAccumulator queue_wait;
        LatencyAccumulator execution;
        LatencyAccumulator end_to_end;
    };

    struct WorkItem {
        std::string operation_name;
        std::chrono::steady_clock::time_point enqueued_at;
        std::function<void()> work;
    };

    void WorkerLoop();
    void RecordCompletionLocked(
        const WorkItem& item,
        std::chrono::steady_clock::time_point started_at,
        std::chrono::steady_clock::time_point finished_at,
        bool failed);
    void RecordRejectedLocked(std::string_view operation_name, bool at_capacity);

    std::string name_;
    std::size_t capacity_ = 0;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<WorkItem> queue_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    std::size_t high_water_depth_ = 0;
    std::uint64_t enqueued_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
    std::uint64_t near_saturation_events_ = 0;
    std::uint64_t at_capacity_rejections_ = 0;
    std::uint64_t sqlite_busy_ = 0;
    std::uint64_t sqlite_locked_ = 0;
    std::map<std::string, OperationTelemetry> operations_;
};

[[nodiscard]] std::string MakeQueuedDbOperationName(
    std::string_view db_context,
    const std::source_location& location = std::source_location::current());

[[nodiscard]] QueuedDbTelemetrySnapshot BuildQueuedDbTelemetrySnapshot(
    const QueuedDbLane* write_lane,
    const QueuedDbLane* read_lane);

class QueuedDbExecutor {
protected:
    template <typename Result, typename Fn>
    Result ExecuteQueued(
        QueuedDbLane& lane,
        std::mutex& serialized_call_mtx,
        std::string operation_name,
        Fn&& fn,
        Result fallback,
        std::string* error_out) const;
};

template <typename Result, typename Fn>
Result QueuedDbExecutor::ExecuteQueued(
    QueuedDbLane& lane,
    std::mutex& serialized_call_mtx,
    std::string operation_name,
    Fn&& fn,
    Result fallback,
    std::string* error_out) const {
    if (!lane.IsRunning()) {
        if (error_out != nullptr) {
            *error_out = "queued db lane is stopped";
        }
        return fallback;
    }

    auto promise = std::make_shared<std::promise<Result>>();
    auto future = promise->get_future();
    auto work = [promise, &serialized_call_mtx, fn = std::forward<Fn>(fn)]() mutable {
        try {
            std::lock_guard<std::mutex> lock(serialized_call_mtx);
            if constexpr (std::is_void_v<Result>) {
                fn();
                promise->set_value();
            } else {
                promise->set_value(fn());
            }
        } catch (...) {
            promise->set_exception(std::current_exception());
            throw;
        }
    };

    if (!lane.Enqueue(operation_name, std::move(work))) {
        if (error_out != nullptr) {
            *error_out = "queued db lane rejected request";
        }
        return fallback;
    }

    try {
        if constexpr (std::is_void_v<Result>) {
            future.get();
            return;
        } else {
            auto result = future.get();
            if (error_out != nullptr && !error_out->empty()) {
                lane.RecordErrorText(operation_name, *error_out);
            }
            return result;
        }
    } catch (const std::exception& ex) {
        lane.RecordErrorText(operation_name, ex.what());
        if (error_out != nullptr) {
            *error_out = ex.what();
        }
    } catch (...) {
        lane.RecordErrorText(operation_name, "unknown queued db request failure");
        if (error_out != nullptr) {
            *error_out = "queued db request failed";
        }
    }
    return fallback;
}

} // namespace savor::db::core
