#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

class CoordinatorController;

#include <atomic>
#include <string>
#include <thread>

class VisualReplayCoordinator final : public QObject
{
    Q_OBJECT

public:
    explicit VisualReplayCoordinator(CoordinatorController* controller, QObject* parent = nullptr);
    ~VisualReplayCoordinator() override;

    void startHostEventsListener();
    void stopHostEventsListener();
    void startLiveLogStreaming();
    void stopLiveLogStreaming();
    QString hostEventsPipeName() const;

signals:
    void hostEventReceived(const QString& eventName, const QString& argsJson);
    void renderSurfaceResizeRequested(int widthPx, int heightPx);
    void liveLogLinesReady(const QStringList& lines);

private:
    static std::string extractJsonStringField(const std::string& json, const std::string& key);
    static std::string extractJsonObjectField(const std::string& json, const std::string& key);
    static bool extractIntField(const std::string& json, const std::string& key, int& out_value);
    void hostEventsLoop();
    void pollLiveLogLines();

    CoordinatorController* controller_ = nullptr;
    QString hostEventsPipeName_;
    std::thread hostEventsThread_;
    std::atomic<bool> stopHostEvents_{ false };
    class QTimer* logPollTimer_ = nullptr;
};
