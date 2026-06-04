#include "QueuedDb.h"

#include <utility>

namespace simcore::db::core {
namespace {

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

} // namespace

QueuedDbLane::QueuedDbLane(std::string name, std::size_t capacity)
    : name_(std::move(name))
    , capacity_(capacity) {
}

QueuedDbLane::~QueuedDbLane() {
    Stop();
}

bool QueuedDbLane::Start(std::string* error_out) {
    std::lock_guard<std::mutex> lock(mtx_);
    if (running_) {
        return true;
    }
    if (capacity_ == 0) {
        SetError(error_out, name_ + " capacity must be > 0");
        return false;
    }
    stopping_ = false;
    running_ = true;
    worker_ = std::thread([this]() { WorkerLoop(); });
    return true;
}

void QueuedDbLane::Stop() {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_ && !worker_.joinable()) {
            return;
        }
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
}

bool QueuedDbLane::IsRunning() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return running_ && !stopping_;
}

bool QueuedDbLane::Enqueue(std::function<void()> work) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_ || stopping_) {
            ++rejected_;
            return false;
        }
        if (queue_.size() >= capacity_) {
            ++rejected_;
            return false;
        }
        queue_.push_back(std::move(work));
        ++enqueued_;
    }
    cv_.notify_one();
    return true;
}

QueuedDbLaneTelemetrySnapshot QueuedDbLane::GetTelemetrySnapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return QueuedDbLaneTelemetrySnapshot{
        .depth = queue_.size(),
        .enqueued = enqueued_,
        .rejected = rejected_,
        .completed = completed_,
        .failed = failed_,
    };
}

void QueuedDbLane::WorkerLoop() {
    for (;;) {
        std::function<void()> work;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                break;
            }
            work = std::move(queue_.front());
            queue_.pop_front();
        }

        try {
            work();
            std::lock_guard<std::mutex> lock(mtx_);
            ++completed_;
        } catch (...) {
            std::lock_guard<std::mutex> lock(mtx_);
            ++failed_;
        }
    }
}

} // namespace simcore::db::core
