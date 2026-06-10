#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QObject>
#include <QtCore/QTimer>

#include <functional>

namespace soasimqt2::gui {

enum class RefreshReason {
    Initial,
    Manual,
    Auto,
    Pending,
    ActiveChanged,
};

struct RefreshStatus {
    bool active = false;
    bool autoRefreshEnabled = true;
    bool refreshInFlight = false;
    bool pendingRefresh = false;
    int refreshIntervalMs = 2000;
    QDateTime lastRefresh;
    QString errorMessage;
};

class RefreshCoordinator final : public QObject
{
public:
    explicit RefreshCoordinator(QObject* parent = nullptr)
        : QObject(parent)
        , timer_(new QTimer(this))
    {
        connect(timer_, &QTimer::timeout, this, [this]() {
            requestRefresh(RefreshReason::Auto);
        });
    }

    const RefreshStatus& status() const { return status_; }

    void setRefreshRequestedCallback(std::function<void(RefreshReason)> callback)
    {
        refreshRequested_ = std::move(callback);
    }

    void setActive(bool active)
    {
        if (status_.active == active) {
            return;
        }

        status_.active = active;
        syncTimer();
        if (active) {
            requestRefresh(RefreshReason::ActiveChanged);
        }
    }

    void setAutoRefreshEnabled(bool enabled)
    {
        if (status_.autoRefreshEnabled == enabled) {
            return;
        }

        status_.autoRefreshEnabled = enabled;
        syncTimer();
    }

    void setRefreshIntervalMs(int intervalMs)
    {
        status_.refreshIntervalMs = intervalMs > 0 ? intervalMs : 1000;
        syncTimer();
    }

    bool requestRefresh(RefreshReason reason)
    {
        if (!status_.active && reason == RefreshReason::Auto) {
            return false;
        }
        if (status_.refreshInFlight) {
            status_.pendingRefresh = true;
            return false;
        }

        status_.refreshInFlight = true;
        status_.pendingRefresh = false;
        status_.errorMessage.clear();
        if (refreshRequested_) {
            refreshRequested_(reason);
        }
        return true;
    }

    bool finishRefresh(bool ok, const QString& errorMessage = QString())
    {
        status_.refreshInFlight = false;
        if (ok) {
            status_.lastRefresh = QDateTime::currentDateTime();
            status_.errorMessage.clear();
        } else {
            status_.errorMessage = errorMessage;
        }

        const bool shouldRunPending = status_.pendingRefresh;
        status_.pendingRefresh = false;
        if (shouldRunPending) {
            return requestRefresh(RefreshReason::Pending);
        }
        return false;
    }

private:
    void syncTimer()
    {
        if (status_.active && status_.autoRefreshEnabled) {
            timer_->start(status_.refreshIntervalMs);
        } else {
            timer_->stop();
        }
    }

    RefreshStatus status_;
    QTimer* timer_ = nullptr;
    std::function<void(RefreshReason)> refreshRequested_;
};

} // namespace soasimqt2::gui
