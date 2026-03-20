#include "JobsTableView.h"

#include "JobsTableModel.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

JobsTableView::JobsTableView(QWidget* parent)
    : QTableView(parent)
{
    setObjectName("jobsTable");
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setAlternatingRowColors(true);
    setShowGrid(false);
    verticalHeader()->setVisible(false);
    horizontalHeader()->setStretchLastSection(true);
}

void JobsTableView::attachModel(JobsTableModel* model)
{
    setModel(model);

    QHeaderView* header = horizontalHeader();
    header->setSectionResizeMode(QHeaderView::ResizeToContents);
    header->setSectionResizeMode(JobsTableModel::ProgressColumn, QHeaderView::Stretch);
}

const JobsTableModel* JobsTableView::jobsModel() const
{
    return static_cast<const JobsTableModel*>(model());
}

JobsTableModel* JobsTableView::jobsModel()
{
    return static_cast<JobsTableModel*>(model());
}

qint64 JobsTableView::selectedJobId() const
{
    const QModelIndex current = currentIndex();
    const JobsTableModel* tableModel = jobsModel();
    if (!current.isValid() || !tableModel) {
        return 0;
    }
    const JobsTableModel::Row* row = tableModel->rowAt(current.row());
    return row ? row->jobId : 0;
}
