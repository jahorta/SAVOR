#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace simcore::db::core {

struct QueuedDbLaneTelemetrySnapshot {
    std::size_t depth = 0;
    std::uint64_t enqueued = 0;
    std::uint64_t rejected = 0;
    std::uint64_t completed = 0;
    std::uint64_t failed = 0;
};

struct QueuedDbConfig {
    std::size_t write_capacity = 4096;
    std::size_t read_capacity = 4096;
};

struct QueuedDbTelemetrySnapshot {
    std::size_t write_depth = 0;
    std::size_t read_depth = 0;
    std::uint64_t write_enqueued = 0;
    std::uint64_t read_enqueued = 0;
    std::uint64_t write_rejected = 0;
    std::uint64_t read_rejected = 0;
    std::uint64_t write_completed = 0;
    std::uint64_t read_completed = 0;
    std::uint64_t write_failed = 0;
    std::uint64_t read_failed = 0;
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
    [[nodiscard]] bool Enqueue(std::function<void()> work);
    [[nodiscard]] QueuedDbLaneTelemetrySnapshot GetTelemetrySnapshot() const;

private:
    void WorkerLoop();

    std::string name_;
    std::size_t capacity_ = 0;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    std::thread worker_;
    bool running_ = false;
    bool stopping_ = false;
    std::uint64_t enqueued_ = 0;
    std::uint64_t rejected_ = 0;
    std::uint64_t completed_ = 0;
    std::uint64_t failed_ = 0;
};

class QueuedDbExecutor {
protected:
    template <typename Result, typename Fn>
    Result ExecuteQueued(
        QueuedDbLane& lane,
        std::mutex& serialized_call_mtx,
        Fn&& fn,
        Result fallback,
        std::string* error_out) const;
};

template <typename Result, typename Fn>
Result QueuedDbExecutor::ExecuteQueued(
    QueuedDbLane& lane,
    std::mutex& serialized_call_mtx,
    Fn&& fn,
    Result fallback,
    std::string* error_out) const {
    if (!lane.IsRunning()) {
        if (error_out != nullptr) {
            *error_out = "queued db lane is stopped";
        }
        return fallback;
    }

    auto task = std::make_shared<std::packaged_task<Result()>>(
        [&serialized_call_mtx, fn = std::forward<Fn>(fn)]() mutable {
            std::lock_guard<std::mutex> lock(serialized_call_mtx);
            return fn();
        });
    auto future = task->get_future();
    if (!lane.Enqueue([task]() { (*task)(); })) {
        if (error_out != nullptr) {
            *error_out = "queued db lane rejected request";
        }
        return fallback;
    }

    try {
        return future.get();
    } catch (const std::exception& ex) {
        if (error_out != nullptr) {
            *error_out = ex.what();
        }
    } catch (...) {
        if (error_out != nullptr) {
            *error_out = "queued db request failed";
        }
    }
    return fallback;
}

} // namespace simcore::db::core
