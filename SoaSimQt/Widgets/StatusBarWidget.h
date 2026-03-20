#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtWidgets/QWidget>

class QHBoxLayout;
class QLabel;

struct StatusToast
{
    enum class Severity {
        Info,
        Success,
        Warn,
        Error
    };

    Severity severity = Severity::Info;
    QString message;
    QString details;
    int count = 1;
};

struct StatusBarSnapshot
{
    bool connected = false;
    QString envLabel = "prod";
    QDateTime lastRefresh;
    QString lastError;
    bool coordinatorRunning = false;
    int coordinatorWorkers = 0;
    QList<StatusToast> toasts;
};

class StatusBarWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StatusBarWidget(QWidget* parent = nullptr);

    void setSnapshot(const StatusBarSnapshot& snapshot);

private:
    QLabel* createBadge(const QString& text, const QString& variant);
    QLabel* createSeparator();
    void rebuildToasts();
    void updateConnectionBadge();
    void updateRefreshLabel();
    void updateCoordinatorBadge();

    StatusBarSnapshot snapshot_;

    QLabel* connectionBadge_ = nullptr;
    QLabel* envLabel_ = nullptr;
    QLabel* refreshLabel_ = nullptr;
    QLabel* coordinatorBadge_ = nullptr;
    QWidget* toastHost_ = nullptr;
    QHBoxLayout* toastLayout_ = nullptr;
};
