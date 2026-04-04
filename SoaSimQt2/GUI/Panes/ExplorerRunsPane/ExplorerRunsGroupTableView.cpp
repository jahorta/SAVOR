#include "ExplorerRunsGroupTableView.h"

#include "ExplorerRunsGroupTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

ExplorerRunsGroupTableView::ExplorerRunsGroupTableView(QWidget* parent)
    : QTreeView(parent)
{
    setObjectName("explorerRunsGroupsTable");
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setRootIsDecorated(false);
    setItemsExpandable(false);
    setAllColumnsShowFocus(true);
    setUniformRowHeights(true);
    setIndentation(0);
    header()->setStretchLastSection(true);
}

void ExplorerRunsGroupTableView::attachModel(ExplorerRunsGroupTableModel* model)
{
    setModel(model);
    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(ExplorerRunsGroupTableModel::ExplorerSettings, QHeaderView::Stretch);
    header()->setSectionResizeMode(ExplorerRunsGroupTableModel::StatusSummary, QHeaderView::Stretch);
}

qint64 ExplorerRunsGroupTableView::selectedRootGroupId() const
{
    const QModelIndex current = currentIndex();
    const auto* tableModel = static_cast<const ExplorerRunsGroupTableModel*>(model());
    if (!current.isValid() || !tableModel) {
        return 0;
    }
    const ExplorerRunsGroupRow* row = tableModel->rowAt(current.row());
    return row ? row->rootGroupId : 0;
}
