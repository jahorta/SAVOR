#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <atomic>
#include <string>
#include <thread>

class VisualReplayCoordinator final : public QObject
{
    Q_OBJECT

public:
    explicit VisualReplayCoordinator(QObject* parent = nullptr);
    ~VisualReplayCoordinator() override;

    void startHostEventsListener();
    void stopHostEventsListener();
    void startLiveLogStreaming();
    void stopLiveLogStreaming();
    QString hostEventsPipeName() const;

signals:
    void liveLogLinesRequested();
    void hostEventReceived(const QString& eventName, const QString& argsJson);
    void renderSurfaceResizeRequested(int widthPx, int heightPx);
    void liveLogLinesReady(const QStringList& lines);

public slots:
    void setLiveLogLines(const QStringList& lines);

private:
    static std::string extractJsonStringField(const std::string& json, const std::string& key);
    static std::string extractJsonObjectField(const std::string& json, const std::string& key);
    static bool extractIntField(const std::string& json, const std::string& key, int& out_value);
    void hostEventsLoop();
    void pollLiveLogLines();

    QString hostEventsPipeName_;
    std::thread hostEventsThread_;
    std::atomic<bool> stopHostEvents_{ false };
    class QTimer* logPollTimer_ = nullptr;
};
