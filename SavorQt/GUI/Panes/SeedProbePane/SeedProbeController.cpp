#include "SeedProbeController.h"

#include <QtCore/QTimer>
#include <QtCore/QSettings>

#include "Core/Input/GCInputFrameFmt.h"
#include "SavorDbRuntime.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <string>
#include <utility>

using savor::DescribeFrameCompact;
using savor::GCInputFrame;
using savor::db::UiReadSeedProbeRunListCursor;
using savor::db::UiReadSeedProbeRunListQuery;
using savor::db::UiSeedProbeDeltaPoint;
using savor::db::UiSeedProbeRunSummary;
using savor::db::UiSeedProbeUniqueValue;

namespace {
constexpr auto kSettingsGroup = "SeedProbePane";
constexpr auto kSearchKey = "search";
constexpr auto kOnlyDoneKey = "only_done";
constexpr auto kAutoRefreshKey = "auto_refresh";
constexpr auto kRefreshSecondsKey = "refresh_seconds";
constexpr auto kPageLimitKey = "page_limit";
constexpr auto kDataSourceUnavailableMessage = "Seed probe data source is not implemented in this Qt2 migration slice yet.";

QString describeException(const char* prefix)
{
    try {
        throw;
    } catch (const std::exception& ex) {
        return QStringLiteral("%1: %2").arg(QString::fromUtf8(prefix), QString::fromUtf8(ex.what()));
    } catch (...) {
        return QStringLiteral("%1: unknown exception").arg(QString::fromUtf8(prefix));
    }
}

template <typename AsyncCall>
auto runAsyncFetch(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}

QString hex32(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0'));
}

QString neutralSeedText(const std::optional<std::int64_t>& neutralSeed)
{
    return neutralSeed.has_value() ? hex32(static_cast<quint32>(*neutralSeed)) : QStringLiteral("n/a");
}

int inferSpan(const std::vector<int>& coords)
{
    if (coords.size() < 2) {
        return 2;
    }
    std::vector<int> sorted = coords;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    int best = 0;
    for (size_t i = 1; i < sorted.size(); ++i) {
        const int diff = sorted[i] - sorted[i - 1];
        if (diff > 1) {
            best = best == 0 ? diff : std::min(best, diff);
        }
    }
    return best > 1 ? best : 2;
}

SeedProbeController::GridView buildGrid(const std::vector<UiSeedProbeDeltaPoint>& rows, const char* family, int minNeg, int maxPos)
{
    SeedProbeController::GridView view;
    std::vector<int> xs;
    std::vector<int> ys;

    struct RawPoint { int x; int y; int delta; };
    std::vector<RawPoint> raw;
    raw.reserve(rows.size());

    for (const UiSeedProbeDeltaPoint& row : rows) {
        if (row.source_family != family) {
            continue;
        }

        RawPoint point{};
        point.delta = static_cast<int>(row.seed_delta);
        point.x = row.axis_x;
        point.y = row.axis_y;
        raw.push_back(point);
        xs.push_back(point.x);
        ys.push_back(point.y);
    }

    if (raw.empty()) {
        return view;
    }

    const int xSpan = inferSpan(xs);
    const int ySpan = inferSpan(ys);
    view.minNeg = minNeg;
    view.maxPos = maxPos;
    view.hasData = true;
    for (const RawPoint& point : raw) {
        view.points.push_back(SeedProbeController::GridPoint{ point.x, point.y, xSpan, ySpan, point.delta });
    }
    return view;
}

QVector<int> buildLegend(const std::vector<UiSeedProbeDeltaPoint>& rows)
{
    QVector<int> values;
    values.reserve(static_cast<int>(rows.size()));
    for (const UiSeedProbeDeltaPoint& row : rows) {
        values.push_back(static_cast<int>(row.seed_delta));
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

QVector<SeedProbeController::UniqueSeedRow> buildSearchRows(
    const std::vector<UiSeedProbeUniqueValue>& uniqueRows)
{
    QVector<SeedProbeController::UniqueSeedRow> rows;
    rows.reserve(static_cast<int>(uniqueRows.size()));
    for (const UiSeedProbeUniqueValue& row : uniqueRows) {
        GCInputFrame frame{};
        frame.main_x = static_cast<std::uint8_t>(row.main_x);
        frame.main_y = static_cast<std::uint8_t>(row.main_y);
        frame.c_x = static_cast<std::uint8_t>(row.cstick_x);
        frame.c_y = static_cast<std::uint8_t>(row.cstick_y);
        frame.trig_l = static_cast<std::uint8_t>(row.trigger_x);
        frame.trig_r = static_cast<std::uint8_t>(row.trigger_y);
        rows.push_back({ QString::fromStdString(DescribeFrameCompact(frame)), hex32(static_cast<quint32>(row.seed_value)) });
    }
    return rows;
}

QString resolveSavestateLabel(qint64 probeId, qint64 savestateId)
{
    (void)probeId;
    return QString::number(savestateId);
}

}

SeedProbeController::SeedProbeController(QObject* parent)
    : QObject(parent)
{
    loadSettings();
    syncFetchStateFromView();

    pageRefreshPipeline_ = new savorqt::gui::AsyncRefreshPipeline<ListFetchRequest, ListBundleResult>(this);
    pageRefreshPipeline_->setAutoRefreshEnabled(false);
    pageRefreshPipeline_->setRequestBuilder([this](savorqt::gui::RefreshReason) -> std::optional<ListFetchRequest> {
        pageInFlight_ = true;
        state_.loadingList = true;
        state_.errorMessage.clear();
        UiReadSeedProbeRunListQuery query;
        query.before = before_;
        query.after = after_;
        query.limit = fetchPageLimit_;
        query.search = fetchSearch_.toStdString();
        query.only_completed = fetchOnlyDone_;
        emitStateChanged();
        return ListFetchRequest{ query };
    });
    pageRefreshPipeline_->setLoadAndPrepare([](ListFetchRequest request) {
        auto* uiReadDb = savorqt::SavorDbRuntime::instance().uiReadDb();
        if (uiReadDb == nullptr) {
            ListBundleResult result;
            result.errorMessage = QString::fromUtf8(kDataSourceUnavailableMessage);
            return savorqt::gui::AsyncRefreshResult<ListBundleResult>::Ok(result);
        }

        ListBundle bundle{};
        bundle.page = uiReadDb->ListSeedProbeRuns(request.query);
        bundle.rows.reserve(static_cast<int>(bundle.page.items.size()));
        for (const UiSeedProbeRunSummary& item : bundle.page.items) {
            ProbeSummary row{};
            row.probeId = item.probe_run_id;
            row.status = QString::fromStdString(item.status);
            row.savestateId = item.entry_savestate_id;
            row.filename = resolveSavestateLabel(row.probeId, row.savestateId);
            bundle.rows.push_back(row);
        }
        ListBundleResult result;
        result.ok = true;
        result.value = std::move(bundle);
        return savorqt::gui::AsyncRefreshResult<ListBundleResult>::Ok(result);
    });
    pageRefreshPipeline_->setApply([this](const ListBundleResult& result, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        pageInFlight_ = false;
        state_.loadingList = false;
        if (result.ok) {
            state_.page = result.value.page;
            state_.probeRows = result.value.rows;
            state_.lastRefresh = QDateTime::currentDateTime();
            state_.errorMessage.clear();
            state_.infoMessage.clear();

            bool found = false;
            for (const ProbeSummary& row : state_.probeRows) {
                if (row.probeId == state_.selectedProbeId) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                state_.selectedProbeId = state_.probeRows.isEmpty() ? 0 : state_.probeRows.front().probeId;
            }
            if (state_.selectedProbeId > 0) {
                kickDetailFetch(state_.selectedProbeId);
            }
        } else {
            state_.page = {};
            state_.probeRows.clear();
            state_.errorMessage = QStringLiteral("Seed probes failed: %1").arg(result.errorMessage);
        }
        emitStateChanged();
    });
    pageRefreshPipeline_->setApplyError([this](const QString& error, savorqt::gui::RefreshReason, const savorqt::gui::RefreshStatus&) {
        pageInFlight_ = false;
        state_.loadingList = false;
        state_.page = {};
        state_.probeRows.clear();
        state_.errorMessage = error;
        emitStateChanged();
    });

    connect(&runningRefreshWatcher_, &QFutureWatcher<RunningProbeUpdateResult>::finished, this, [this]() {
        bool changed = false;
        try {
            const auto result = runningRefreshWatcher_.result();
            runningRefreshInFlight_ = false;
            if (result.ok) {
                state_.lastRefresh = QDateTime::currentDateTime();
                state_.errorMessage.clear();
                for (const RunningProbeUpdate& update : result.value) {
                    for (ProbeSummary& row : state_.probeRows) {
                        if (row.probeId == update.probeId) {
                            changed = changed || row.status != update.statusText;
                            row.status = update.statusText;
                            break;
                        }
                    }
                    if (state_.selectedProbeId == update.probeId) {
                        changed = changed || state_.statusText != update.statusText;
                        state_.statusText = update.statusText;
                    }
                }
            } else {
                state_.errorMessage = QStringLiteral("Seed probe refresh failed: %1").arg(result.errorMessage);
                changed = true;
            }
        } catch (...) {
            runningRefreshInFlight_ = false;
            state_.errorMessage = describeException("Seed probe refresh failed");
            changed = true;
        }
        if (changed) {
            emitStateChanged();
        }
    });

    connect(&detailWatcher_, &QFutureWatcher<DetailBundleResult>::finished, this, [this]() {
        try {
            const auto result = detailWatcher_.result();
            detailInFlight_ = false;
            state_.loadingDetail = false;
            if (result.ok && detailRequestProbeId_ == state_.selectedProbeId) {
                state_.lastRefresh = QDateTime::currentDateTime();
                state_.neutralSeedText = neutralSeedText(result.value.summary.neutral_seed_value);
                state_.probeIdText = QString::number(result.value.summary.probe_run_id);
                state_.statusText = QString::fromStdString(result.value.summary.status);
                state_.codecVersionText = QString::number(result.value.summary.codec_version);
                state_.savestateText = result.value.savestateText;
                state_.mainGrid = result.value.mainGrid;
                state_.cStickGrid = result.value.cStickGrid;
                state_.triggerGrid = result.value.triggerGrid;
                state_.legendDeltas = result.value.legendDeltas;
                state_.uniqueRows = result.value.uniqueRows;
                state_.errorMessage.clear();
            } else if (!result.ok) {
                state_.errorMessage = QStringLiteral("Seed probe detail failed: %1").arg(result.errorMessage);
            }
        } catch (...) {
            detailInFlight_ = false;
            state_.loadingDetail = false;
            state_.errorMessage = describeException("Seed probe detail failed");
        }
        emitStateChanged();
    });

    refreshTimer_ = new QTimer(this);
    connect(refreshTimer_, &QTimer::timeout, this, [this]() {
        if (!canAutoRefresh()) {
            return;
        }

        QVector<qint64> runningProbeIds;
        runningProbeIds.reserve(state_.probeRows.size());
        for (const ProbeSummary& row : state_.probeRows) {
            if (row.status.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0) {
                runningProbeIds.push_back(row.probeId);
            }
        }

        if (runningProbeIds.isEmpty()) {
            return;
        }

        const bool selectedIsRunning = std::any_of(state_.probeRows.cbegin(), state_.probeRows.cend(), [this](const ProbeSummary& row) {
            return row.probeId == state_.selectedProbeId
                && row.status.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0;
        });
        if (selectedIsRunning) {
            kickDetailFetch(state_.selectedProbeId, true);
            runningProbeIds.erase(std::remove(runningProbeIds.begin(), runningProbeIds.end(), state_.selectedProbeId), runningProbeIds.end());
        }
        if (!runningProbeIds.isEmpty()) {
            kickRunningRefresh(runningProbeIds);
        }
    });
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
}

const SeedProbeController::ViewState& SeedProbeController::viewState() const
{
    return state_;
}

void SeedProbeController::loadInitial()
{
    if (initialLoadStarted_ || !pageActive_) {
        return;
    }
    initialLoadStarted_ = true;
    kickPageFetch();
}

void SeedProbeController::setPageActive(bool active)
{
    if (pageActive_ == active) {
        return;
    }

    pageActive_ = active;
    if (!pageActive_) {
        if (refreshTimer_) {
            refreshTimer_->stop();
        }
        return;
    }

    if (refreshTimer_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
    loadInitial();
}

void SeedProbeController::setSearch(const QString& search)
{
    state_.search = search;
}

void SeedProbeController::setOnlyDone(bool onlyDone)
{
    state_.onlyDone = onlyDone;
}

void SeedProbeController::setAutoRefreshEnabled(bool enabled)
{
    state_.autoRefresh = enabled;
    persistSettings();
    emitStateChanged();
}

void SeedProbeController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = seconds;
    persistSettings();
    if (pageActive_) {
        refreshTimer_->start(seconds * 1000);
    }
    emitStateChanged();
}

void SeedProbeController::applyFilters(int pageLimit)
{
    state_.pageLimit = pageLimit;
    before_.reset();
    after_.reset();
    state_.selectedProbeId = 0;
    state_.neutralSeedText.clear();
    state_.probeIdText.clear();
    state_.statusText.clear();
    state_.codecVersionText.clear();
    state_.savestateText.clear();
    state_.mainGrid = {};
    state_.cStickGrid = {};
    state_.triggerGrid = {};
    state_.legendDeltas.clear();
    state_.uniqueRows.clear();
    syncFetchStateFromView();
    persistSettings();
    kickPageFetch();
}

void SeedProbeController::resetFilters()
{
    state_.search.clear();
    state_.onlyDone = false;
    state_.autoRefresh = true;
    state_.refreshSeconds = 2;
    state_.pageLimit = 50;
    before_.reset();
    after_.reset();
    state_.selectedProbeId = 0;
    state_.neutralSeedText.clear();
    state_.probeIdText.clear();
    state_.statusText.clear();
    state_.codecVersionText.clear();
    state_.savestateText.clear();
    state_.mainGrid = {};
    state_.cStickGrid = {};
    state_.triggerGrid = {};
    state_.legendDeltas.clear();
    state_.uniqueRows.clear();
    syncFetchStateFromView();
    persistSettings();
    if (pageActive_) {
        refreshTimer_->start(state_.refreshSeconds * 1000);
    }
    kickPageFetch();
    emitStateChanged();
}

void SeedProbeController::requestRefresh()
{
    before_.reset();
    after_.reset();
    kickPageFetch();
}

void SeedProbeController::requestNextPage()
{
    if (!state_.page.next.has_value()) {
        return;
    }
    before_ = state_.page.next;
    after_.reset();
    kickPageFetch();
}

void SeedProbeController::requestPreviousPage()
{
    if (!state_.page.prev.has_value()) {
        return;
    }
    after_ = state_.page.prev;
    before_.reset();
    kickPageFetch();
}

void SeedProbeController::selectProbe(qint64 probeId)
{
    if (probeId <= 0 || state_.selectedProbeId == probeId) {
        return;
    }
    state_.selectedProbeId = probeId;
    kickDetailFetch(probeId, true);
    emitStateChanged();
}

void SeedProbeController::kickPageFetch()
{
    if (pageRefreshPipeline_ != nullptr) {
        pageRefreshPipeline_->requestRefresh(savorqt::gui::RefreshReason::Manual);
    }
}

void SeedProbeController::kickDetailFetch(qint64 probeId, bool force)
{
    if (probeId <= 0 || detailInFlight_) {
        return;
    }
    if (!force && detailRequestProbeId_ == probeId && !state_.neutralSeedText.isEmpty()) {
        return;
    }

    detailInFlight_ = true;
    detailRequestProbeId_ = probeId;
    state_.loadingDetail = true;

    detailWatcher_.setFuture(runAsyncFetch([probeId]() -> DetailBundleResult {
        auto* uiReadDb = savorqt::SavorDbRuntime::instance().uiReadDb();
        if (uiReadDb == nullptr) {
            DetailBundleResult result;
            result.errorMessage = QString::fromUtf8(kDataSourceUnavailableMessage);
            return result;
        }
        const auto summary = uiReadDb->GetSeedProbeRunSummary(probeId);
        if (!summary.has_value()) {
            DetailBundleResult result;
            result.errorMessage = QStringLiteral("Seed probe run %1 was not found in UIRead").arg(probeId);
            return result;
        }
        const auto gridRows = uiReadDb->ListSeedProbeDeltaPoints(probeId);
        const auto uniqueRows = uiReadDb->ListSeedProbeUniqueValues(probeId);

        int minNeg = 0;
        int maxPos = 0;
        for (const UiSeedProbeDeltaPoint& row : gridRows) {
            minNeg = std::min(minNeg, static_cast<int>(row.seed_delta));
            maxPos = std::max(maxPos, static_cast<int>(row.seed_delta));
        }

        DetailBundle bundle{};
        bundle.summary = *summary;
        bundle.savestateText = resolveSavestateLabel(probeId, summary->entry_savestate_id);
        bundle.mainGrid = buildGrid(gridRows, "MAIN", minNeg, maxPos);
        bundle.cStickGrid = buildGrid(gridRows, "CSTICK", minNeg, maxPos);
        bundle.triggerGrid = buildGrid(gridRows, "TRIGGER", minNeg, maxPos);
        bundle.legendDeltas = buildLegend(gridRows);
        bundle.uniqueRows = buildSearchRows(uniqueRows);
        DetailBundleResult result;
        result.ok = true;
        result.value = std::move(bundle);
        return result;
    }));
    emitStateChanged();
}

bool SeedProbeController::canAutoRefresh() const
{
    return pageActive_ && state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !detailInFlight_ && !runningRefreshInFlight_;
}


void SeedProbeController::kickRunningRefresh(const QVector<qint64>& probeIds)
{
    if (probeIds.isEmpty() || runningRefreshInFlight_) {
        return;
    }

    runningRefreshInFlight_ = true;
    runningRefreshWatcher_.setFuture(runAsyncFetch([probeIds]() -> RunningProbeUpdateResult {
        auto* uiReadDb = savorqt::SavorDbRuntime::instance().uiReadDb();
        if (uiReadDb == nullptr) {
            RunningProbeUpdateResult result;
            result.errorMessage = QString::fromUtf8(kDataSourceUnavailableMessage);
            return result;
        }
        QVector<RunningProbeUpdate> updates;
        updates.reserve(probeIds.size());
        for (qint64 probeId : probeIds) {
            const auto summary = uiReadDb->GetSeedProbeRunSummary(probeId);
            if (!summary.has_value()) {
                RunningProbeUpdateResult result;
                result.errorMessage = QStringLiteral("Seed probe run %1 was not found in UIRead").arg(probeId);
                return result;
            }
            updates.push_back({ probeId, QString::fromStdString(summary->status) });
        }
        RunningProbeUpdateResult result;
        result.ok = true;
        result.value = std::move(updates);
        return result;
    }));
}

void SeedProbeController::emitStateChanged()
{
    emit stateChanged();
}

void SeedProbeController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);

    state_.search = settings.value(kSearchKey, QString()).toString();
    state_.onlyDone = settings.value(kOnlyDoneKey, state_.onlyDone).toBool();
    state_.autoRefresh = settings.value(kAutoRefreshKey, state_.autoRefresh).toBool();
    state_.refreshSeconds = (std::max)(1, settings.value(kRefreshSecondsKey, state_.refreshSeconds).toInt());
    state_.pageLimit = (std::max)(1, settings.value(kPageLimitKey, state_.pageLimit).toInt());

    settings.endGroup();
}

void SeedProbeController::persistSettings() const
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.setValue(kSearchKey, state_.search);
    settings.setValue(kOnlyDoneKey, state_.onlyDone);
    settings.setValue(kAutoRefreshKey, state_.autoRefresh);
    settings.setValue(kRefreshSecondsKey, state_.refreshSeconds);
    settings.setValue(kPageLimitKey, state_.pageLimit);
    settings.endGroup();
}

void SeedProbeController::syncFetchStateFromView()
{
    fetchSearch_ = state_.search;
    fetchOnlyDone_ = state_.onlyDone;
    fetchPageLimit_ = state_.pageLimit;
}
