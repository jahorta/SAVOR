#include "JobBuilderPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

JobBuilderPage::JobBuilderPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Job Builder is waiting for SimCoreDB authoring commands."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(QStringLiteral("Legacy job creation is disabled during the UIRead cutover."), this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

