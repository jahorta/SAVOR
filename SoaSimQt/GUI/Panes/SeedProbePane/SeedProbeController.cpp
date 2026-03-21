#include "SeedProbeController.h"

#include <QtCore/QTimer>

#include "Core/Input/InputPlanFmt.h"
#include "DB/DBCore/ObjectStore.h"
#include "DB/SavestateRepo.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_map>

using simcore::DescribeFrameCompact;
using simcore::ElementFamily;
using simcore::GCInputFrame;
using simcore::db::DataService;
using simcore::db::DeltaSeedRepo;
using simcore::db::DeltaSeedRow;
using simcore::db::ObjectStore;
using simcore::db::SavestateRepo;
using simcore::db::SeedProbeLite;
using simcore::db::SeedProbeRepo;
using simcore::db::SeedProbeRow;

namespace {
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
auto runDataServiceCall(AsyncCall&& asyncCall)
{
    return QtConcurrent::run([call = std::forward<AsyncCall>(asyncCall)]() mutable {
        return call();
    });
}

QString hex32(quint32 value)
{
    return QStringLiteral("0x%1").arg(value, 8, 16, QChar('0'));
}

QString neutralSeedText(const SeedProbeRow& probe)
{
    return probe.neutral_seed >= 0 ? hex32(static_cast<quint32>(probe.neutral_seed)) : QStringLiteral("n/a");
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

SeedProbeController::GridView buildGrid(const std::vector<DeltaSeedRow>& rows, ElementFamily family, int minNeg, int maxPos)
{
    SeedProbeController::GridView view;
    std::vector<int> xs;
    std::vector<int> ys;

    struct RawPoint { int x; int y; int delta; };
    std::vector<RawPoint> raw;
    raw.reserve(rows.size());

    for (const DeltaSeedRow& row : rows) {
        const ElementFamily fam = static_cast<ElementFamily>(row.input.get_family());
        if (fam != family) {
            continue;
        }

        RawPoint point{};
        point.delta = row.seed_delta;
        if (fam == ElementFamily::Main) {
            point.x = row.input.main_x;
            point.y = row.input.main_y;
        } else if (fam == ElementFamily::CStick) {
            point.x = row.input.c_x;
            point.y = row.input.c_y;
        } else if (fam == ElementFamily::Triggers) {
            point.x = row.input.trig_l;
            point.y = row.input.trig_r;
        } else {
            continue;
        }
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

QVector<int> buildLegend(const std::vector<DeltaSeedRow>& rows)
{
    QVector<int> values;
    values.reserve(static_cast<int>(rows.size()));
    for (const DeltaSeedRow& row : rows) {
        values.push_back(row.seed_delta);
    }
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

QVector<SeedProbeController::UniqueSeedRow> buildUniqueRows(const SeedProbeRow& probe, const std::vector<DeltaSeedRow>& uniqueRows)
{
    QVector<SeedProbeController::UniqueSeedRow> rows;
    if (probe.neutral_seed < 0) {
        rows.push_back({ QStringLiteral("Unique results require a neutral seed."), QStringLiteral("n/a") });
        return rows;
    }

    const quint32 neutral = static_cast<quint32>(probe.neutral_seed);
    DeltaSeedRow neutralRow{};
    neutralRow.input = GCInputFrame{};
    std::unordered_map<quint32, const DeltaSeedRow*> latestBySeed;
    for (const DeltaSeedRow& row : uniqueRows) {
        const quint32 seed = static_cast<quint32>(static_cast<quint64>(neutral) + static_cast<qint64>(row.seed_delta));
        auto it = latestBySeed.find(seed);
        if (it == latestBySeed.end() || row.id > it->second->id) {
            latestBySeed[seed] = &row;
        }
    }

    std::vector<std::pair<quint32, const DeltaSeedRow*>> ordered;
    ordered.reserve(latestBySeed.size() + 1);
    for (const auto& item : latestBySeed) {
        ordered.emplace_back(item.first, item.second);
    }
    ordered.emplace_back(neutral, &neutralRow);
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    rows.reserve(static_cast<int>(ordered.size()));
    for (const auto& item : ordered) {
        rows.push_back({ QString::fromStdString(DescribeFrameCompact(item.second->input)), hex32(item.first) });
    }
    return rows;
}

QString resolveSavestateLabel(qint64 probeId, qint64 savestateId)
{
    const auto savestateResult = SavestateRepo::GetByProbeId(probeId);
    if (savestateResult.ok && savestateResult.value.has_value()) {
        const auto objectResult = ObjectStore::Get(savestateResult.value->object_ref_id);
        if (objectResult.ok && !objectResult.value.filename.empty()) {
            return QString::fromStdString(objectResult.value.filename);
        }
    }
    return QString::number(savestateId);
}
}

SeedProbeController::SeedProbeController(QObject* parent)
    : QObject(parent)
{
    connect(&pageWatcher_, &QFutureWatcher<ListBundleResult>::finished, this, [this]() {
        try {
            const auto result = pageWatcher_.result();
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
                state_.errorMessage = QStringLiteral("Seed probes failed: %1").arg(QString::fromStdString(result.error.message));
            }
        } catch (...) {
            pageInFlight_ = false;
            state_.loadingList = false;
            state_.page = {};
            state_.probeRows.clear();
            state_.errorMessage = describeException("Seed probes failed");
        }
        emitStateChanged();
    });

    connect(&detailWatcher_, &QFutureWatcher<DetailBundleResult>::finished, this, [this]() {
        try {
            const auto result = detailWatcher_.result();
            detailInFlight_ = false;
            state_.loadingDetail = false;
            if (result.ok && detailRequestProbeId_ == state_.selectedProbeId) {
                state_.neutralSeedText = neutralSeedText(result.value.probe);
                state_.probeIdText = QString::number(result.value.probe.id);
                state_.statusText = QString::fromStdString(result.value.probe.status);
                state_.codecVersionText = QString::number(result.value.probe.codec_version);
                state_.savestateText = result.value.savestateText;
                state_.mainGrid = result.value.mainGrid;
                state_.cStickGrid = result.value.cStickGrid;
                state_.triggerGrid = result.value.triggerGrid;
                state_.legendDeltas = result.value.legendDeltas;
                state_.uniqueRows = result.value.uniqueRows;
                state_.errorMessage.clear();
            } else if (!result.ok) {
                state_.errorMessage = QStringLiteral("Seed probe detail failed: %1").arg(QString::fromStdString(result.error.message));
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
        if (canAutoRefresh()) {
            if (state_.selectedProbeId > 0 && state_.statusText.compare(QStringLiteral("running"), Qt::CaseInsensitive) == 0) {
                kickDetailFetch(state_.selectedProbeId, true);
            } else {
                requestRefresh();
            }
        }
    });
    refreshTimer_->start(state_.refreshSeconds * 1000);
}

const SeedProbeController::ViewState& SeedProbeController::viewState() const
{
    return state_;
}

void SeedProbeController::loadInitial()
{
    if (initialLoadStarted_) {
        return;
    }
    initialLoadStarted_ = true;
    kickPageFetch();
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
    emitStateChanged();
}

void SeedProbeController::setRefreshSeconds(int seconds)
{
    state_.refreshSeconds = seconds;
    refreshTimer_->start(seconds * 1000);
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
    refreshTimer_->start(state_.refreshSeconds * 1000);
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
    if (pageInFlight_) {
        return;
    }

    pageInFlight_ = true;
    state_.loadingList = true;
    state_.errorMessage.clear();

    PagedQuery<> query;
    query.before = before_;
    query.after = after_;
    query.limit = state_.pageLimit;

    pageWatcher_.setFuture(runDataServiceCall([search = state_.search, onlyDone = state_.onlyDone, query]() -> ListBundleResult {
        auto pageResult = DataService::FetchSeedProbesPage(query, search.toStdString(), onlyDone).get();
        if (!pageResult.ok) {
            return ListBundleResult::Err(pageResult.error);
        }

        ListBundle bundle{};
        bundle.page = pageResult.value;
        bundle.rows.reserve(static_cast<int>(bundle.page.items.size()));
        for (const SeedProbeLite& item : bundle.page.items) {
            ProbeSummary row{};
            row.probeId = item.id;
            row.status = QString::fromStdString(item.status);
            row.savestateId = item.savestate_id;
            const auto savestateResult = SavestateRepo::Get(item.savestate_id);
            if (savestateResult.ok && savestateResult.value.has_value()) {
                const auto objectResult = ObjectStore::Get(savestateResult.value->object_ref_id);
                if (objectResult.ok) {
                    row.filename = QString::fromStdString(objectResult.value.filename);
                }
            }
            bundle.rows.push_back(row);
        }
        return ListBundleResult::Ok(std::move(bundle));
    }));
    emitStateChanged();
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

    detailWatcher_.setFuture(runDataServiceCall([probeId]() -> DetailBundleResult {
        const auto probeResult = SeedProbeRepo::GetAsync(probeId).get();
        if (!probeResult.ok) {
            return DetailBundleResult::Err(probeResult.error);
        }
        const auto gridResult = DeltaSeedRepo::ListGridForProbeAsync(probeId).get();
        if (!gridResult.ok) {
            return DetailBundleResult::Err(gridResult.error);
        }
        const auto uniqueResult = DeltaSeedRepo::ListUniqueForProbeAsync(probeId).get();
        if (!uniqueResult.ok) {
            return DetailBundleResult::Err(uniqueResult.error);
        }

        int minNeg = -2;
        int maxPos = 32;
        for (const DeltaSeedRow& row : gridResult.value) {
            minNeg = std::min(minNeg, row.seed_delta);
            maxPos = std::max(maxPos, row.seed_delta);
        }

        DetailBundle bundle{};
        bundle.probe = probeResult.value;
        bundle.savestateText = resolveSavestateLabel(probeId, probeResult.value.savestate_id);
        bundle.mainGrid = buildGrid(gridResult.value, ElementFamily::Main, minNeg, maxPos);
        bundle.cStickGrid = buildGrid(gridResult.value, ElementFamily::CStick, minNeg, maxPos);
        bundle.triggerGrid = buildGrid(gridResult.value, ElementFamily::Triggers, minNeg, maxPos);
        bundle.legendDeltas = buildLegend(gridResult.value);
        bundle.uniqueRows = buildUniqueRows(probeResult.value, uniqueResult.value);
        return DetailBundleResult::Ok(std::move(bundle));
    }));
    emitStateChanged();
}

bool SeedProbeController::canAutoRefresh() const
{
    return state_.autoRefresh && !before_.has_value() && !after_.has_value() && !pageInFlight_ && !detailInFlight_;
}

void SeedProbeController::emitStateChanged()
{
    emit stateChanged();
}
