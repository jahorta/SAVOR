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
    void setActionsEnabled(bool enabled);
    QSet<qint64> expandedJobSetIds() const;
    void restoreExpandedJobSetIds(const QSet<qint64>& expandedIds);
    qint64 currentJobSetId() const;

signals:
    void boostRequested(qint64 jobSetId);
    void cancelQueuedRequested(qint64 jobSetId);
    void tagsRequested(qint64 jobSetId);
    void deleteRequested(qint64 jobSetId);

private:
    void showContextMenu(const QPoint& position);

    bool actionsEnabled_ = true;
};
