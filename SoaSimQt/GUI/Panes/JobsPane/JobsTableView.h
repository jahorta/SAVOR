#pragma once

#include <QtWidgets/QTableView>

class JobsTableModel;

class JobsTableView final : public QTableView
{
public:
    explicit JobsTableView(QWidget* parent = nullptr);

    void attachModel(JobsTableModel* model);
    const JobsTableModel* jobsModel() const;
    JobsTableModel* jobsModel();
    qint64 selectedJobId() const;
};
