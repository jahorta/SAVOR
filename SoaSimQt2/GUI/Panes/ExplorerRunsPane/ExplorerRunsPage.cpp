#include "ExplorerRunsPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

ExplorerRunsPage::ExplorerRunsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Explorer Runs are waiting for SimCoreDB battle UIRead queries."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(QStringLiteral("Legacy explorer-run repositories are disabled during the UIRead cutover."), this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

void ExplorerRunsPage::setPageActive(bool)
{
}

