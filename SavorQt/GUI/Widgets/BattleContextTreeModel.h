#pragma once

#include <QtGui/QStandardItemModel>

#include "Core/Memory/Soa/Battle/BattleContext.h"

class BattleContextTreeModel final : public QStandardItemModel
{
public:
    explicit BattleContextTreeModel(QObject* parent = nullptr);

    void setBattleContext(
        qint64 seedProbeId,
        qint64 savestateId,
        int partySize,
        bool hasContext,
        const soa::battle::ctx::BattleContext& context);
};
