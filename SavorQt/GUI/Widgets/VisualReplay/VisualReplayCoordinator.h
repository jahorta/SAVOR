#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

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
    void pollLiveLogLines();

    QString hostEventsPipeName_;
    class QTimer* logPollTimer_ = nullptr;
};
