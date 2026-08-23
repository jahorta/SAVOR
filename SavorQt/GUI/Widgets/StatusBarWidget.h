#pragma once

#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QWidget>

#include "GUI/Common/StatusToast.h"
#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"

class QHBoxLayout;
class QLabel;
class QPushButton;
class QResizeEvent;
class QTimer;

struct StatusBarSnapshot
{
    bool connected = false;
    QString envLabel = "prod";
    QDateTime lastRefresh;
    QString lastError;
    CoordinatorLifecycleState coordinatorState =
        CoordinatorLifecycleState::Stopped;
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
    void setCoordinatorState(CoordinatorLifecycleState state, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private:
    struct ToastHistoryEntry
    {
        QDateTime createdAt;
        StatusToast::Severity severity = StatusToast::Severity::Info;
        QString message;
        QString details;
        int count = 1;
        int ttlMs = 4000;
    };

    enum class CoordinatorToastState {
        Stopped,
        Starting,
        Running,
        Paused,
        Stopping,
    };

    QLabel* createBadge(const QString& text, const QString& variant);
    QLabel* createSeparator();
    void rebuildToasts();
    void reconcileVisibleToastsWithWidth();
    bool canFitToast(const StatusToast& toast) const;
    int toastWidth(const StatusToast& toast) const;
    int availableToastWidth() const;
    void updateConnectionBadge();
    void updateRefreshLabel();
    void updateCoordinatorBadge();
    void pruneExpiredToasts();
    void scheduleToastExpiry();
    bool hasDuplicateMessage(const QString& message) const;
    QString formatHistoryLine(const ToastHistoryEntry& entry) const;
    void appendToastHistory(const StatusToast& toast);
    void ensureToastHistoryLogReady();
    void showToastHistoryDialog();
    QStringList buildHistoryLinesFromMemory() const;
    QStringList loadHistoryLinesFromFile() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

    StatusBarSnapshot snapshot_;
    QList<StatusToast> visibleToasts_;
    QList<StatusToast> queuedToasts_;
    QList<ToastHistoryEntry> toastHistory_;
    QString lastValidationMessage_;
    CoordinatorToastState lastCoordinatorToastState_ = CoordinatorToastState::Stopped;
    QString toastHistoryFilePath_;

    QLabel* connectionBadge_ = nullptr;
    QLabel* envLabel_ = nullptr;
    QLabel* refreshLabel_ = nullptr;
    QLabel* coordinatorBadge_ = nullptr;
    QPushButton* historyButton_ = nullptr;
    QWidget* toastHost_ = nullptr;
    QHBoxLayout* toastLayout_ = nullptr;
    QTimer* toastExpiryTimer_ = nullptr;
};
