#include "VisualWorkerDialog.h"

#include <QtCore/QTimer>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTextEdit>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>
#include <QtGui/QTextCursor>

namespace {
constexpr int kVisualLogPollIntervalMs = 250;
}

VisualWorkerDialog::VisualWorkerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Visual Worker"));
    resize(960, 640);

    QHBoxLayout* overallLayout = new QHBoxLayout(this);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel(QStringLiteral("Visual worker render surface"), this);
    layout->addWidget(label);

    renderWidget_ = new QWidget(this);
    renderWidget_->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget_->setMinimumSize(640, 360);
    renderWidget_->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget_->setAttribute(Qt::WA_PaintOnScreen, true);
    renderWidget_->setAutoFillBackground(true);
    layout->addWidget(renderWidget_, 1);

    replayDoneLabel_ = new QLabel(QStringLiteral("visual replay done"), this);
    replayDoneLabel_->setAlignment(Qt::AlignCenter);
    replayDoneLabel_->setVisible(false);
    layout->addWidget(replayDoneLabel_, 1);

    replayStateLabel_ = new QLabel(QStringLiteral("Replay state: idle"), this);
    layout->addWidget(replayStateLabel_);
    overallLayout->addLayout(layout);
    
    QVBoxLayout* loglayout = new QVBoxLayout(this);
    QLabel* logLabel = new QLabel(QStringLiteral("Live worker log (last 30 lines)"), this);
    loglayout->addWidget(logLabel);

    liveLogView_ = new QTextEdit(this);
    liveLogView_->setObjectName(QStringLiteral("visualWorkerLiveLogView"));
    liveLogView_->setReadOnly(true);
    liveLogView_->setLineWrapMode(QTextEdit::NoWrap);
    liveLogView_->setMinimumHeight(180);
    liveLogView_->setMinimumWidth(600);
    loglayout->addWidget(liveLogView_, 1);
    overallLayout->addLayout(loglayout);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    pauseButton_ = buttons->addButton(QStringLiteral("Pause Emulation"), QDialogButtonBox::ActionRole);
    stepVmButton_ = buttons->addButton(QStringLiteral("Step VM"), QDialogButtonBox::ActionRole);
    resumeButton_ = buttons->addButton(QStringLiteral("Resume Emulation"), QDialogButtonBox::ActionRole);
    connect(pauseButton_, &QPushButton::clicked, this, &VisualWorkerDialog::pauseRequested);
    connect(stepVmButton_, &QPushButton::clicked, this, &VisualWorkerDialog::vmStepRequested);
    connect(resumeButton_, &QPushButton::clicked, this, &VisualWorkerDialog::resumeRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    logPollTimer_ = new QTimer(this);
    logPollTimer_->setInterval(kVisualLogPollIntervalMs);
    connect(logPollTimer_, &QTimer::timeout, this, &VisualWorkerDialog::logPollRequested);
    connect(this, &QDialog::finished, this, [this](int) {
        stopLogPolling();
    });
    setReplayControlsEnabled(false);
}

VisualWorkerDialog::~VisualWorkerDialog()
{
    stopLogPolling();
}

quintptr VisualWorkerDialog::renderWidgetHandle() const
{
    return renderWidget_ ? renderWidget_->winId() : 0;
}

void VisualWorkerDialog::showRenderSurface()
{
    if (renderWidget_) {
        renderWidget_->setVisible(true);
    }
    if (replayDoneLabel_) {
        replayDoneLabel_->setVisible(false);
    }
}

void VisualWorkerDialog::showReplayDoneLabel()
{
    if (renderWidget_) {
        renderWidget_->setVisible(false);
    }
    if (replayDoneLabel_) {
        replayDoneLabel_->setVisible(true);
    }
}

void VisualWorkerDialog::startLogPolling()
{
    updateLiveLogLines(QStringList{});
    if (logPollTimer_) {
        logPollTimer_->start();
    }
}

void VisualWorkerDialog::stopLogPolling()
{
    if (logPollTimer_) {
        logPollTimer_->stop();
    }
}

void VisualWorkerDialog::updateLiveLogLines(const QStringList& lines)
{
    if (!liveLogView_) {
        return;
    }
    liveLogView_->setPlainText(lines.join(QLatin1Char('\n')));
    liveLogView_->moveCursor(QTextCursor::End);
}

void VisualWorkerDialog::appendHostEventLine(const QString& eventName, const QString& argsJson)
{
    if (!liveLogView_) {
        return;
    }
    const QString eventLine = argsJson.isEmpty()
        ? QStringLiteral("[host] %1").arg(eventName)
        : QStringLiteral("[host] %1 %2").arg(eventName, argsJson);
    liveLogView_->append(eventLine);
}

void VisualWorkerDialog::setReplayRuntimeStateText(const QString& text)
{
    if (!replayStateLabel_) {
        return;
    }
    replayStateLabel_->setText(QStringLiteral("Replay state: %1").arg(text.isEmpty() ? QStringLiteral("idle") : text));
}

void VisualWorkerDialog::setReplayControlsEnabled(bool enabled)
{
    if (pauseButton_) pauseButton_->setEnabled(enabled);
    if (stepVmButton_) stepVmButton_->setEnabled(enabled);
    if (resumeButton_) resumeButton_->setEnabled(enabled);
}

void VisualWorkerDialog::setRenderSurfaceSize(int widthPx, int heightPx)
{
    if (!renderWidget_) {
        return;
    }
    if (widthPx > 0 && heightPx > 0) {
        renderWidget_->setMinimumSize(widthPx, heightPx);
        renderWidget_->resize(widthPx, heightPx);
    }
}
