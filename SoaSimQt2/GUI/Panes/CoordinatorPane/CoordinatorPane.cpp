#include "CoordinatorPane.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

CoordinatorPane::CoordinatorPane(CoordinatorController*, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Worker Coordinator is waiting for SimCoreDB runtime commands."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(QStringLiteral("Legacy DB worker coordinator controls are disabled during the UIRead cutover."), this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

void CoordinatorPane::setPageActive(bool)
{
}

void CoordinatorPane::requestVisualReplay(qint64)
{
}

