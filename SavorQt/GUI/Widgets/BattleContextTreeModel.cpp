#include "BattleContextTreeModel.h"

#include "Core/Memory/Soa/SoaConstants.h"

#include <QtCore/QSet>
#include <QtGui/QStandardItem>

namespace {
QStandardItem* makeItem(const QString& text)
{
    QStandardItem* item = new QStandardItem(text);
    item->setEditable(false);
    return item;
}
}

BattleContextTreeModel::BattleContextTreeModel(QObject* parent)
    : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({ QStringLiteral("Battle Context") });
}

void BattleContextTreeModel::setBattleContext(
    qint64 seedProbeId,
    qint64 savestateId,
    int partySize,
    bool hasContext,
    const soa::battle::ctx::BattleContext& context)
{
    clear();
    setHorizontalHeaderLabels({ QStringLiteral("Battle Context") });

    QStandardItem* root = makeItem(QStringLiteral("Battle Context"));
    invisibleRootItem()->appendRow(root);
    root->appendRow(makeItem(QStringLiteral("Seed Probe ID: %1").arg(seedProbeId > 0 ? QString::number(seedProbeId) : QStringLiteral("(none)"))));
    root->appendRow(makeItem(QStringLiteral("Savestate ID: %1").arg(savestateId > 0 ? QString::number(savestateId) : QStringLiteral("(none)"))));
    root->appendRow(makeItem(QStringLiteral("Party size: %1").arg(partySize)));

    if (!hasContext) {
        root->appendRow(makeItem(QStringLiteral("No decoded BattleContext available.")));
        return;
    }

    QStandardItem* partyRoot = makeItem(QStringLiteral("Party Members"));
    QStandardItem* enemyRoot = makeItem(QStringLiteral("Enemies"));
    QStandardItem* dropRoot = makeItem(QStringLiteral("Item Drops"));
    root->appendRow(partyRoot);
    root->appendRow(enemyRoot);
    root->appendRow(dropRoot);

    QSet<int> seenEnemyIds;
    for (int slot = 0; slot < soa::battle::ctx::SLOT_COUNT; ++slot) {
        const auto& entry = context.slots_[slot];
        if (entry.present != 1) {
            continue;
        }

        const bool isPlayer = entry.is_player == 1;
        const QString name = isPlayer
            ? QString::fromUtf8(soa::text::PCNames.at(static_cast<std::size_t>(entry.id)).data())
            : QString::fromUtf8(soa::text::get_enemy_name(entry.id).data());
        QStandardItem* slotItem = makeItem(QStringLiteral("[%1] %2").arg(slot).arg(name));
        slotItem->appendRow(makeItem(QStringLiteral("Alive: %1").arg(entry.is_alive ? QStringLiteral("yes") : QStringLiteral("no"))));
        slotItem->appendRow(makeItem(QStringLiteral("ID: %1").arg(entry.id)));

        if (isPlayer) {
            slotItem->appendRow(makeItem(QStringLiteral("Weapon element: %1").arg(QString::fromUtf8(soa::text::get_element_name(entry.instance.current_weapon_element).data()))));
            partyRoot->appendRow(slotItem);
            continue;
        }

        slotItem->appendRow(makeItem(QStringLiteral("Enemy definition loaded: %1").arg(entry.has_enemy_def ? QStringLiteral("yes") : QStringLiteral("no"))));
        enemyRoot->appendRow(slotItem);

        if (!entry.has_enemy_def || seenEnemyIds.contains(entry.id)) {
            continue;
        }

        seenEnemyIds.insert(entry.id);
        QStandardItem* enemyDropItem = makeItem(name);
        bool anyDrops = false;
        for (const auto& item : entry.enemy_def.items) {
            if (item.itemId < 0) {
                continue;
            }

            anyDrops = true;
            enemyDropItem->appendRow(
                makeItem(
                    QStringLiteral("(%1%%) [%2] %3 x%4")
                        .arg(static_cast<int>(item.chance))
                        .arg(static_cast<int>(item.itemId))
                        .arg(QString::fromUtf8(soa::text::get_item_name(static_cast<std::size_t>(item.itemId)).data()))
                        .arg(static_cast<int>(item.amount))));
        }
        if (!anyDrops) {
            enemyDropItem->appendRow(makeItem(QStringLiteral("No item drops recorded.")));
        }
        dropRoot->appendRow(enemyDropItem);
    }

    if (partyRoot->rowCount() == 0) {
        partyRoot->appendRow(makeItem(QStringLiteral("No party members present.")));
    }
    if (enemyRoot->rowCount() == 0) {
        enemyRoot->appendRow(makeItem(QStringLiteral("No enemies present.")));
    }
    if (dropRoot->rowCount() == 0) {
        dropRoot->appendRow(makeItem(QStringLiteral("No unique enemy drop tables available.")));
    }
}
