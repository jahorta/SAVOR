#include "GUI/Widgets/StatusBarWidget.h"

#include "GUI/Panes/CoordinatorPane/CoordinatorController.h"
#include "GUI/Widgets/ToastHistoryDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QStandardPaths>
#include <QtCore/QTextStream>
#include <QtCore/QTimer>
#include <QtGui/QResizeEvent>
#include <QtWidgets/QHBoxLayout>
#include <utility>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSizePolicy>
#include <QtWidgets/QStyle>

namespace {
constexpr int kDefaultToastTtlMs = 4000;
constexpr int kValidationToastTtlMs = 5000;
constexpr int kMinValidToastHostWidthPx = 80;
constexpr int kFallbackToastWidthFloorPx = 240;
constexpr int kToastHistoryMemoryCap = 1000;

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
    setMinimumHeight(40);

    toastExpiryTimer_ = new QTimer(this);
    toastExpiryTimer_->setSingleShot(true);
    connect(toastExpiryTimer_, &QTimer::timeout, this, [this]() {
        pruneExpiredToasts();
        reconcileVisibleToastsWithWidth();
        rebuildToasts();
    });

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 6, 12, 6);
    layout->setSpacing(10);

    auto* leftHost = new QWidget(this);
    leftHost->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
    auto* leftLayout = new QHBoxLayout(leftHost);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(10);

    historyButton_ = new QPushButton(QStringLiteral("H"), this);
    historyButton_->setObjectName("statusHistoryButton");
    historyButton_->setFixedWidth(24);
    historyButton_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    historyButton_->setToolTip(QStringLiteral("Open toast history"));
    connect(historyButton_, &QPushButton::clicked, this, [this]() { showToastHistoryDialog(); });
    leftLayout->addWidget(historyButton_);
    leftLayout->addWidget(createSeparator());

    connectionBadge_ = createBadge(QString(), QStringLiteral("connected"));
    leftLayout->addWidget(connectionBadge_);
    leftLayout->addWidget(createSeparator());

    envLabel_ = new QLabel(this);
    envLabel_->setObjectName("statusText");
    leftLayout->addWidget(envLabel_);
    leftLayout->addWidget(createSeparator());

    refreshLabel_ = new QLabel(this);
    refreshLabel_->setObjectName("statusText");
    leftLayout->addWidget(refreshLabel_);

    coordinatorBadge_ = createBadge(QString(), QStringLiteral("stopped"));
    leftLayout->addWidget(coordinatorBadge_);

    layout->addWidget(leftHost, 0);

    toastHost_ = new QWidget(this);
    toastHost_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    toastLayout_ = new QHBoxLayout(toastHost_);
    toastLayout_->setContentsMargins(0, 0, 0, 0);
    toastLayout_->setSpacing(8);
    layout->addWidget(toastHost_, 1);

    setSnapshot(StatusBarSnapshot{});
    ensureToastHistoryLogReady();
}

