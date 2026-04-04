#include "ExplorerRunsWaveTreeModel.h"

#include <QtGui/QStandardItem>

#include <algorithm>
#include <unordered_map>

ExplorerRunsWaveTreeModel::ExplorerRunsWaveTreeModel(QObject* parent)
    : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({ QStringLiteral("Wave"), QStringLiteral("Status") });
}

void ExplorerRunsWaveTreeModel::syncFromGroup(const ExplorerRunsCoordinator::GroupRow* group)
{
    const std::vector<TurnNode> turns = buildTurns(group);

    while (rowCount() > static_cast<int>(turns.size())) {
        removeRow(rowCount() - 1);
    }

    for (int row = 0; row < static_cast<int>(turns.size()); ++row) {
        if (row >= rowCount()) {
            auto* turnItem = new QStandardItem();
            turnItem->setSelectable(false);
            auto* statusItem = new QStandardItem();
            statusItem->setSelectable(false);
            appendRow({ turnItem, statusItem });
        }
        syncTurnRow(row, turns[static_cast<size_t>(row)]);
    }
}

std::vector<ExplorerRunsWaveTreeModel::TurnNode> ExplorerRunsWaveTreeModel::buildTurns(const ExplorerRunsCoordinator::GroupRow* group) const
{
    if (!group) {
        return {};
    }

    std::unordered_map<quint32, std::vector<const ExplorerRunsCoordinator::WaveRow*>> byTurn;
    for (const ExplorerRunsCoordinator::WaveRow& wave : group->waves) {
        byTurn[wave.waveTurn].push_back(&wave);
    }

    std::vector<quint32> turns;
    turns.reserve(byTurn.size());
    for (const auto& entry : byTurn) {
        turns.push_back(entry.first);
    }
    std::sort(turns.begin(), turns.end());

    std::vector<TurnNode> out;
    out.reserve(turns.size());
    for (const quint32 turn : turns) {
        auto waves = byTurn[turn];
        std::sort(waves.begin(), waves.end(), [](const auto* a, const auto* b) {
            return a->createdAt < b->createdAt;
        });

        TurnNode turnNode;
        turnNode.turn = turn;
        turnNode.status = QStringLiteral("%1 waves").arg(waves.size());
        turnNode.waves.reserve(waves.size());
        for (const auto* wave : waves) {
            const QString icon = wave->hasWinner
                ? QStringLiteral("●")
                : (wave->hasSuccessOutcome ? QStringLiteral("◐") : QStringLiteral("○"));
            turnNode.waves.push_back(WaveNode{
                wave->jobSetId,
                QStringLiteral("%1 Wave %2").arg(icon).arg(wave->jobSetId),
                wave->statusSummary
            });
        }
        out.push_back(std::move(turnNode));
    }

    return out;
}

void ExplorerRunsWaveTreeModel::setItemTextIfDifferent(QStandardItem* item, const QString& text)
{
    if (item && item->text() != text) {
        item->setText(text);
    }
}

void ExplorerRunsWaveTreeModel::setWaveRoleData(QStandardItem* item, qint64 jobSetId)
{
    if (!item) {
        return;
    }
    if (item->data(kWaveJobSetIdUserRole).toLongLong() != jobSetId) {
        item->setData(jobSetId, kWaveJobSetIdUserRole);
    }
}

bool ExplorerRunsWaveTreeModel::containsWaveId(const TurnNode& turnNode, qint64 jobSetId)
{
    return std::any_of(turnNode.waves.begin(), turnNode.waves.end(), [jobSetId](const WaveNode& wave) {
        return wave.jobSetId == jobSetId;
    });
}

void ExplorerRunsWaveTreeModel::syncTurnRow(int row, const TurnNode& node)
{
    auto* turnItem = item(row, 0);
    auto* turnStatusItem = item(row, 1);
    if (!turnItem || !turnStatusItem) {
        return;
    }

    setItemTextIfDifferent(turnItem, QStringLiteral("Turn %1").arg(node.turn));
    setItemTextIfDifferent(turnStatusItem, node.status);
    turnItem->setSelectable(false);
    turnStatusItem->setSelectable(false);

    for (int childRow = turnItem->rowCount() - 1; childRow >= 0; --childRow) {
        QStandardItem* childItem = turnItem->child(childRow, 0);
        if (!childItem) {
            continue;
        }
        const qint64 jobSetId = childItem->data(kWaveJobSetIdUserRole).toLongLong();
        if (!containsWaveId(node, jobSetId)) {
            turnItem->removeRow(childRow);
        }
    }

    for (int idx = 0; idx < static_cast<int>(node.waves.size()); ++idx) {
        const WaveNode& wave = node.waves[static_cast<size_t>(idx)];
        QStandardItem* waveItem = nullptr;
        QStandardItem* statusItem = nullptr;

        if (idx < turnItem->rowCount()) {
            waveItem = turnItem->child(idx, 0);
            statusItem = turnItem->child(idx, 1);
            const qint64 currentId = waveItem ? waveItem->data(kWaveJobSetIdUserRole).toLongLong() : -1;
            if (currentId != wave.jobSetId) {
                int foundRow = -1;
                for (int searchRow = idx + 1; searchRow < turnItem->rowCount(); ++searchRow) {
                    QStandardItem* searchItem = turnItem->child(searchRow, 0);
                    if (searchItem && searchItem->data(kWaveJobSetIdUserRole).toLongLong() == wave.jobSetId) {
                        foundRow = searchRow;
                        break;
                    }
                }
                if (foundRow >= 0) {
                    QList<QStandardItem*> moved = turnItem->takeRow(foundRow);
                    turnItem->insertRow(idx, moved);
                } else {
                    auto* newWaveItem = new QStandardItem();
                    auto* newStatusItem = new QStandardItem();
                    turnItem->insertRow(idx, { newWaveItem, newStatusItem });
                }
            }
        } else {
            auto* newWaveItem = new QStandardItem();
            auto* newStatusItem = new QStandardItem();
            turnItem->appendRow({ newWaveItem, newStatusItem });
        }

        waveItem = turnItem->child(idx, 0);
        statusItem = turnItem->child(idx, 1);
        setItemTextIfDifferent(waveItem, wave.label);
        setItemTextIfDifferent(statusItem, wave.status);
        setWaveRoleData(waveItem, wave.jobSetId);
        setWaveRoleData(statusItem, wave.jobSetId);
    }

    while (turnItem->rowCount() > static_cast<int>(node.waves.size())) {
        turnItem->removeRow(turnItem->rowCount() - 1);
    }
}
