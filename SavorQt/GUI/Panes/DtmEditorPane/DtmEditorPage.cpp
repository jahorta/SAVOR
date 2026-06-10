#include "DtmEditorPage.h"

#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtCore/QDateTime>

DtmEditorPage::DtmEditorPage(QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    auto* title = new QLabel(QStringLiteral("DTM Editor is temporarily unavailable."), this);
    title->setObjectName("sectionTitle");
    auto* detail = new QLabel(
        QStringLiteral("DTM Editor is not implemented in this Qt2 migration slice yet. The legacy SavorCore/DB workflow path is not the forward path."),
        this);
    detail->setObjectName("sectionDescription");
    detail->setWordWrap(true);
    layout->addWidget(title);
    layout->addWidget(detail);
    layout->addStretch();
}

void DtmEditorPage::setPageActive(bool active)
{
    if (active) {
        emit statusToastRequested(StatusToast{
            StatusToast::Severity::Warn,
            QStringLiteral("DTM Editor is temporarily not implemented in Qt2."),
            QString(),
            1,
            QDateTime::currentDateTimeUtc(),
            6000 });
    }
}
