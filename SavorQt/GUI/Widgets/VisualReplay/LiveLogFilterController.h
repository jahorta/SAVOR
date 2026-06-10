#pragma once

#include <QtCore/QSet>
#include <QtCore/QString>

class LiveLogListModel;

class LiveLogFilterController final
{
public:
    explicit LiveLogFilterController(LiveLogListModel* model);

    void setMinLevel(int level);
    void setShowSource(bool show);
    void setSelectedSources(const QSet<QString>& sources);
    void setSelectAllSources(bool selected);

private:
    LiveLogListModel* model_ = nullptr;
};
