#include "CoordinatorPane.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QDateTime>

CoordinatorPane::CoordinatorPane(CoordinatorController*, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Worker Coordinator is temporarily unavailable."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(
        QStringLiteral("Coordinator controls are not implemented in the SimCoreDB Qt2 migration slice yet. "
                       "Legacy SimCore/DB coordinator wiring is intentionally disabled."),
        this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();

}

void CoordinatorPane::setPageActive(bool active)
{
    if (active) {
        emit statusToastRequested(StatusToast{
            StatusToast::Severity::Warn,
            QStringLiteral("Coordinator controls are temporarily disabled in Qt2."),
            QString(),
            1,
            QDateTime::currentDateTimeUtc(),
            6000 });
    }
}

void CoordinatorPane::requestVisualReplay(qint64)
{
}
