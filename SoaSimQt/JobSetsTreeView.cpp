#include "JobSetsTreeView.h"

#include "JobSetsTreeModel.h"

#include <QtCore/QItemSelectionModel>

#include <functional>
#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMenu>

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
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAllColumnsShowFocus(true);
    setSortingEnabled(false);
    setExpandsOnDoubleClick(true);
    header()->setStretchLastSection(false);
    header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    header()->setSectionResizeMode(JobSetsTreeModel::PurposeColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(JobSetsTreeModel::ProgressColumn, QHeaderView::Stretch);
    header()->setSectionResizeMode(JobSetsTreeModel::ActionsColumn, QHeaderView::ResizeToContents);

    connect(this, &QWidget::customContextMenuRequested, this, &JobSetsTreeView::showContextMenu);
}

void JobSetsTreeView::attachModel(JobSetsTreeModel* model)
{
    setModel(model);
}

void JobSetsTreeView::setActionsEnabled(bool enabled)
{
    actionsEnabled_ = enabled;
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

qint64 JobSetsTreeView::currentJobSetId() const
{
    const QModelIndex idx = currentIndex();
    return idx.isValid() ? idx.data(JobSetsTreeModel::JobSetIdRole).toLongLong() : 0;
}

void JobSetsTreeView::showContextMenu(const QPoint& position)
{
    const QModelIndex idx = indexAt(position);
    if (!idx.isValid()) {
        return;
    }

    selectionModel()->setCurrentIndex(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const qint64 jobSetId = idx.data(JobSetsTreeModel::JobSetIdRole).toLongLong();
    if (jobSetId <= 0) {
        return;
    }

    QMenu menu(this);
    QAction* boostAction = menu.addAction(QStringLiteral("Boost"));
    QAction* cancelQueuedAction = menu.addAction(QStringLiteral("Cancel queued"));
    menu.addSeparator();
    QAction* deleteAction = menu.addAction(QStringLiteral("Delete"));

    boostAction->setEnabled(actionsEnabled_);
    cancelQueuedAction->setEnabled(actionsEnabled_);
    deleteAction->setEnabled(actionsEnabled_);

    QAction* chosen = menu.exec(viewport()->mapToGlobal(position));
    if (chosen == boostAction) {
        emit boostRequested(jobSetId);
    } else if (chosen == cancelQueuedAction) {
        emit cancelQueuedRequested(jobSetId);
    } else if (chosen == deleteAction) {
        emit deleteRequested(jobSetId);
    }
}
