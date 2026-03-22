#pragma once

#include <QtWidgets/QTreeView>

class ExplorerRunsGroupTableModel;

class ExplorerRunsGroupTableView final : public QTreeView
{
public:
    explicit ExplorerRunsGroupTableView(QWidget* parent = nullptr);

    void attachModel(ExplorerRunsGroupTableModel* model);
    qint64 selectedRootGroupId() const;
};
