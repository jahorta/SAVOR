#include "BattleRunSettingsPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>

BattleRunSettingsPage::BattleRunSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Battle Run Settings are waiting for SimCoreDB authoring commands."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(QStringLiteral("Legacy authoring repositories are disabled during the UIRead cutover."), this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

