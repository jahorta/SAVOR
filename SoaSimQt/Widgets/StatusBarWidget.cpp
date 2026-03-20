#include "Widgets/StatusBarWidget.h"

#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QStyle>

namespace {
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

StatusBarWidget::StatusBarWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("statusBarWidget");
    setMinimumHeight(34);

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
    rebuildToasts();
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

    for (const StatusToast& toast : snapshot_.toasts) {
        QLabel* badge = createBadge(buildToastText(toast), toastVariant(toast.severity));
        if (!toast.details.isEmpty()) {
            badge->setToolTip(toast.details);
        }
        toastLayout_->addWidget(badge);
    }
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
