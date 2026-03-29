#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

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
    QString hostEventsPipeName() const;

signals:
    void hostEventReceived(const QString& eventName, const QString& argsJson);
    void renderSurfaceResizeRequested(int widthPx, int heightPx);

private:
    static std::string extractJsonStringField(const std::string& json, const std::string& key);
    static std::string extractJsonObjectField(const std::string& json, const std::string& key);
    static bool extractIntField(const std::string& json, const std::string& key, int& out_value);
    void hostEventsLoop();

    QString hostEventsPipeName_;
    std::thread hostEventsThread_;
    std::atomic<bool> stopHostEvents_{ false };
};
