#pragma once

#include <QtWidgets/QTreeView>

#include "Core/Memory/Soa/Battle/BattleContext.h"

class BattleContextTreeModel;

class BattleContextTreeWidget final : public QTreeView
{
public:
    explicit BattleContextTreeWidget(QWidget* parent = nullptr);

    void setBattleContext(
        qint64 seedProbeId,
        qint64 savestateId,
        int partySize,
        bool hasContext,
        const soa::battle::ctx::BattleContext& context);

private:
    BattleContextTreeModel* model_ = nullptr;
};
