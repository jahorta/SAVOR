#pragma once

#include <QtWidgets/QTreeView>

class JobsTableModel;

class JobsTableView final : public QTreeView
{
public:
    explicit JobsTableView(QWidget* parent = nullptr);

    void attachModel(JobsTableModel* model);
    const JobsTableModel* jobsModel() const;
    JobsTableModel* jobsModel();
    qint64 selectedJobId() const;
};
