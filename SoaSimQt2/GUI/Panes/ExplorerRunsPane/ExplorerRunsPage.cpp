#include "ExplorerRunsPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QDateTime>

ExplorerRunsPage::ExplorerRunsPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("Explorer Runs is temporarily unavailable."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(
        QStringLiteral("Explorer Runs are not yet implemented in the SimCoreDB Qt2 workflow path. Legacy explorer-run calls are disabled."),
        this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();

}

void ExplorerRunsPage::setPageActive(bool active)
{
    if (active) {
        emit statusToastRequested(StatusToast{
            StatusToast::Severity::Warn,
            QStringLiteral("Explorer Runs are temporarily not implemented in Qt2."),
            QString(),
            1,
            QDateTime::currentDateTimeUtc(),
            6000 });
    }
}
