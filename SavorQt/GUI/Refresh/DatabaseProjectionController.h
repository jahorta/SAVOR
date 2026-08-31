#pragma once

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QDateTime>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QTimer>

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

namespace savorqt::gui {

enum class RefreshReason {
    Initial,
    Manual,
    Auto,
    Pending,
    ActiveChanged,
};

struct RefreshIntent {
    QString scope_key;
    std::uint64_t revision = 0;

    friend bool operator==(const RefreshIntent&, const RefreshIntent&) = default;
};

struct RefreshStatus {
    bool active = false;
    bool auto_refresh_enabled = false;
    bool refresh_in_flight = false;
    bool refresh_pending = false;
    int refresh_interval_ms = 5000;
    QDateTime last_refresh_at;
    QString last_error;
};

template <typename Projection>
struct ProjectionLoadResult {
    bool ok = false;
    Projection value{};
    QString error;

    static ProjectionLoadResult Ok(Projection projection)
    {
        ProjectionLoadResult result;
        result.ok = true;
        result.value = std::move(projection);
        return result;
    }

    static ProjectionLoadResult Error(QString message)
    {
        ProjectionLoadResult result;
        result.error = std::move(message);
        return result;
    }
};

template <typename Request, typename Projection>
class DatabaseProjectionController final : public QObject
{
public:
    using RequestBuilder = std::function<std::optional<Request>(RefreshReason)>;
    using IntentBuilder = std::function<RefreshIntent(const Request&, RefreshReason)>;
    using LoadAndPrepare = std::function<ProjectionLoadResult<Projection>(Request)>;
    using Apply = std::function<void(const Projection&, RefreshReason, const RefreshStatus&)>;
    using ApplyError = std::function<void(const QString&, RefreshReason, const RefreshStatus&)>;

    explicit DatabaseProjectionController(QObject* parent = nullptr)
        : QObject(parent)
    {
        timer_.setSingleShot(false);
        connect(&timer_, &QTimer::timeout, this, [this]() { requestRefresh(RefreshReason::Auto); });
        connect(&watcher_, &QFutureWatcher<ProjectionLoadResult<Projection>>::finished,
                this, [this]() { finishCurrent(); });
    }

    const RefreshStatus& status() const { return status_; }

    void setRequestBuilder(RequestBuilder builder) { request_builder_ = std::move(builder); }
    void setIntentBuilder(IntentBuilder builder) { intent_builder_ = std::move(builder); }
    void setLoadAndPrepare(LoadAndPrepare loader) { loader_ = std::move(loader); }
    void setApply(Apply apply) { apply_ = std::move(apply); }
    void setApplyError(ApplyError apply_error) { apply_error_ = std::move(apply_error); }

    void setActive(bool active)
    {
        if (status_.active == active) {
            return;
        }
        status_.active = active;
        if (!active) {
            ++default_intent_revision_;
            latest_intent_ = RefreshIntent{QStringLiteral("inactive"), default_intent_revision_};
            pending_.reset();
            status_.refresh_pending = false;
        }
        updateTimer();
    }

    void setAutoRefreshEnabled(bool enabled)
    {
        status_.auto_refresh_enabled = enabled;
        updateTimer();
    }

    void setRefreshIntervalMs(int interval_ms)
    {
        status_.refresh_interval_ms = qMax(1, interval_ms);
        updateTimer();
    }

    void requestRefresh(RefreshReason reason)
    {
        if (!status_.active || !request_builder_ || !loader_) {
            return;
        }

        const auto request = request_builder_(reason);
        if (!request.has_value()) {
            return;
        }

        Snapshot snapshot;
        snapshot.request = *request;
        snapshot.reason = reason;
        if (intent_builder_) {
            snapshot.intent = intent_builder_(snapshot.request, reason);
        } else if (reason == RefreshReason::Auto && active_.has_value() && !pending_.has_value()) {
            // An automatic refresh for the unchanged default scope is already represented
            // by the running request. Do not invalidate it or create an endless tail.
            return;
        } else {
            snapshot.intent = RefreshIntent{QString(), ++default_intent_revision_};
        }

        latest_intent_ = snapshot.intent;
        if (active_.has_value()) {
            if (pending_.has_value() && pending_->intent == snapshot.intent
                && reason == RefreshReason::Auto) {
                return;
            }
            pending_ = std::move(snapshot);
            status_.refresh_pending = true;
            return;
        }
        start(std::move(snapshot));
    }

private:
    struct Snapshot {
        Request request{};
        RefreshIntent intent;
        RefreshReason reason = RefreshReason::Manual;
    };

    void start(Snapshot snapshot)
    {
        active_ = std::move(snapshot);
        status_.refresh_in_flight = true;
        const Request request = active_->request;
        const LoadAndPrepare loader = loader_;
        watcher_.setFuture(QtConcurrent::run([loader, request]() mutable {
            return loader(std::move(request));
        }));
    }

    void finishCurrent()
    {
        const auto finished = active_;
        const auto result = watcher_.result();
        active_.reset();
        status_.refresh_in_flight = false;

        if (finished.has_value() && status_.active && finished->intent == latest_intent_) {
            if (result.ok) {
                status_.last_refresh_at = QDateTime::currentDateTime();
                status_.last_error.clear();
                if (apply_) {
                    apply_(result.value, finished->reason, status_);
                }
            } else {
                status_.last_error = result.error;
                if (apply_error_) {
                    apply_error_(result.error, finished->reason, status_);
                }
            }
        }

        if (pending_.has_value() && status_.active) {
            Snapshot next = std::move(*pending_);
            pending_.reset();
            status_.refresh_pending = false;
            start(std::move(next));
        } else {
            pending_.reset();
            status_.refresh_pending = false;
        }
    }

    void updateTimer()
    {
        if (status_.active && status_.auto_refresh_enabled) {
            timer_.start(status_.refresh_interval_ms);
        } else {
            timer_.stop();
        }
    }

    RequestBuilder request_builder_;
    IntentBuilder intent_builder_;
    LoadAndPrepare loader_;
    Apply apply_;
    ApplyError apply_error_;
    QTimer timer_;
    QFutureWatcher<ProjectionLoadResult<Projection>> watcher_;
    RefreshStatus status_;
    std::optional<Snapshot> active_;
    std::optional<Snapshot> pending_;
    RefreshIntent latest_intent_;
    std::uint64_t default_intent_revision_ = 0;
};

} // namespace savorqt::gui
