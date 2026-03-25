#pragma once

#include <QtWidgets/QDialog>

struct ExplorerRunsJobRow;

class QLabel;
class QTreeWidget;

class ExplorerRunsTurnInputsDialog final : public QDialog
{
public:
    explicit ExplorerRunsTurnInputsDialog(QWidget* parent = nullptr);

    void loadForJob(const ExplorerRunsJobRow& row);

private:
    QLabel* banner_ = nullptr;
    QTreeWidget* tree_ = nullptr;
};
