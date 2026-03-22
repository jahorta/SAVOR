#pragma once

#include <QtCore/QAbstractTableModel>

#include <vector>

struct ExplorerRunsJobRow
{
    qint64 jobId = 0;
    QString state;
    QString outcome;
    QString predicates;
    quint32 deltaVi = 0;
    quint32 fakeAttacks = 0;
    QString rngSeed;
};

class ExplorerRunsJobsTableModel final : public QAbstractTableModel
{
public:
    enum Column
    {
        JobId = 0,
        JobState,
        Outcome,
        Predicates,
        DeltaVi,
        FakeAttacks,
        RngSeed,
        Count
    };

    explicit ExplorerRunsJobsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setRows(std::vector<ExplorerRunsJobRow> rows);
    const ExplorerRunsJobRow* rowAt(int row) const;

private:
    std::vector<ExplorerRunsJobRow> rows_;
};
