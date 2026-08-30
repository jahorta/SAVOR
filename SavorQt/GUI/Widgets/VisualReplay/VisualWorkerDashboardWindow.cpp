#include "GUI/Widgets/VisualReplay/VisualWorkerDashboardWindow.h"

#include <QtCore/QtMath>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSettings>
#include <QtGui/QPlatformSurfaceEvent>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>

namespace {

constexpr auto kSettingsGroup = "VisualWorkerDashboard";
constexpr auto kColumnsKey = "columns";
constexpr int kDefaultColumns = 4;
constexpr int kMinimumColumns = 1;
constexpr int kMaximumColumns = 10;

class AspectRatioSurfaceContainer final : public QWidget
{
public:
    explicit AspectRatioSurfaceContainer(QWidget* parent = nullptr)
        : QWidget(parent)
    {
        setMinimumSize(160, 90);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setSurface(QWidget* surface)
    {
        surface_ = surface;
        updateSurfaceGeometry();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QWidget::resizeEvent(event);
        updateSurfaceGeometry();
    }

private:
    void updateSurfaceGeometry()
    {
        if (surface_ == nullptr || width() <= 0 || height() <= 0) {
            return;
        }

        int surfaceWidth = width();
        int surfaceHeight = surfaceWidth * 9 / 16;
        if (surfaceHeight > height()) {
            surfaceHeight = height();
            surfaceWidth = surfaceHeight * 16 / 9;
        }
        surface_->setGeometry(
            (width() - surfaceWidth) / 2,
            (height() - surfaceHeight) / 2,
            surfaceWidth,
            surfaceHeight);
    }

    QWidget* surface_ = nullptr;
};

} // namespace

VisualWorkerDashboardWindow::VisualWorkerDashboardWindow()
    : PersistentToolWindow(nullptr)
{
    setAttribute(Qt::WA_QuitOnClose, false);
    setWindowTitle(QStringLiteral("Visual Workers"));
    resize(1180, 760);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    QHBoxLayout* toolbarLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("Visual Workers"), this);
    titleLabel->setObjectName(QStringLiteral("panelTitle"));
    toolbarLayout->addWidget(titleLabel);
    toolbarLayout->addStretch();
    toolbarLayout->addWidget(new QLabel(QStringLiteral("Columns"), this));

    QSettings settings;
    settings.beginGroup(QString::fromLatin1(kSettingsGroup));
    configuredColumns_ = std::clamp(
        settings.value(QString::fromLatin1(kColumnsKey), kDefaultColumns).toInt(),
        kMinimumColumns,
        kMaximumColumns);
    settings.endGroup();

    columnsSpin_ = new QSpinBox(this);
    columnsSpin_->setRange(kMinimumColumns, kMaximumColumns);
    columnsSpin_->setValue(configuredColumns_);
    columnsSpin_->setToolTip(QStringLiteral(
        "Keep the visual worker grid at this number of columns."));
    toolbarLayout->addWidget(columnsSpin_);
    rootLayout->addLayout(toolbarLayout);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);

    gridContainer_ = new QWidget(scrollArea_);
    gridContainer_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    gridLayout_ = new QGridLayout(gridContainer_);
    gridLayout_->setContentsMargins(0, 0, 0, 0);
    gridLayout_->setHorizontalSpacing(10);
    gridLayout_->setVerticalSpacing(10);
    scrollArea_->setWidget(gridContainer_);

    rootLayout->addWidget(scrollArea_, 1);

    connect(
        columnsSpin_,
        qOverload<int>(&QSpinBox::valueChanged),
        this,
        [this](int columns) {
            configuredColumns_ = columns;
            QSettings settings;
            settings.beginGroup(QString::fromLatin1(kSettingsGroup));
            settings.setValue(QString::fromLatin1(kColumnsKey), columns);
            settings.endGroup();
            rebuildGrid();
        });
}

void VisualWorkerDashboardWindow::ensureWorkerCount(int count)
{
    const int clampedCount = std::max(0, count);
    while (tiles_.size() < clampedCount) {
        tiles_.append(createTile(tiles_.size()));
    }

    rebuildGrid();
}

