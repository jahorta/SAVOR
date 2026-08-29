#include "GUI/Widgets/VisualReplay/VisualWorkerDashboardWindow.h"

#include <QtCore/QtMath>
#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtGui/QPlatformSurfaceEvent>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

#include <algorithm>

VisualWorkerDashboardWindow::VisualWorkerDashboardWindow(QWidget* parent)
    : PersistentToolWindow(parent)
{
    setWindowTitle(QStringLiteral("Visual Workers"));
    resize(1180, 760);

    QVBoxLayout* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(12, 12, 12, 12);
    rootLayout->setSpacing(10);

    QLabel* titleLabel = new QLabel(QStringLiteral("Visual Workers"), this);
    titleLabel->setObjectName(QStringLiteral("panelTitle"));
    rootLayout->addWidget(titleLabel);

    scrollArea_ = new QScrollArea(this);
    scrollArea_->setWidgetResizable(true);
    scrollArea_->setFrameShape(QFrame::NoFrame);

    gridContainer_ = new QWidget(scrollArea_);
    gridLayout_ = new QGridLayout(gridContainer_);
    gridLayout_->setContentsMargins(0, 0, 0, 0);
    gridLayout_->setHorizontalSpacing(10);
    gridLayout_->setVerticalSpacing(10);
    scrollArea_->setWidget(gridContainer_);

    rootLayout->addWidget(scrollArea_, 1);
}

void VisualWorkerDashboardWindow::setWorkerCount(int count, bool allowShrink)
{
    const int clampedCount = std::max(0, count);
    if (allowShrink) {
        while (tiles_.size() > clampedCount) {
            Tile tile = tiles_.takeLast();
            if (tile.frame != nullptr) {
                tile.frame->deleteLater();
            }
        }
    }

    while (tiles_.size() < clampedCount) {
        tiles_.append(createTile(tiles_.size()));
    }

    rebuildGrid();
}

void VisualWorkerDashboardWindow::updateWorkerStatus(
    const WorkerSnapshot& snapshot)
{
    if (snapshot.worker_id >= static_cast<std::size_t>(tiles_.size()))
        return;
    Tile& tile = tiles_[static_cast<int>(snapshot.worker_id)];
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
    frame->setMinimumWidth(340);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

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

    QWidget* renderWidget = new QWidget(frame);
    renderWidget->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget->setAttribute(Qt::WA_PaintOnScreen, true);
    renderWidget->setAutoFillBackground(true);
    renderWidget->setMinimumSize(320, 180);
    renderWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    renderWidget->resize(320, 180);
    renderWidget->installEventFilter(this);
    (void)renderWidget->winId();

    QLabel* jobLabel = new QLabel(QStringLiteral("Job: --"), frame);
    jobLabel->setObjectName(QStringLiteral("panelBody"));

    layout->addLayout(headerLayout);
    layout->addWidget(renderWidget);
    layout->addWidget(jobLabel);

    tile.frame = frame;
    tile.renderWidget = renderWidget;
    tile.titleLabel = titleLabel;
    tile.stateLabel = stateLabel;
    tile.jobLabel = jobLabel;
    tile.workerIndex = workerIndex;
    tile.nativeHandle = renderWidget->effectiveWinId();
    tile.surfaceGeneration = 1;
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

void VisualWorkerDashboardWindow::resizeEvent(QResizeEvent* event)
{
    PersistentToolWindow::resizeEvent(event);
    rebuildGrid();
}

void VisualWorkerDashboardWindow::rebuildGrid()
{
    while (QLayoutItem* item = gridLayout_->takeAt(0)) {
        delete item;
    }

    const int columns = std::max(1, width() / 390);
    for (int i = 0; i < tiles_.size(); ++i) {
        const int row = i / columns;
        const int column = i % columns;
        gridLayout_->addWidget(tiles_[i].frame, row, column);
    }
    for (int column = 0; column < columns; ++column) {
        gridLayout_->setColumnStretch(column, 1);
    }
}
