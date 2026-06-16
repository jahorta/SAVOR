#pragma once

#include "GUI/Refresh/RefreshCoordinator.h"

#include <QtConcurrent/QtConcurrentRun>
#include <QtCore/QFutureWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>

namespace savorqt::gui {

template <typename Data>
struct AsyncRefreshResult {
    bool ok = false;
    Data data{};
    QString errorMessage;

    static AsyncRefreshResult Ok(Data value)
    {
        AsyncRefreshResult result;
        result.ok = true;
        result.data = std::move(value);
        return result;
    }

    static AsyncRefreshResult Err(QString error)
    {
        AsyncRefreshResult result;
        result.ok = false;
        result.errorMessage = std::move(error);
        return result;
    }
};

template <typename Request, typename Data>
class AsyncRefreshPipeline final : public QObject
{
public:
    using RequestBuilder = std::function<std::optional<Request>(RefreshReason)>;
    using LoadAndPrepare = std::function<AsyncRefreshResult<Data>(Request)>;
    using Apply = std::function<void(const Data&, RefreshReason, const RefreshStatus&)>;
    using ApplyError = std::function<void(const QString&, RefreshReason, const RefreshStatus&)>;

    explicit AsyncRefreshPipeline(QObject* parent = nullptr)
        : QObject(parent)
        , coordinator_(this)
        , watcher_(this)
    {
        coordinator_.setRefreshRequestedCallback([this](RefreshReason reason) {
            startRefresh(reason);
        });
        connect(&watcher_, &QFutureWatcher<AsyncRefreshResult<Data>>::finished, this, [this]() {
            handleFinished();
        });
    }

    const RefreshStatus& status() const { return coordinator_.status(); }

    void setRequestBuilder(RequestBuilder builder) { requestBuilder_ = std::move(builder); }
    void setLoadAndPrepare(LoadAndPrepare loader) { loadAndPrepare_ = std::move(loader); }
    void setApply(Apply apply) { apply_ = std::move(apply); }
    void setApplyError(ApplyError applyError) { applyError_ = std::move(applyError); }

    void setActive(bool active) { coordinator_.setActive(active); }
    void setAutoRefreshEnabled(bool enabled) { coordinator_.setAutoRefreshEnabled(enabled); }
    void setRefreshIntervalMs(int intervalMs) { coordinator_.setRefreshIntervalMs(intervalMs); }
    bool requestRefresh(RefreshReason reason = RefreshReason::Manual) { return coordinator_.requestRefresh(reason); }

private:
    void startRefresh(RefreshReason reason)
    {
        if (!requestBuilder_ || !loadAndPrepare_) {
            coordinator_.finishRefresh(false, QStringLiteral("Refresh pipeline is not configured."));
            return;
        }

        std::optional<Request> request;
        try {
            request = requestBuilder_(reason);
        } catch (const std::exception& ex) {
            coordinator_.finishRefresh(false, QStringLiteral("Refresh request failed: %1").arg(QString::fromUtf8(ex.what())));
            return;
        } catch (...) {
            coordinator_.finishRefresh(false, QStringLiteral("Refresh request failed: unknown exception."));
            return;
        }

        if (!request.has_value()) {
            coordinator_.finishRefresh(true);
            return;
        }

        activeReason_ = reason;
        auto loader = loadAndPrepare_;
        watcher_.setFuture(QtConcurrent::run([loader = std::move(loader), request = std::move(*request)]() mutable {
            try {
                return loader(std::move(request));
            } catch (const std::exception& ex) {
                return AsyncRefreshResult<Data>::Err(QStringLiteral("Refresh failed: %1").arg(QString::fromUtf8(ex.what())));
            } catch (...) {
                return AsyncRefreshResult<Data>::Err(QStringLiteral("Refresh failed: unknown exception."));
            }
        }));
    }

    void handleFinished()
    {
        AsyncRefreshResult<Data> result;
        try {
            result = watcher_.result();
        } catch (const std::exception& ex) {
            result = AsyncRefreshResult<Data>::Err(QStringLiteral("Refresh failed: %1").arg(QString::fromUtf8(ex.what())));
        } catch (...) {
            result = AsyncRefreshResult<Data>::Err(QStringLiteral("Refresh failed: unknown exception."));
        }

        if (result.ok) {
            if (apply_) {
                apply_(result.data, activeReason_, coordinator_.status());
            }
            coordinator_.finishRefresh(true);
            return;
        }

        if (applyError_) {
            applyError_(result.errorMessage, activeReason_, coordinator_.status());
        }
        coordinator_.finishRefresh(false, result.errorMessage);
    }

    RefreshCoordinator coordinator_;
    QFutureWatcher<AsyncRefreshResult<Data>> watcher_;
    RequestBuilder requestBuilder_;
    LoadAndPrepare loadAndPrepare_;
    Apply apply_;
    ApplyError applyError_;
    RefreshReason activeReason_ = RefreshReason::Manual;
};

} // namespace savorqt::gui
