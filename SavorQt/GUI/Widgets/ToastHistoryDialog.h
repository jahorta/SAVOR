#pragma once

#include <functional>

#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtWidgets/QDialog>

class QPlainTextEdit;

class ToastHistoryDialog final : public QDialog
{
public:
    explicit ToastHistoryDialog(const QString& historyFilePath, std::function<QStringList()> loadFromFileFn, QWidget* parent = nullptr);

    void setHistoryLines(const QStringList& lines);

private:
    QPlainTextEdit* historyText_ = nullptr;
    QString historyFilePath_;
    std::function<QStringList()> loadFromFileFn_;
};
