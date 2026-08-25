#include "VisualReplayWindow.h"

#include "GUI/Widgets/ScrollBarStabilizer.h"
#include "GUI/Widgets/VisualReplay/LiveLogFilterController.h"
#include "GUI/Widgets/VisualReplay/LiveLogListModel.h"
#include "GUI/Widgets/VisualReplay/VisualReplayCoordinator.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QMenu>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QToolButton>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtGui/QAction>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtCore/QEvent>

#include <algorithm>

namespace {
constexpr int kLevelAll = -1;

class LiveLogPointerMotionBlocker final : public QObject
{
public:
    explicit LiveLogPointerMotionBlocker(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        switch (event ? event->type() : QEvent::None) {
        case QEvent::Enter:
        case QEvent::Leave:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
        case QEvent::HoverLeave:
        case QEvent::MouseMove:
            return true;
        default:
            return QObject::eventFilter(watched, event);
        }
    }
};

template <typename Fn>
void runWithStabilizedScroll(QListView* view, Fn&& fn)
{
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(view);
    fn();
    restoreItemViewScrollSnapshot(view, scrollSnapshot);
}
}

VisualReplayWindow::VisualReplayWindow(QWidget* parent)
    : PersistentToolWindow(parent)
{
    setWindowTitle(QStringLiteral("Visual Worker"));
    resize(960, 640);

    QHBoxLayout* overallLayout = new QHBoxLayout(this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel(QStringLiteral("Visual worker render surface"), this);
    layout->addWidget(label);

    renderWidget_ = new QWidget(this);
    renderWidget_->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget_->setFixedSize(640, 360);
    renderWidget_->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget_->setAttribute(Qt::WA_PaintOnScreen, true);
    renderWidget_->setAutoFillBackground(true);
    layout->addWidget(renderWidget_);

    replayDoneLabel_ = new QLabel(QStringLiteral("visual replay done"), this);
    replayDoneLabel_->setAlignment(Qt::AlignCenter);
    replayDoneLabel_->setVisible(false);
    layout->addWidget(replayDoneLabel_, 1);

    replayStateLabel_ = new QLabel(QStringLiteral("Replay state: idle"), this);
    layout->addWidget(replayStateLabel_);
    overallLayout->addLayout(layout);

    QVBoxLayout* logLayout = new QVBoxLayout(this);
    QLabel* logLabel = new QLabel(QStringLiteral("Live worker log"), this);
    logLayout->addWidget(logLabel);

    QHBoxLayout* filterLayout = new QHBoxLayout();
    levelFilterCombo_ = new QComboBox(this);
    levelFilterCombo_->addItem(QStringLiteral("All"), kLevelAll);
    levelFilterCombo_->addItem(QStringLiteral("Debug"), 0);
    levelFilterCombo_->addItem(QStringLiteral("Trace"), 1);
    levelFilterCombo_->addItem(QStringLiteral("Info"), 2);
    levelFilterCombo_->addItem(QStringLiteral("Warn"), 3);
    levelFilterCombo_->addItem(QStringLiteral("Error"), 4);
    levelFilterCombo_->addItem(QStringLiteral("Fatal"), 5);
    levelFilterCombo_->setCurrentIndex(0);
    filterLayout->addWidget(new QLabel(QStringLiteral("Level:"), this));
    filterLayout->addWidget(levelFilterCombo_);

    sourceFilterButton_ = new QToolButton(this);
    sourceFilterButton_->setText(QStringLiteral("Sources"));
    sourceFilterButton_->setPopupMode(QToolButton::InstantPopup);
    filterLayout->addWidget(new QLabel(QStringLiteral("Source:"), this));
    filterLayout->addWidget(sourceFilterButton_);

    showFileCheck_ = new QCheckBox(QStringLiteral("Show file/source"), this);
    showFileCheck_->setChecked(true);
    filterLayout->addWidget(showFileCheck_);
    filterLayout->addStretch(1);

    logLayout->addLayout(filterLayout);

    liveLogView_ = new QListView(this);
    liveLogView_->setObjectName(QStringLiteral("visualWorkerLiveLogView"));
    liveLogView_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    liveLogView_->setSelectionMode(QAbstractItemView::NoSelection);
    liveLogView_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    liveLogView_->setUniformItemSizes(true);
    liveLogView_->setMinimumHeight(180);
    liveLogView_->setMinimumWidth(600);
    liveLogView_->setWordWrap(false);
    liveLogView_->setTextElideMode(Qt::ElideNone);
    liveLogView_->setGridSize(QSize(1, 12));
    liveLogView_->setStyleSheet(QStringLiteral("QListView#visualWorkerLiveLogView::item:hover { background: transparent; }"));
    liveLogView_->viewport()->installEventFilter(new LiveLogPointerMotionBlocker(liveLogView_->viewport()));
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    liveLogView_->setFont(mono);
    logLayout->addWidget(liveLogView_, 1);
    overallLayout->addLayout(logLayout);
    overallLayout->setStretch(1, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    pauseButton_ = buttons->addButton(QStringLiteral("Pause Emulation"), QDialogButtonBox::ActionRole);
    stepVmButton_ = buttons->addButton(QStringLiteral("Step VM"), QDialogButtonBox::ActionRole);
    resumeButton_ = buttons->addButton(QStringLiteral("Resume Emulation"), QDialogButtonBox::ActionRole);
    connect(pauseButton_, &QPushButton::clicked, this, &VisualReplayWindow::pauseRequested);
    connect(stepVmButton_, &QPushButton::clicked, this, &VisualReplayWindow::vmStepRequested);
    connect(resumeButton_, &QPushButton::clicked, this, &VisualReplayWindow::resumeRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);
    layout->addWidget(buttons);

    setReplayControlsEnabled(false);

    liveLogModel_ = new LiveLogListModel(this);
    liveLogController_ = new LiveLogFilterController(liveLogModel_);
    liveLogView_->setModel(liveLogModel_);
    liveLogModel_->onSourcesChanged = [this]() { refreshSourceMenu(); };
    updateLiveLogGridSize();

    connect(levelFilterCombo_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        if (liveLogController_) {
            runWithStabilizedScroll(liveLogView_, [this, idx]() {
                liveLogController_->setMinLevel(levelFilterCombo_->itemData(idx).toInt());
                updateLiveLogGridSize();
            });
        }
    });

    connect(showFileCheck_, &QCheckBox::toggled, this, [this](bool checked) {
        if (liveLogController_) {
            runWithStabilizedScroll(liveLogView_, [this, checked]() {
                liveLogController_->setShowSource(checked);
                updateLiveLogGridSize();
            });
        }
    });

    refreshSourceMenu();

    visualReplayCoordinator_ = new VisualReplayCoordinator(this);
    connect(visualReplayCoordinator_, &VisualReplayCoordinator::liveLogLinesRequested, this, &VisualReplayWindow::visualLiveLogLinesRequested);
    connect(visualReplayCoordinator_, &VisualReplayCoordinator::liveLogLinesReady, this, &VisualReplayWindow::appendLiveLogLines);
    connect(visualReplayCoordinator_, &VisualReplayCoordinator::hostEventReceived, this, &VisualReplayWindow::appendHostEventLine);
    connect(visualReplayCoordinator_, &VisualReplayCoordinator::renderSurfaceResizeRequested, this, &VisualReplayWindow::setRenderSurfaceSize);
}

VisualReplayWindow::~VisualReplayWindow() = default;

quintptr VisualReplayWindow::renderWidgetHandle() const
{
    return renderWidget_ ? renderWidget_->winId() : 0;
}

void VisualReplayWindow::showRenderSurface()
{
    if (renderWidget_) {
        renderWidget_->setVisible(true);
    }
    if (replayDoneLabel_) {
        replayDoneLabel_->setVisible(false);
    }
}

void VisualReplayWindow::showReplayDoneLabel()
{
    if (renderWidget_) {
        renderWidget_->setVisible(false);
    }
    if (replayDoneLabel_) {
        replayDoneLabel_->setVisible(true);
    }
}

void VisualReplayWindow::resetLiveLog()
{
    if (liveLogModel_) {
        runWithStabilizedScroll(liveLogView_, [this]() {
            liveLogModel_->clear();
            updateLiveLogGridSize();
        });
    }
    if (levelFilterCombo_) {
        levelFilterCombo_->setCurrentIndex(0);
    }
    refreshSourceMenu();
}

void VisualReplayWindow::updateLiveLogLines(const QStringList& lines)
{
    if (!liveLogModel_) {
        return;
    }
    runWithStabilizedScroll(liveLogView_, [this, &lines]() {
        liveLogModel_->clear();
        liveLogModel_->appendRawLines(lines);
        updateLiveLogGridSize();
    });
}

void VisualReplayWindow::appendLiveLogLines(const QStringList& lines)
{
    if (!liveLogModel_ || lines.isEmpty()) {
        return;
    }
    runWithStabilizedScroll(liveLogView_, [this, &lines]() {
        liveLogModel_->appendRawLines(lines);
        updateLiveLogGridSize();
    });
}

void VisualReplayWindow::appendHostEventLine(const QString& eventName, const QString& argsJson)
{
    const QString eventLine = argsJson.isEmpty()
        ? QStringLiteral("[host] %1").arg(eventName)
        : QStringLiteral("[host] %1 %2").arg(eventName, argsJson);
    appendLiveLogLines(QStringList{ eventLine });
}

void VisualReplayWindow::setReplayRuntimeStateText(const QString& text)
{
    if (!replayStateLabel_) {
        return;
    }
    replayStateLabel_->setText(QStringLiteral("Replay state: %1").arg(text.isEmpty() ? QStringLiteral("idle") : text));
}

void VisualReplayWindow::setReplayControlsEnabled(bool enabled)
{
    if (pauseButton_) pauseButton_->setEnabled(enabled);
    if (stepVmButton_) stepVmButton_->setEnabled(enabled);
    if (resumeButton_) resumeButton_->setEnabled(enabled);
}

void VisualReplayWindow::setRenderSurfaceSize(int widthPx, int heightPx)
{
    if (!renderWidget_) {
        return;
    }
    if (widthPx > 0 && heightPx > 0) {
        renderWidget_->setFixedSize(widthPx, heightPx);
        renderWidget_->resize(widthPx, heightPx);
    }
}

void VisualReplayWindow::startLiveLogStreaming()
{
    if (visualReplayCoordinator_) {
        visualReplayCoordinator_->startLiveLogStreaming();
    }
}

void VisualReplayWindow::stopLiveLogStreaming()
{
    if (visualReplayCoordinator_) {
        visualReplayCoordinator_->stopLiveLogStreaming();
    }
}

void VisualReplayWindow::startHostEventsListener()
{
    if (visualReplayCoordinator_) {
        visualReplayCoordinator_->startHostEventsListener();
    }
}

void VisualReplayWindow::stopHostEventsListener()
{
    if (visualReplayCoordinator_) {
        visualReplayCoordinator_->stopHostEventsListener();
    }
}

QString VisualReplayWindow::hostEventsPipeName() const
{
    return visualReplayCoordinator_ ? visualReplayCoordinator_->hostEventsPipeName() : QString{};
}

VisualReplayCoordinator* VisualReplayWindow::visualReplayCoordinator() const
{
    return visualReplayCoordinator_;
}

void VisualReplayWindow::refreshSourceMenu()
{
    if (!sourceFilterButton_ || !liveLogModel_ || !liveLogController_) {
        return;
    }

    if (QMenu* existingMenu = sourceFilterButton_->menu()) {
        existingMenu->deleteLater();
    }

    QMenu* menu = new QMenu(sourceFilterButton_);

    QAction* selectAllAction = menu->addAction(QStringLiteral("Select all"));
    QAction* clearAllAction = menu->addAction(QStringLiteral("Clear all"));
    menu->addSeparator();

    const QStringList knownSources = liveLogModel_->knownSources();
    const QSet<QString> selectedSources = liveLogModel_->selectedSources();

    for (const QString& source : knownSources) {
        QAction* action = menu->addAction(source);
        action->setCheckable(true);
        action->setChecked(selectedSources.contains(source));
    }

    connect(selectAllAction, &QAction::triggered, this, [this]() {
        if (liveLogController_) {
            runWithStabilizedScroll(liveLogView_, [this]() {
                liveLogController_->setSelectAllSources(true);
                updateLiveLogGridSize();
            });
        }
        refreshSourceMenu();
    });

    connect(clearAllAction, &QAction::triggered, this, [this]() {
        if (liveLogController_) {
            runWithStabilizedScroll(liveLogView_, [this]() {
                liveLogController_->setSelectAllSources(false);
                updateLiveLogGridSize();
            });
        }
        refreshSourceMenu();
    });

    const QList<QAction*> actions = menu->actions();
    for (int i = 0; i < actions.size(); ++i) {
        QAction* action = actions[i];
        if (i < 3) {
            continue;
        }
        connect(action, &QAction::toggled, this, [this, menu](bool) {
            QSet<QString> selected;
            const QList<QAction*> menuActions = menu->actions();
            for (int idx = 3; idx < menuActions.size(); ++idx) {
                QAction* menuAction = menuActions[idx];
                if (menuAction->isChecked()) {
                    selected.insert(menuAction->text());
                }
            }
            if (liveLogController_) {
                runWithStabilizedScroll(liveLogView_, [this, &selected]() {
                    liveLogController_->setSelectedSources(selected);
                    updateLiveLogGridSize();
                });
            }
        });
    }

    sourceFilterButton_->setMenu(menu);
}

void VisualReplayWindow::updateLiveLogGridSize()
{
    if (!liveLogView_ || !liveLogModel_) {
        return;
    }

    constexpr int kLiveLogRowHeightPx = 12;
    constexpr int kLiveLogHorizontalPaddingPx = 12;
    const QFontMetrics fontMetrics(liveLogView_->font());

    int widestTextPx = 0;
    const int rowCount = liveLogModel_->rowCount();
    for (int row = 0; row < rowCount; ++row) {
        const QModelIndex index = liveLogModel_->index(row, 0);
        const QString text = liveLogModel_->data(index, Qt::DisplayRole).toString();
        widestTextPx = std::max(widestTextPx, fontMetrics.horizontalAdvance(text));
    }

    const int targetWidth = std::max(1, widestTextPx + kLiveLogHorizontalPaddingPx);
    if (targetWidth == liveLogGridWidthPx_) {
        return;
    }

    liveLogGridWidthPx_ = targetWidth;
    liveLogView_->setGridSize(QSize(liveLogGridWidthPx_, kLiveLogRowHeightPx));
}
