#include "DtmEditorPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

DtmEditorPage::DtmEditorPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("DTM Editor import is waiting for SimCoreDB artifact commands."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(QStringLiteral("Legacy ObjectStore and job creation calls are disabled during the UIRead cutover."), this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

