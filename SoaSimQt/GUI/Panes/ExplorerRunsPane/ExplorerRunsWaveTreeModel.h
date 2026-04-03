#pragma once

#include <QtGui/QStandardItemModel>

#include "ExplorerRunsCoordinator.h"

#include <vector>

class QStandardItem;

class ExplorerRunsWaveTreeModel final : public QStandardItemModel
{
public:
    struct WaveNode {
        qint64 jobSetId = 0;
        QString label;
        QString status;
    };

    struct TurnNode {
        quint32 turn = 0;
        QString status;
        std::vector<WaveNode> waves;
    };

    explicit ExplorerRunsWaveTreeModel(QObject* parent = nullptr);

    void syncFromGroup(const ExplorerRunsCoordinator::GroupRow* group);

private:
    std::vector<TurnNode> buildTurns(const ExplorerRunsCoordinator::GroupRow* group) const;
    static void setItemTextIfDifferent(QStandardItem* item, const QString& text);
    static void setWaveRoleData(QStandardItem* item, qint64 jobSetId);
    static bool containsWaveId(const TurnNode& turnNode, qint64 jobSetId);
    void syncTurnRow(int row, const TurnNode& node);

    static constexpr int kWaveJobSetIdUserRole = Qt::UserRole + 1;
};