void StatusBarWidget::setSnapshot(const StatusBarSnapshot& snapshot)
{
    snapshot_ = snapshot;

    envLabel_->setText(QStringLiteral("env: %1").arg(snapshot_.envLabel));

    updateConnectionBadge();
    updateRefreshLabel();
    updateCoordinatorBadge();
    pruneExpiredToasts();
    reconcileVisibleToastsWithWidth();
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
    appendToastHistory(toast);

    pruneExpiredToasts();
    if (hasDuplicateMessage(toast.message)) {
        reconcileVisibleToastsWithWidth();
        rebuildToasts();
        return;
    }

    queuedToasts_.append(std::move(toast));
    reconcileVisibleToastsWithWidth();
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
    if (visibleToasts_.isEmpty() && queuedToasts_.isEmpty()) {
        return;
    }

    visibleToasts_.clear();
    queuedToasts_.clear();
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

    for (const StatusToast& toast : visibleToasts_) {
        QLabel* badge = createBadge(buildToastText(toast), toastVariant(toast.severity));
        badge->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
        if (!toast.details.isEmpty()) {
            badge->setToolTip(toast.details);
        }
        toastLayout_->addWidget(badge);
    }

    toastLayout_->addStretch(1);
    toastHost_->setVisible(true);
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
    if (visibleToasts_.isEmpty() && queuedToasts_.isEmpty()) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (auto it = visibleToasts_.begin(); it != visibleToasts_.end();) {
        const qint64 ageMs = it->createdAt.msecsTo(now);
        if (ageMs > it->ttlMs) {
            it = visibleToasts_.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = queuedToasts_.begin(); it != queuedToasts_.end();) {
        const qint64 ageMs = it->createdAt.msecsTo(now);
        if (ageMs > it->ttlMs) {
            it = queuedToasts_.erase(it);
            continue;
        }
        ++it;
    }
}

void StatusBarWidget::scheduleToastExpiry()
{
    if (visibleToasts_.isEmpty() && queuedToasts_.isEmpty()) {
        toastExpiryTimer_->stop();
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    int nextExpiryMs = 0;
    bool foundExpiry = false;
    for (const StatusToast& toast : visibleToasts_) {
        const qint64 ageMs = toast.createdAt.msecsTo(now);
        const int remainingMs = qMax(0, toast.ttlMs - static_cast<int>(ageMs));
        if (!foundExpiry || remainingMs < nextExpiryMs) {
            nextExpiryMs = remainingMs;
            foundExpiry = true;
        }
    }
    for (const StatusToast& toast : queuedToasts_) {
        const qint64 ageMs = toast.createdAt.msecsTo(now);
        const int remainingMs = qMax(0, toast.ttlMs - static_cast<int>(ageMs));
        if (!foundExpiry || remainingMs < nextExpiryMs) {
            nextExpiryMs = remainingMs;
            foundExpiry = true;
        }
    }

    toastExpiryTimer_->start(qMax(1, nextExpiryMs));
}

void StatusBarWidget::reconcileVisibleToastsWithWidth()
{
    const int availableWidth = availableToastWidth();
    while (!queuedToasts_.isEmpty() && canFitToast(queuedToasts_.front())) {
        visibleToasts_.append(queuedToasts_.takeFirst());
    }

    // Keep one toast visible on cramped layouts so newly queued messages still surface.
    if (visibleToasts_.isEmpty() && !queuedToasts_.isEmpty() && availableWidth > 0) {
        visibleToasts_.append(queuedToasts_.takeFirst());
    }

    while (!visibleToasts_.isEmpty()) {
        int totalWidth = 0;
        for (int i = 0; i < visibleToasts_.size(); ++i) {
            totalWidth += toastWidth(visibleToasts_.at(i));
            if (i > 0) {
                totalWidth += toastLayout_->spacing();
            }
        }

        if (totalWidth <= availableWidth) {
            break;
        }

        if (visibleToasts_.size() == 1) {
            break;
        }

        queuedToasts_.prepend(visibleToasts_.takeLast());
    }
}

bool StatusBarWidget::canFitToast(const StatusToast& toast) const
{
    const int availableWidth = availableToastWidth();
    int requiredWidth = toastWidth(toast);
    if (!visibleToasts_.isEmpty()) {
        requiredWidth += toastLayout_->spacing();
    }

    for (const StatusToast& visibleToast : visibleToasts_) {
        requiredWidth += toastWidth(visibleToast);
    }

    return requiredWidth <= availableWidth;
}

int StatusBarWidget::toastWidth(const StatusToast& toast) const
{
    QLabel probe(buildToastText(toast));
    probe.setObjectName("statusBadge");
    probe.setProperty("variant", toastVariant(toast.severity));
    probe.setAlignment(Qt::AlignCenter);
    probe.setMargin(6);
    probe.ensurePolished();
    return probe.sizeHint().width();
}

int StatusBarWidget::availableToastWidth() const
{
    const int hostWidth = toastHost_->contentsRect().width();
    if (hostWidth >= kMinValidToastHostWidthPx) {
        return hostWidth;
    }

    const int fallbackWidth = qMax(kFallbackToastWidthFloorPx, contentsRect().width() / 3);
    return qMax(0, fallbackWidth);
}

bool StatusBarWidget::hasDuplicateMessage(const QString& message) const
{
    for (const StatusToast& toast : visibleToasts_) {
        if (toast.message == message) {
            return true;
        }
    }

    for (const StatusToast& toast : queuedToasts_) {
        if (toast.message == message) {
            return true;
        }
    }

    return false;
}

QString StatusBarWidget::formatHistoryLine(const ToastHistoryEntry& entry) const
{
    const QString timestamp = entry.createdAt.isValid()
        ? entry.createdAt.toString(Qt::ISODateWithMs)
        : QStringLiteral("unknown-time");
    const QString severity = toastVariant(entry.severity).toUpper();
    const QString detailsSuffix = entry.details.isEmpty() ? QString() : QStringLiteral(" | details=%1").arg(entry.details);
    return QStringLiteral("[%1] [%2] %3 | count=%4 | ttlMs=%5%6")
        .arg(timestamp)
        .arg(severity)
        .arg(entry.message)
        .arg(entry.count)
        .arg(entry.ttlMs)
        .arg(detailsSuffix);
}

void StatusBarWidget::appendToastHistory(const StatusToast& toast)
{
    ToastHistoryEntry entry;
    entry.createdAt = toast.createdAt;
    entry.severity = toast.severity;
    entry.message = toast.message;
    entry.details = toast.details;
    entry.count = toast.count;
    entry.ttlMs = toast.ttlMs;
    toastHistory_.append(entry);
    while (toastHistory_.size() > kToastHistoryMemoryCap) {
        toastHistory_.removeFirst();
    }

    ensureToastHistoryLogReady();
    if (toastHistoryFilePath_.isEmpty()) {
        return;
    }

    QFile file(toastHistoryFilePath_);
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << formatHistoryLine(entry) << '\n';
    stream.flush();
}

void StatusBarWidget::ensureToastHistoryLogReady()
{
    if (!toastHistoryFilePath_.isEmpty()) {
        return;
    }

    const QString baseDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (baseDir.isEmpty()) {
        return;
    }

    const QString historyDir = QDir(baseDir).filePath(QStringLiteral("toast-history"));
    QDir dir;
    if (!dir.mkpath(historyDir)) {
        return;
    }

    const QString filename = QStringLiteral("toast-history-%1.log")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
    toastHistoryFilePath_ = QDir(historyDir).filePath(filename);
}

QStringList StatusBarWidget::buildHistoryLinesFromMemory() const
{
    QStringList lines;
    lines.reserve(toastHistory_.size());
    for (const ToastHistoryEntry& entry : toastHistory_) {
        lines.append(formatHistoryLine(entry));
    }
    return lines;
}

QStringList StatusBarWidget::loadHistoryLinesFromFile() const
{
    if (toastHistoryFilePath_.isEmpty()) {
        return { QStringLiteral("History file is unavailable for this run.") };
    }

    QFile file(toastHistoryFilePath_);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return { QStringLiteral("Failed to open history file: %1").arg(toastHistoryFilePath_) };
    }

    QTextStream stream(&file);
    QStringList lines;
    while (!stream.atEnd()) {
        lines.append(stream.readLine());
    }
    return lines;
}

void StatusBarWidget::showToastHistoryDialog()
{
    ToastHistoryDialog dialog(
        toastHistoryFilePath_,
        [this]() { return loadHistoryLinesFromFile(); },
        this);
    dialog.setHistoryLines(buildHistoryLinesFromMemory());
    dialog.exec();
}

void StatusBarWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    pruneExpiredToasts();
    reconcileVisibleToastsWithWidth();
    rebuildToasts();
}
