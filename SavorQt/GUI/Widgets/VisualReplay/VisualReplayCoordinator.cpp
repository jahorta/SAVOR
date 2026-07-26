#include "GUI/Widgets/VisualReplay/VisualReplayCoordinator.h"

#include <QtCore/QTimer>

namespace {
constexpr int kVisualLogPollIntervalMs = 250;
}

VisualReplayCoordinator::VisualReplayCoordinator(QObject* parent)
    : QObject(parent)
{
    logPollTimer_ = new QTimer(this);
    logPollTimer_->setInterval(kVisualLogPollIntervalMs);
    connect(logPollTimer_, &QTimer::timeout, this, &VisualReplayCoordinator::pollLiveLogLines);
}

VisualReplayCoordinator::~VisualReplayCoordinator()
{
    stopHostEventsListener();
    stopLiveLogStreaming();
}

void VisualReplayCoordinator::startHostEventsListener()
{
    // Host events now travel through WRMS and ProcessWorker's typed callback.
    // Interactive visual-debug wiring remains unavailable until Slice 3.
    hostEventsPipeName_.clear();
}

void VisualReplayCoordinator::stopHostEventsListener()
{
    hostEventsPipeName_.clear();
}


void VisualReplayCoordinator::startLiveLogStreaming()
{
    pollLiveLogLines();
    if (logPollTimer_) {
        logPollTimer_->start();
    }
}

void VisualReplayCoordinator::stopLiveLogStreaming()
{
    if (logPollTimer_) {
        logPollTimer_->stop();
    }
}

QString VisualReplayCoordinator::hostEventsPipeName() const
{
    return hostEventsPipeName_;
}

void VisualReplayCoordinator::pollLiveLogLines()
{
    emit liveLogLinesRequested();
}

void VisualReplayCoordinator::setLiveLogLines(const QStringList& lines)
{
    if (!lines.isEmpty()) {
        emit liveLogLinesReady(lines);
    }
}
