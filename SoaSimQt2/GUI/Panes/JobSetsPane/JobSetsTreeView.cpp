#include "JobSetsTreeView.h"

#include "JobSetsTreeModel.h"

#include <functional>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

JobSetsTreeView::JobSetsTreeView(QWidget* parent)
    : QTreeView(parent)
{
    setObjectName("jobSetsTree");
    setRootIsDecorated(true);
    setUniformRowHeights(false);
    setAlternatingRowColors(true);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAllColumnsShowFocus(true);
    setSortingEnabled(false);
    setExpandsOnDoubleClick(true);
    header()->setStretchLastSection(false);
}

void JobSetsTreeView::attachModel(JobSetsTreeModel* model)
{
    setModel(model);

    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(JobSetsTreeModel::PurposeColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(JobSetsTreeModel::ProgressColumn, QHeaderView::Stretch);
}

QSet<qint64> JobSetsTreeView::expandedJobSetIds() const
{
    QSet<qint64> expanded;
    const auto* treeModel = static_cast<const JobSetsTreeModel*>(model());
    if (!treeModel) {
        return expanded;
    }

    std::function<void(const QModelIndex&)> visit = [&](const QModelIndex& parentIndex) {
        const int rows = treeModel->rowCount(parentIndex);
        for (int row = 0; row < rows; ++row) {
            const QModelIndex idx = treeModel->index(row, 0, parentIndex);
            if (!idx.isValid()) {
                continue;
            }
            if (isExpanded(idx)) {
                expanded.insert(idx.data(JobSetsTreeModel::JobSetIdRole).toLongLong());
            }
            visit(idx);
        }
    };

    visit(QModelIndex{});
    return expanded;
}

void JobSetsTreeView::restoreExpandedJobSetIds(const QSet<qint64>& expandedIds)
{
    auto* treeModel = static_cast<JobSetsTreeModel*>(model());
    if (!treeModel) {
        return;
    }

    for (qint64 id : expandedIds) {
        const QModelIndex idx = treeModel->indexForJobSetId(id);
        if (idx.isValid()) {
            expand(idx);
        }
    }
}