void VisualWorkerDashboardWindow::releaseWorkersFrom(int firstWorkerIndex)
{
    const int retainedCount = std::clamp(
        firstWorkerIndex, 0, static_cast<int>(tiles_.size()));
    while (static_cast<int>(tiles_.size()) > retainedCount) {
        Tile tile = tiles_.takeLast();
        if (tile.nativeHandle != 0) {
            emit surfaceInvalidated(tile.workerIndex, tile.surfaceGeneration);
            tile.nativeHandle = 0;
        }
        if (tile.frame != nullptr) {
            tile.frame->deleteLater();
        }
    }
    rebuildGrid();
}

int VisualWorkerDashboardWindow::workerCount() const
{
    return tiles_.size();
}

void VisualWorkerDashboardWindow::updateWorkerStatus(
    const WorkerSnapshot& snapshot)
{
    const qint64 workerIndex = static_cast<qint64>(snapshot.worker_id);
    if (workerIndex < 0 ||
        workerIndex >= static_cast<qint64>(tiles_.size()))
        return;
    Tile& tile = tiles_[static_cast<int>(workerIndex)];
    tile.stateLabel->setText(stateText(snapshot.state));
    tile.jobLabel->setText(snapshot.job_id.has_value()
        ? QStringLiteral("Job: %1").arg(*snapshot.job_id)
        : QStringLiteral("Job: --"));
}

void VisualWorkerDashboardWindow::clearWorkerStatusesExcept(
    const QSet<int>& workerIds)
{
    for (int i = 0; i < tiles_.size(); ++i) {
        if (workerIds.contains(i))
            continue;
        tiles_[i].stateLabel->setText(QStringLiteral("Waiting"));
        tiles_[i].jobLabel->setText(QStringLiteral("Job: --"));
    }
}

QVector<VisualWorkerSurfaceBinding> VisualWorkerDashboardWindow::surfaceBindings() const
{
    QVector<VisualWorkerSurfaceBinding> bindings;
    bindings.reserve(tiles_.size());
    for (int i = 0; i < tiles_.size(); ++i) {
        const Tile& tile = tiles_[i];
        bindings.append(VisualWorkerSurfaceBinding{
            .workerIndex = i,
            .renderWidgetHandle = tile.nativeHandle,
            .surfaceGeneration = tile.surfaceGeneration,
            .ownerProcessId = static_cast<quint32>(
                QCoreApplication::applicationPid()),
            .hostEventsPipeName = QString{},
        });
    }
    return bindings;
}

QString VisualWorkerDashboardWindow::stateText(WorkerStateKind state)
{
    switch (state) {
    case WorkerStateKind::Spawning: return QStringLiteral("Spawning");
    case WorkerStateKind::Idle: return QStringLiteral("Idle");
    case WorkerStateKind::Leasing: return QStringLiteral("Leasing");
    case WorkerStateKind::Running: return QStringLiteral("Running");
    case WorkerStateKind::Renewing: return QStringLiteral("Renewing");
    case WorkerStateKind::Paused: return QStringLiteral("Paused");
    case WorkerStateKind::Draining: return QStringLiteral("Draining");
    case WorkerStateKind::Exiting: return QStringLiteral("Exiting");
    case WorkerStateKind::Stopping: return QStringLiteral("Stopping");
    case WorkerStateKind::Dead: return QStringLiteral("Dead");
    default: return QStringLiteral("Unknown");
    }
}

