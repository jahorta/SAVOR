#include "QueuedDb.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>

#include <utility>

namespace savor::db::core {
namespace {

constexpr std::array<std::uint64_t, 18> kLatencyBucketUpperBoundsMs{
    0, 1, 2, 5, 10, 25, 50, 100, 250, 500, 1000, 2500, 5000, 10000, 30000, 60000, 120000,
    std::numeric_limits<std::uint64_t>::max(),
};

void SetError(std::string* error_out, std::string message) {
    if (error_out != nullptr) {
        *error_out = std::move(message);
    }
}

std::uint64_t DurationMs(
    std::chrono::steady_clock::time_point start,
    std::chrono::steady_clock::time_point end) {
    if (end <= start) {
        return 0;
    }
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count());
}

void RecordLatency(QueuedDbLane::LatencyAccumulator* accumulator, std::uint64_t value_ms) {
    if (accumulator == nullptr) {
        return;
    }
    ++accumulator->count;
    accumulator->total_ms += value_ms;
    accumulator->max_ms = std::max(accumulator->max_ms, value_ms);

    for (std::size_t i = 0; i < kLatencyBucketUpperBoundsMs.size(); ++i) {
        if (value_ms <= kLatencyBucketUpperBoundsMs[i]) {
            ++accumulator->buckets[i];
            return;
        }
    }
    ++accumulator->buckets.back();
}

std::uint64_t PercentileFromBuckets(
    const QueuedDbLane::LatencyAccumulator& accumulator,
    std::uint64_t percentile) {
    if (accumulator.count == 0) {
        return 0;
    }
    const std::uint64_t target =
        std::max<std::uint64_t>(1, (accumulator.count * percentile + 99) / 100);
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < accumulator.buckets.size(); ++i) {
        seen += accumulator.buckets[i];
        if (seen >= target) {
            return kLatencyBucketUpperBoundsMs[i] == std::numeric_limits<std::uint64_t>::max()
                ? accumulator.max_ms
                : kLatencyBucketUpperBoundsMs[i];
        }
    }
    return accumulator.max_ms;
}

DbLatencySnapshot BuildLatencySnapshot(const QueuedDbLane::LatencyAccumulator& accumulator) {
    return DbLatencySnapshot{
        .count = accumulator.count,
        .total_ms = accumulator.total_ms,
        .p50_ms = PercentileFromBuckets(accumulator, 50),
        .p95_ms = PercentileFromBuckets(accumulator, 95),
        .p99_ms = PercentileFromBuckets(accumulator, 99),
        .max_ms = accumulator.max_ms,
    };
}

bool ContainsCaseInsensitive(std::string_view haystack, std::string_view needle) {
    auto to_lower = [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); };
    std::string lower_haystack;
    lower_haystack.reserve(haystack.size());
    for (const auto ch : haystack) {
        lower_haystack.push_back(to_lower(static_cast<unsigned char>(ch)));
    }

    std::string lower_needle;
    lower_needle.reserve(needle.size());
    for (const auto ch : needle) {
        lower_needle.push_back(to_lower(static_cast<unsigned char>(ch)));
    }
    return lower_haystack.find(lower_needle) != std::string::npos;
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
    return Enqueue("QueuedDb.Anonymous", std::move(work));
}

bool QueuedDbLane::Enqueue(std::string_view operation_name, std::function<void()> work) {
    const auto enqueued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!running_ || stopping_) {
            RecordRejectedLocked(operation_name, false);
            return false;
        }
        if (queue_.size() >= capacity_) {
            RecordRejectedLocked(operation_name, true);
            return false;
        }
        if ((queue_.size() + 1) * 10 >= capacity_ * 8) {
            ++near_saturation_events_;
        }
        queue_.push_back(WorkItem{
            .operation_name = operation_name.empty() ? std::string("QueuedDb.Unknown") : std::string(operation_name),
            .enqueued_at = enqueued_at,
            .work = std::move(work),
        });
        ++enqueued_;
        ++operations_[queue_.back().operation_name].enqueued;
        high_water_depth_ = std::max(high_water_depth_, queue_.size());
    }
    cv_.notify_one();
    return true;
}

void QueuedDbLane::RecordErrorText(std::string_view operation_name, std::string_view error_text) {
    const bool busy = ContainsCaseInsensitive(error_text, "sqlite_busy")
        || ContainsCaseInsensitive(error_text, "database is busy")
        || ContainsCaseInsensitive(error_text, "busy");
    const bool locked = ContainsCaseInsensitive(error_text, "sqlite_locked")
        || ContainsCaseInsensitive(error_text, "database is locked")
        || ContainsCaseInsensitive(error_text, "locked");
    if (!busy && !locked) {
        return;
    }

    std::lock_guard<std::mutex> lock(mtx_);
    auto& operation = operations_[operation_name.empty() ? std::string("QueuedDb.Unknown") : std::string(operation_name)];
    if (busy) {
        ++sqlite_busy_;
        ++operation.sqlite_busy;
    }
    if (locked) {
        ++sqlite_locked_;
        ++operation.sqlite_locked;
    }
}

