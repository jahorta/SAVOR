#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"

class CoordinatorController;

class QHBoxLayout;
class QLabel;
class QTimer;

struct StatusBarSnapshot
{
    bool connected = false;
    QString envLabel = "prod";
    QDateTime lastRefresh;
    QString lastError;
    bool coordinatorRunning = false;
    int coordinatorWorkers = 0;
};

class StatusBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget* parent = nullptr);

    static StatusBarSnapshot buildSnapshot(const CoordinatorController* controller, const QDateTime& lastRefresh);
    void setSnapshot(const StatusBarSnapshot& snapshot);
    void clearToasts();

public slots:
    void postToast(StatusToast toast);
    void postToast(StatusToast::Severity severity, const QString& message, const QString& details = QString(), int ttlMs = 4000);
    void setCoordinatorState(bool running, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private:
    enum class CoordinatorToastState {
        Stopped,
        Running,
        Paused
    };

    QLabel* createBadge(const QString& text, const QString& variant);
    QLabel* createSeparator();
    void rebuildToasts();
    void updateConnectionBadge();
    void updateRefreshLabel();
    void updateCoordinatorBadge();
    void pruneExpiredToasts();
    void scheduleToastExpiry();

    StatusBarSnapshot snapshot_;
    QList<StatusToast> activeToasts_;
    QString lastValidationMessage_;
    CoordinatorToastState lastCoordinatorToastState_ = CoordinatorToastState::Stopped;

    QLabel* connectionBadge_ = nullptr;
    QLabel* envLabel_ = nullptr;
    QLabel* refreshLabel_ = nullptr;
    QLabel* coordinatorBadge_ = nullptr;
    QWidget* toastHost_ = nullptr;
    QHBoxLayout* toastLayout_ = nullptr;
    QTimer* toastExpiryTimer_ = nullptr;
};
