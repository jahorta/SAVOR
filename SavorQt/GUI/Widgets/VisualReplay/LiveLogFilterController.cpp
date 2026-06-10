#include "LiveLogFilterController.h"

#include "LiveLogListModel.h"

LiveLogFilterController::LiveLogFilterController(LiveLogListModel* model)
    : model_(model)
{
}

void LiveLogFilterController::setMinLevel(int level)
{
    model_->setMinLevel(level);
}

void LiveLogFilterController::setShowSource(bool show)
{
    model_->setShowSource(show);
}

void LiveLogFilterController::setSelectedSources(const QSet<QString>& sources)
{
    model_->setSelectedSources(sources);
}

void LiveLogFilterController::setSelectAllSources(bool selected)
{
    model_->setSelectAllSources(selected);
}
