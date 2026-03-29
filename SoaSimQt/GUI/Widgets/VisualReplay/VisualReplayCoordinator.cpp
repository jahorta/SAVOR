#include "GUI/Widgets/VisualReplay/VisualReplayCoordinator.h"

#include <QtCore/QMetaObject>
#include <QtCore/QTimer>

#include "CoordinatorController.h"

namespace {
constexpr int kVisualLogPollIntervalMs = 250;
}

#include <windows.h>

#include <cstdlib>

VisualReplayCoordinator::VisualReplayCoordinator(CoordinatorController* controller, QObject* parent)
    : QObject(parent), controller_(controller)
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
    stopHostEventsListener();
    hostEventsPipeName_ = QStringLiteral("\\\\.\\pipe\\soasim_visual_host_events_%1")
        .arg(reinterpret_cast<qulonglong>(this));
    stopHostEvents_.store(false);
    hostEventsThread_ = std::thread(&VisualReplayCoordinator::hostEventsLoop, this);
}

void VisualReplayCoordinator::stopHostEventsListener()
{
    stopHostEvents_.store(true);
    if (hostEventsThread_.joinable()) {
        hostEventsThread_.join();
    }
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

std::string VisualReplayCoordinator::extractJsonStringField(const std::string& json, const std::string& key)
{
    const std::string token = "\"" + key + "\":\"";
    const size_t start = json.find(token);
    if (start == std::string::npos) return {};
    const size_t value_start = start + token.size();
    const size_t value_end = json.find('"', value_start);
    if (value_end == std::string::npos) return {};
    return json.substr(value_start, value_end - value_start);
}

std::string VisualReplayCoordinator::extractJsonObjectField(const std::string& json, const std::string& key)
{
    const std::string token = "\"" + key + "\":";
    const size_t start = json.find(token);
    if (start == std::string::npos) return {};
    size_t value_start = start + token.size();
    while (value_start < json.size() && (json[value_start] == ' ' || json[value_start] == '\t')) ++value_start;
    if (value_start >= json.size()) return {};
    if (json[value_start] == '{') {
        int depth = 0;
        for (size_t i = value_start; i < json.size(); ++i) {
            if (json[i] == '{') ++depth;
            else if (json[i] == '}') {
                --depth;
                if (depth == 0) return json.substr(value_start, i - value_start + 1);
            }
        }
        return {};
    }
    return json.substr(value_start);
}

bool VisualReplayCoordinator::extractIntField(const std::string& json, const std::string& key, int& out_value)
{
    const std::string token = "\"" + key + "\":";
    const size_t start = json.find(token);
    if (start == std::string::npos) return false;
    size_t p = start + token.size();
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t')) ++p;
    if (p >= json.size()) return false;
    char* end_ptr = nullptr;
    const long value = std::strtol(json.c_str() + p, &end_ptr, 10);
    if (end_ptr == json.c_str() + p) return false;
    out_value = static_cast<int>(value);
    return true;
}

void VisualReplayCoordinator::pollLiveLogLines()
{
    if (!controller_) {
        return;
    }
    const QStringList lines = controller_->pullVisualLiveLogLines();
    if (!lines.isEmpty()) {
        emit liveLogLinesReady(lines);
    }
}

void VisualReplayCoordinator::hostEventsLoop()
{
    if (hostEventsPipeName_.isEmpty()) return;
    const std::string pipe_name = hostEventsPipeName_.toStdString();
    HANDLE hPipe = CreateNamedPipeA(
        pipe_name.c_str(),
        PIPE_ACCESS_INBOUND,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_NOWAIT,
        1,
        0,
        4096,
        0,
        nullptr);
    if (hPipe == INVALID_HANDLE_VALUE) return;

    (void)ConnectNamedPipe(hPipe, nullptr);
    while (!stopHostEvents_.load()) {
        DWORD available_bytes = 0;
        if (!PeekNamedPipe(hPipe, nullptr, 0, nullptr, &available_bytes, nullptr) || available_bytes == 0) {
            Sleep(25);
            continue;
        }

        char buffer[4096]{};
        DWORD bytes_read = 0;
        if (!ReadFile(hPipe, buffer, sizeof(buffer) - 1, &bytes_read, nullptr) || bytes_read == 0) {
            Sleep(25);
            continue;
        }

        std::string line(buffer, buffer + bytes_read);
        const size_t nl = line.find_first_of("\r\n");
        if (nl != std::string::npos) line.resize(nl);

        const QString event_q = QString::fromStdString(extractJsonStringField(line, "event"));
        const QString args_q = QString::fromStdString(extractJsonObjectField(line, "args"));
        QMetaObject::invokeMethod(this, [this, event_q, args_q]() {
            emit hostEventReceived(event_q, args_q);
            if (event_q == QStringLiteral("Host_RequestRenderWindowSize")) {
                const std::string args_std = args_q.toStdString();
                int width = 0;
                int height = 0;
                if (extractIntField(args_std, "width", width) && extractIntField(args_std, "height", height)) {
                    emit renderSurfaceResizeRequested(width, height);
                }
            }
            }, Qt::QueuedConnection);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}
