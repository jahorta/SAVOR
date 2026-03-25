#include "VisualWorkerDialog.h"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QVBoxLayout>
#include <QtWidgets/QWidget>

VisualWorkerDialog::VisualWorkerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("Visual Worker"));
    resize(960, 640);

    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel(QStringLiteral("Visual worker render surface"), this);
    layout->addWidget(label);

    renderWidget_ = new QWidget(this);
    renderWidget_->setObjectName(QStringLiteral("visualWorkerRenderWidget"));
    renderWidget_->setMinimumSize(640, 360);
    renderWidget_->setAttribute(Qt::WA_NativeWindow, true);
    renderWidget_->setAutoFillBackground(true);
    layout->addWidget(renderWidget_, 1);

    QDialogButtonBox* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

VisualWorkerDialog::~VisualWorkerDialog() = default;

quintptr VisualWorkerDialog::renderWidgetHandle() const
{
    return renderWidget_ ? renderWidget_->winId() : 0;
}
