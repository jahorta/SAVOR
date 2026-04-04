#pragma once

#include <QtCore/QAbstractTableModel>

#include <vector>
#include <string>

struct ExplorerRunsGroupRow
{
    qint64 rootGroupId = 0;
    QString settingsLabel;
    QString resultsSummary;
    int totalWaves = 0;
    QString statusSummary;
};

class ExplorerRunsGroupTableModel final : public QAbstractTableModel
{
public:
    enum Column
    {
        RootGroupId = 0,
        ExplorerSettings,
        Results,
        TotalWaves,
        StatusSummary,
        Count
    };

    explicit ExplorerRunsGroupTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    void setRows(std::vector<ExplorerRunsGroupRow> rows);
    const ExplorerRunsGroupRow* rowAt(int row) const;

private:
    std::vector<ExplorerRunsGroupRow> rows_;
};