VisualWorkerDashboardWindow::Tile VisualWorkerDashboardWindow::createTile(int workerIndex)
{
    Tile tile{};

    QFrame* frame = new QFrame(gridContainer_);
    frame->setObjectName(QStringLiteral("coordinatorCard"));
    frame->setMinimumWidth(180);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    QVBoxLayout* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    QHBoxLayout* headerLayout = new QHBoxLayout();
    QLabel* titleLabel = new QLabel(QStringLiteral("Worker %1").arg(workerIndex), frame);
    titleLabel->setObjectName(QStringLiteral("panelTitle"));
    QLabel* stateLabel = new QLabel(QStringLiteral("Waiting"), frame);
    stateLabel->setObjectName(QStringLiteral("panelBody"));
    headerLayout->addWidget(titleLabel);
    headerLayout->addStretch();
    headerLayout->addWidget(stateLabel);

    auto* renderContainer = new AspectRatioSurfaceContainer(frame);
    QWidget* renderWidget = new QWidget(renderContainer);
    renderWidget->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget->setAttribute(Qt::WA_PaintOnScreen, true);
    renderWidget->setAutoFillBackground(true);
    renderWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    renderContainer->setSurface(renderWidget);
    renderWidget->installEventFilter(this);
    (void)renderWidget->winId();

    QLabel* jobLabel = new QLabel(QStringLiteral("Job: --"), frame);
    jobLabel->setObjectName(QStringLiteral("panelBody"));

    layout->addLayout(headerLayout);
    layout->addWidget(renderContainer, 1);
    layout->addWidget(jobLabel);

    tile.frame = frame;
    tile.renderContainer = renderContainer;
    tile.renderWidget = renderWidget;
    tile.titleLabel = titleLabel;
    tile.stateLabel = stateLabel;
    tile.jobLabel = jobLabel;
    tile.workerIndex = workerIndex;
    tile.nativeHandle = renderWidget->effectiveWinId();
    tile.surfaceGeneration = surfaceGenerations_.value(workerIndex, 0) + 1;
    surfaceGenerations_.insert(workerIndex, tile.surfaceGeneration);
    return tile;
}

bool VisualWorkerDashboardWindow::eventFilter(
    QObject* watched,
    QEvent* event)
{
    auto found = std::find_if(
        tiles_.begin(), tiles_.end(),
        [watched](const Tile& tile) { return tile.renderWidget == watched; });
    if (found == tiles_.end())
        return PersistentToolWindow::eventFilter(watched, event);

    if (event->type() == QEvent::PlatformSurface) {
        auto* surfaceEvent = static_cast<QPlatformSurfaceEvent*>(event);
        if (surfaceEvent->surfaceEventType() ==
            QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed &&
            found->nativeHandle != 0) {
            emit surfaceInvalidated(
                found->workerIndex, found->surfaceGeneration);
            found->nativeHandle = 0;
        }
    } else if (event->type() == QEvent::WinIdChange) {
        const quintptr newHandle = found->renderWidget->effectiveWinId();
        if (newHandle != 0 && newHandle != found->nativeHandle) {
            if (found->nativeHandle != 0) {
                emit surfaceInvalidated(
                    found->workerIndex, found->surfaceGeneration);
            }
            found->nativeHandle = newHandle;
            ++found->surfaceGeneration;
            surfaceGenerations_.insert(
                found->workerIndex, found->surfaceGeneration);
            emit surfaceChanged(VisualWorkerSurfaceBinding{
                .workerIndex = found->workerIndex,
                .renderWidgetHandle = found->nativeHandle,
                .surfaceGeneration = found->surfaceGeneration,
                .ownerProcessId = static_cast<quint32>(
                    QCoreApplication::applicationPid()),
                .hostEventsPipeName = QString{},
            });
        }
    }
    return PersistentToolWindow::eventFilter(watched, event);
}

void VisualWorkerDashboardWindow::rebuildGrid()
{
    while (QLayoutItem* item = gridLayout_->takeAt(0)) {
        delete item;
    }

    for (int row = 0; row < gridRowCount_; ++row) {
        gridLayout_->setRowStretch(row, 0);
    }
    for (int column = 0; column < gridColumnCount_; ++column) {
        gridLayout_->setColumnStretch(column, 0);
    }

    if (tiles_.isEmpty()) {
        gridRowCount_ = 0;
        gridColumnCount_ = 0;
        return;
    }

    const int tileCount = static_cast<int>(tiles_.size());
    const int columns = std::min(configuredColumns_, tileCount);
    const int rows = (tileCount + columns - 1) / columns;
    for (int i = 0; i < tiles_.size(); ++i) {
        const int row = i / columns;
        const int column = i % columns;
        gridLayout_->addWidget(tiles_[i].frame, row, column);
    }
    for (int column = 0; column < columns; ++column) {
        gridLayout_->setColumnStretch(column, 1);
    }
    for (int row = 0; row < rows; ++row) {
        gridLayout_->setRowStretch(row, 1);
    }
    gridRowCount_ = rows;
    gridColumnCount_ = columns;
}
