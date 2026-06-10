#pragma once

#include <QtCore/QAbstractTableModel>

#include <vector>

#include "Runner/Parallel/WorkerTelemetry.h"

class WorkerTableModel : public QAbstractTableModel
{

public:
    explicit WorkerTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    void setSnapshots(std::vector<WorkerSnapshot> snapshots);

private:
    static bool rowsAffectDisplay(const WorkerSnapshot& lhs, const WorkerSnapshot& rhs);

    enum Column {
        Id = 0,
        Pid,
        State,
        Job,
        Kind,
        LastHeartbeat,
        Status,
        Count
    };

    std::vector<WorkerSnapshot> snapshots_;
};
