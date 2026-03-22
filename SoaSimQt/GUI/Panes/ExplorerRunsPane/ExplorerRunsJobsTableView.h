#pragma once

#include <QtWidgets/QTreeView>

class ExplorerRunsJobsTableModel;

class ExplorerRunsJobsTableView final : public QTreeView
{
public:
    explicit ExplorerRunsJobsTableView(QWidget* parent = nullptr);

    void attachModel(ExplorerRunsJobsTableModel* model);
    qint64 selectedJobId() const;
};
