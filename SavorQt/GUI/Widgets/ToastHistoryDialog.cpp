#include "GUI/Widgets/ToastHistoryDialog.h"

#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QVBoxLayout>

ToastHistoryDialog::ToastHistoryDialog(const QString& historyFilePath, std::function<QStringList()> loadFromFileFn, QWidget* parent)
    : QDialog(parent)
    , historyFilePath_(historyFilePath)
    , loadFromFileFn_(std::move(loadFromFileFn))
{
    setWindowTitle(QStringLiteral("Toast History"));
    setModal(true);
    resize(900, 520);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(10);

    auto* filePathLabel = new QLabel(this);
    filePathLabel->setObjectName("toastHistoryPath");
    filePathLabel->setText(historyFilePath_.isEmpty()
        ? QStringLiteral("History file: unavailable for this run")
        : QStringLiteral("History file: %1").arg(historyFilePath_));
    filePathLabel->setWordWrap(true);
    layout->addWidget(filePathLabel);

    historyText_ = new QPlainTextEdit(this);
    historyText_->setObjectName("toastHistoryText");
    historyText_->setReadOnly(true);
    historyText_->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(historyText_, 1);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, this);
    auto* loadFromFileButton = buttonBox->addButton(QStringLiteral("Load full history from file"), QDialogButtonBox::ActionRole);
    connect(loadFromFileButton, &QPushButton::clicked, this, [this]() {
        if (!loadFromFileFn_) {
            return;
        }
        setHistoryLines(loadFromFileFn_());
    });
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);
}

void ToastHistoryDialog::setHistoryLines(const QStringList& lines)
{
    historyText_->setPlainText(lines.join('\n'));
}
