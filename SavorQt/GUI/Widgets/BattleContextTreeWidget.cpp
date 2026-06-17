#include "BattleContextTreeWidget.h"

#include "BattleContextTreeModel.h"
#include "GUI/Widgets/ScrollBarStabilizer.h"

#include <QtWidgets/QAbstractItemView>
#include <QtWidgets/QHeaderView>

BattleContextTreeWidget::BattleContextTreeWidget(QWidget* parent)
    : QTreeView(parent)
    , model_(new BattleContextTreeModel(this))
{
    setModel(model_);
    setObjectName("jobsTable");
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setSelectionMode(QAbstractItemView::NoSelection);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setAlternatingRowColors(true);
    setRootIsDecorated(true);
    setAllColumnsShowFocus(true);
    setUniformRowHeights(true);
    header()->hide();
}

void BattleContextTreeWidget::setBattleContext(
    qint64 seedProbeId,
    qint64 savestateId,
    int partySize,
    bool hasContext,
    const soa::battle::ctx::BattleContext& context)
{
    const ItemViewScrollSnapshot scrollSnapshot = captureItemViewScrollSnapshot(this);
    model_->setBattleContext(seedProbeId, savestateId, partySize, hasContext, context);
    expandToDepth(1);
    restoreItemViewScrollSnapshot(this, scrollSnapshot);
}
