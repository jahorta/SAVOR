#include "GUI/Widgets/StatusBarWidget.h"

#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"

#include <QtCore/QTimer>
#include <QtWidgets/QHBoxLayout>
#include <utility>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>

namespace {
constexpr int kDefaultToastTtlMs = 4000;
constexpr int kValidationToastTtlMs = 5000;
constexpr int kDuplicateWindowMs = 1500;
constexpr int kMaxStoredToasts = 6;

QString toastVariant(StatusToast::Severity severity)
{
    switch (severity) {
    case StatusToast::Severity::Info:
        return "info";
    case StatusToast::Severity::Success:
        return "success";
    case StatusToast::Severity::Warn:
        return "warn";
    case StatusToast::Severity::Error:
        return "error";
    }

    return "info";
}

QString buildToastText(const StatusToast& toast)
{
    if (toast.count > 1) {
        return QStringLiteral("%1 ×%2").arg(toast.message).arg(toast.count);
    }

    return toast.message;
}
} // namespace

StatusBarSnapshot StatusBarWidget::buildSnapshot(const CoordinatorController* controller, const QDateTime& lastRefresh)
{
    StatusBarSnapshot snapshot;
    snapshot.connected = true;
    snapshot.envLabel = QStringLiteral("prod");
    snapshot.lastRefresh = lastRefresh;
    snapshot.coordinatorRunning = controller && controller->isRunning();
    snapshot.coordinatorWorkers = controller ? controller->activeWorkers() : 0;

    if (controller) {
        snapshot.lastError = controller->validationMessage();
    }

    return snapshot;
}

StatusBarWidget::StatusBarWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("statusBarWidget");
    setMinimumHeight(34);

    toastExpiryTimer_ = new QTimer(this);
    toastExpiryTimer_->setSingleShot(true);
    connect(toastExpiryTimer_, &QTimer::timeout, this, [this]() {
        pruneExpiredToasts();
        rebuildToasts();
    });

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(10);

    connectionBadge_ = createBadge(QString(), QStringLiteral("connected"));
    layout->addWidget(connectionBadge_);
    layout->addWidget(createSeparator());

    envLabel_ = new QLabel(this);
    envLabel_->setObjectName("statusText");
    layout->addWidget(envLabel_);
    layout->addWidget(createSeparator());

    refreshLabel_ = new QLabel(this);
    refreshLabel_->setObjectName("statusText");
    layout->addWidget(refreshLabel_);

    coordinatorBadge_ = createBadge(QString(), QStringLiteral("stopped"));
    layout->addWidget(coordinatorBadge_);
    layout->addStretch();

    toastHost_ = new QWidget(this);
    toastLayout_ = new QHBoxLayout(toastHost_);
    toastLayout_->setContentsMargins(0, 0, 0, 0);
    toastLayout_->setSpacing(8);
    layout->addWidget(toastHost_);

    setSnapshot(StatusBarSnapshot{});
}

void StatusBarWidget::setSnapshot(const StatusBarSnapshot& snapshot)
{
    snapshot_ = snapshot;

    envLabel_->setText(QStringLiteral("env: %1").arg(snapshot_.envLabel));

    updateConnectionBadge();
    updateRefreshLabel();
    updateCoordinatorBadge();
    pruneExpiredToasts();
    rebuildToasts();
}

void StatusBarWidget::postToast(StatusToast toast)
{
    if (toast.message.isEmpty()) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (!toast.createdAt.isValid()) {
        toast.createdAt = now;
    }
    if (toast.ttlMs <= 0) {
        toast.ttlMs = kDefaultToastTtlMs;
    }
    if (toast.count < 1) {
        toast.count = 1;
    }

    pruneExpiredToasts();

    for (StatusToast& activeToast : activeToasts_) {
        if (activeToast.severity != toast.severity || activeToast.message != toast.message) {
            continue;
        }

        const qint64 ageMs = activeToast.createdAt.msecsTo(now);
        if (ageMs <= kDuplicateWindowMs) {
            activeToast.count += toast.count;
            activeToast.createdAt = now;
            activeToast.ttlMs = qMax(activeToast.ttlMs, toast.ttlMs);
            if (!toast.details.isEmpty()) {
                activeToast.details = toast.details;
            }
            rebuildToasts();
            return;
        }
    }

    activeToasts_.append(std::move(toast));
    while (activeToasts_.size() > kMaxStoredToasts) {
        activeToasts_.removeFirst();
    }

    rebuildToasts();
}

void StatusBarWidget::postToast(StatusToast::Severity severity, const QString& message, const QString& details, int ttlMs)
{
    StatusToast toast;
    toast.severity = severity;
    toast.message = message;
    toast.details = details;
    toast.ttlMs = ttlMs;
    postToast(std::move(toast));
}

void StatusBarWidget::clearToasts()
{
    if (activeToasts_.isEmpty()) {
        return;
    }

    activeToasts_.clear();
    rebuildToasts();
}