QueuedDbLaneTelemetrySnapshot QueuedDbLane::GetTelemetrySnapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    std::vector<DbOperationTelemetrySnapshot> operations;
    operations.reserve(operations_.size());
    for (const auto& [name, operation] : operations_) {
        operations.push_back(DbOperationTelemetrySnapshot{
            .operation_name = name,
            .enqueued = operation.enqueued,
            .rejected = operation.rejected,
            .completed = operation.completed,
            .failed = operation.failed,
            .sqlite_busy = operation.sqlite_busy,
            .sqlite_locked = operation.sqlite_locked,
            .queue_wait = BuildLatencySnapshot(operation.queue_wait),
            .execution = BuildLatencySnapshot(operation.execution),
            .end_to_end = BuildLatencySnapshot(operation.end_to_end),
        });
    }

    const auto oldest_age_ms = queue_.empty()
        ? 0
        : DurationMs(queue_.front().enqueued_at, std::chrono::steady_clock::now());
    return QueuedDbLaneTelemetrySnapshot{
        .name = name_,
        .depth = queue_.size(),
        .capacity = capacity_,
        .high_water_depth = high_water_depth_,
        .oldest_queued_age_ms = oldest_age_ms,
        .enqueued = enqueued_,
        .rejected = rejected_,
        .completed = completed_,
        .failed = failed_,
        .near_saturation_events = near_saturation_events_,
        .at_capacity_rejections = at_capacity_rejections_,
        .sqlite_busy = sqlite_busy_,
        .sqlite_locked = sqlite_locked_,
        .operations = std::move(operations),
    };
}

void QueuedDbLane::WorkerLoop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            cv_.wait(lock, [this]() { return stopping_ || !queue_.empty(); });
            if (stopping_ && queue_.empty()) {
                break;
            }
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        const auto started_at = std::chrono::steady_clock::now();
        bool failed = false;
        try {
            item.work();
        } catch (...) {
            failed = true;
        }
        const auto finished_at = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            RecordCompletionLocked(item, started_at, finished_at, failed);
        }
    }
}

void QueuedDbLane::RecordCompletionLocked(
    const WorkItem& item,
    std::chrono::steady_clock::time_point started_at,
    std::chrono::steady_clock::time_point finished_at,
    bool failed) {
    auto& operation = operations_[item.operation_name];
    if (failed) {
        ++failed_;
        ++operation.failed;
    } else {
        ++completed_;
        ++operation.completed;
    }
    RecordLatency(&operation.queue_wait, DurationMs(item.enqueued_at, started_at));
    RecordLatency(&operation.execution, DurationMs(started_at, finished_at));
    RecordLatency(&operation.end_to_end, DurationMs(item.enqueued_at, finished_at));
}

void QueuedDbLane::RecordRejectedLocked(std::string_view operation_name, bool at_capacity) {
    ++rejected_;
    if (at_capacity) {
        ++at_capacity_rejections_;
    }
    auto& operation = operations_[operation_name.empty() ? std::string("QueuedDb.Unknown") : std::string(operation_name)];
    ++operation.rejected;
}

std::string MakeQueuedDbOperationName(
    std::string_view db_context,
    const std::source_location& location) {
    const std::string function_name = location.function_name();
    std::string method = function_name;
    const auto paren = method.find('(');
    if (paren != std::string::npos) {
        method.resize(paren);
    }
    const auto scope = method.rfind("::");
    if (scope != std::string::npos) {
        method = method.substr(scope + 2);
    }
    if (method.empty()) {
        method = "Unknown";
    }
    return std::string(db_context) + "." + method;
}

QueuedDbTelemetrySnapshot BuildQueuedDbTelemetrySnapshot(
    const QueuedDbLane* write_lane,
    const QueuedDbLane* read_lane) {
    QueuedDbTelemetrySnapshot snapshot{};
    if (write_lane != nullptr) {
        const auto lane = write_lane->GetTelemetrySnapshot();
        snapshot.write_lane = lane;
        snapshot.write_depth = lane.depth;
        snapshot.write_capacity = lane.capacity;
        snapshot.write_high_water_depth = lane.high_water_depth;
        snapshot.write_oldest_queued_age_ms = lane.oldest_queued_age_ms;
        snapshot.write_enqueued = lane.enqueued;
        snapshot.write_rejected = lane.rejected;
        snapshot.write_completed = lane.completed;
        snapshot.write_failed = lane.failed;
        snapshot.write_near_saturation_events = lane.near_saturation_events;
        snapshot.write_at_capacity_rejections = lane.at_capacity_rejections;
        snapshot.sqlite_busy += lane.sqlite_busy;
        snapshot.sqlite_locked += lane.sqlite_locked;
    }
    if (read_lane != nullptr) {
        const auto lane = read_lane->GetTelemetrySnapshot();
        snapshot.read_lane = lane;
        snapshot.read_depth = lane.depth;
        snapshot.read_capacity = lane.capacity;
        snapshot.read_high_water_depth = lane.high_water_depth;
        snapshot.read_oldest_queued_age_ms = lane.oldest_queued_age_ms;
        snapshot.read_enqueued = lane.enqueued;
        snapshot.read_rejected = lane.rejected;
        snapshot.read_completed = lane.completed;
        snapshot.read_failed = lane.failed;
        snapshot.read_near_saturation_events = lane.near_saturation_events;
        snapshot.read_at_capacity_rejections = lane.at_capacity_rejections;
        snapshot.sqlite_busy += lane.sqlite_busy;
        snapshot.sqlite_locked += lane.sqlite_locked;
    }
    return snapshot;
}

} // namespace savor::db::core
