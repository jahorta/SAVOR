#pragma once

#include <QtCore/QSet>
#include <QtWidgets/QTreeView>

class JobSetsTreeModel;

class JobSetsTreeView final : public QTreeView
{
    Q_OBJECT

public:
    explicit JobSetsTreeView(QWidget* parent = nullptr);

    void attachModel(JobSetsTreeModel* model);
    QSet<qint64> expandedJobSetIds() const;
    void restoreExpandedJobSetIds(const QSet<qint64>& expandedIds);
};