void StatusBarWidget::setCoordinatorState(bool running, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage)
{
    if (!validationMessage.isEmpty() && validationMessage != lastValidationMessage_) {
        postToast(
            StatusToast::Severity::Warn,
            QStringLiteral("Coordinator configuration incomplete"),
            validationMessage,
            kValidationToastTtlMs);
    }
    lastValidationMessage_ = validationMessage;

    CoordinatorToastState coordinatorState = CoordinatorToastState::Stopped;
    if (running) {
        coordinatorState = paused
            ? CoordinatorToastState::Paused
            : CoordinatorToastState::Running;
    }

    if (coordinatorState != lastCoordinatorToastState_) {
        switch (coordinatorState) {
        case CoordinatorToastState::Stopped:
            postToast(
                StatusToast::Severity::Info,
                QStringLiteral("Coordinator stopped"),
                QStringLiteral("Workers are no longer processing jobs."));
            break;
        case CoordinatorToastState::Running:
            postToast(
                StatusToast::Severity::Success,
                QStringLiteral("Coordinator running"),
                QStringLiteral("Target workers: %1 • Active workers: %2")
                    .arg(targetWorkers)
                    .arg(activeWorkers));
            break;
        case CoordinatorToastState::Paused:
            postToast(
                StatusToast::Severity::Warn,
                QStringLiteral("Coordinator paused"),
                QStringLiteral("Target workers: %1 • Active workers: %2")
                    .arg(targetWorkers)
                    .arg(activeWorkers),
                kValidationToastTtlMs);
            break;
        }
    }

    lastCoordinatorToastState_ = coordinatorState;
}

QLabel* StatusBarWidget::createBadge(const QString& text, const QString& variant)
{
    auto* badge = new QLabel(text, this);
    badge->setObjectName("statusBadge");
    badge->setProperty("variant", variant);
    badge->setAlignment(Qt::AlignCenter);
    badge->setMargin(6);
    return badge;
}

QLabel* StatusBarWidget::createSeparator()
{
    auto* label = new QLabel(QStringLiteral("|"), this);
    label->setObjectName("statusSeparator");
    return label;
}

void StatusBarWidget::rebuildToasts()
{
    while (toastLayout_->count() > 0) {
        QLayoutItem* item = toastLayout_->takeAt(0);
        if (QWidget* widget = item->widget()) {
            widget->deleteLater();
        }
        delete item;
    }

    for (const StatusToast& toast : activeToasts_) {
        QLabel* badge = createBadge(buildToastText(toast), toastVariant(toast.severity));
        if (!toast.details.isEmpty()) {
            badge->setToolTip(toast.details);
        }
        toastLayout_->addWidget(badge);
    }

    toastHost_->setVisible(!activeToasts_.isEmpty());
    scheduleToastExpiry();
}

void StatusBarWidget::updateConnectionBadge()
{
    const QString variant = snapshot_.connected ? QStringLiteral("connected") : QStringLiteral("disconnected");
    connectionBadge_->setProperty("variant", variant);
    connectionBadge_->setText(snapshot_.connected ? QStringLiteral("Connected") : QStringLiteral("Disconnected"));
    connectionBadge_->setToolTip(snapshot_.connected ? QString() : snapshot_.lastError);
    style()->unpolish(connectionBadge_);
    style()->polish(connectionBadge_);
}

void StatusBarWidget::updateRefreshLabel()
{
    if (!snapshot_.lastRefresh.isValid()) {
        refreshLabel_->setText(QStringLiteral("Last refresh: --"));
        return;
    }

    refreshLabel_->setText(QStringLiteral("Last refresh: %1").arg(snapshot_.lastRefresh.toString("hh:mm:ss AP")));
}

void StatusBarWidget::updateCoordinatorBadge()
{
    const QString variant = snapshot_.coordinatorRunning ? QStringLiteral("coordinator-running") : QStringLiteral("coordinator-stopped");
    coordinatorBadge_->setProperty("variant", variant);

    if (snapshot_.coordinatorRunning) {
        coordinatorBadge_->setText(QStringLiteral("Coordinator: %1").arg(snapshot_.coordinatorWorkers));
    } else {
        coordinatorBadge_->setText(QStringLiteral("Coordinator: Stopped"));
    }

    style()->unpolish(coordinatorBadge_);
    style()->polish(coordinatorBadge_);
}

void StatusBarWidget::pruneExpiredToasts()
{
    if (activeToasts_.isEmpty()) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = activeToasts_.begin(); it != activeToasts_.end();) {
        const qint64 ageMs = it->createdAt.msecsTo(now);
        if (ageMs > it->ttlMs) {
            it = activeToasts_.erase(it);
            continue;
        }
        ++it;
    }
}

void StatusBarWidget::scheduleToastExpiry()
{
    if (activeToasts_.isEmpty()) {
        toastExpiryTimer_->stop();
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    int nextExpiryMs = 0;
    bool foundExpiry = false;
    for (const StatusToast& toast : activeToasts_) {
        const qint64 ageMs = toast.createdAt.msecsTo(now);
        const int remainingMs = qMax(0, toast.ttlMs - static_cast<int>(ageMs));
        if (!foundExpiry || remainingMs < nextExpiryMs) {
            nextExpiryMs = remainingMs;
            foundExpiry = true;
        }
    }

    toastExpiryTimer_->start(qMax(1, nextExpiryMs));
}
