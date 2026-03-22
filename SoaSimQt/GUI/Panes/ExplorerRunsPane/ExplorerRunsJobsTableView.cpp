#include "ExplorerRunsJobsTableView.h"

#include "ExplorerRunsJobsTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

ExplorerRunsJobsTableView::ExplorerRunsJobsTableView(QWidget* parent)
    : QTreeView(parent)
{
    setObjectName("explorerRunsJobsTable");
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

void ExplorerRunsJobsTableView::attachModel(ExplorerRunsJobsTableModel* model)
{
    setModel(model);
    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(ExplorerRunsJobsTableModel::Outcome, QHeaderView::Stretch);
    header()->setSectionResizeMode(ExplorerRunsJobsTableModel::Predicates, QHeaderView::ResizeToContents);
}

qint64 ExplorerRunsJobsTableView::selectedJobId() const
{
    const QModelIndex current = currentIndex();
    const auto* tableModel = static_cast<const ExplorerRunsJobsTableModel*>(model());
    if (!current.isValid() || !tableModel) {
        return 0;
    }
    const ExplorerRunsJobRow* row = tableModel->rowAt(current.row());
    return row ? row->jobId : 0;
}
