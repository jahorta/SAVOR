#include "VisualWorkerDialog.h"

#include <QtCore/QTimer>
#include <QtWidgets/QDialogButtonBox>
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

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel(QStringLiteral("Visual worker render surface"), this);
    layout->addWidget(label);

    renderWidget_ = new QWidget(this);
    renderWidget_->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget_->setMinimumSize(640, 360);
    renderWidget_->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget_->setAutoFillBackground(true);
    layout->addWidget(renderWidget_, 1);

    replayDoneLabel_ = new QLabel(QStringLiteral("visual replay done"), this);
    replayDoneLabel_->setAlignment(Qt::AlignCenter);
    replayDoneLabel_->setVisible(false);
    layout->addWidget(replayDoneLabel_, 1);

    QLabel* logLabel = new QLabel(QStringLiteral("Live worker log (last 30 lines)"), this);
    layout->addWidget(logLabel);

    liveLogView_ = new QTextEdit(this);
    liveLogView_->setObjectName(QStringLiteral("visualWorkerLiveLogView"));
    liveLogView_->setReadOnly(true);
    liveLogView_->setLineWrapMode(QTextEdit::NoWrap);
    liveLogView_->setMinimumHeight(180);
    layout->addWidget(liveLogView_);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton* pauseButton = buttons->addButton(QStringLiteral("Pause Emulation"), QDialogButtonBox::ActionRole);
    QPushButton* stepVmButton = buttons->addButton(QStringLiteral("Step VM"), QDialogButtonBox::ActionRole);
    QPushButton* resumeButton = buttons->addButton(QStringLiteral("Resume Emulation"), QDialogButtonBox::ActionRole);
    connect(pauseButton, &QPushButton::clicked, this, &VisualWorkerDialog::pauseRequested);
    connect(stepVmButton, &QPushButton::clicked, this, &VisualWorkerDialog::vmStepRequested);
    connect(resumeButton, &QPushButton::clicked, this, &VisualWorkerDialog::resumeRequested);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    logPollTimer_ = new QTimer(this);
    logPollTimer_->setInterval(kVisualLogPollIntervalMs);
    connect(logPollTimer_, &QTimer::timeout, this, &VisualWorkerDialog::logPollRequested);
    connect(this, &QDialog::finished, this, [this](int) {
        stopLogPolling();
    });
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
