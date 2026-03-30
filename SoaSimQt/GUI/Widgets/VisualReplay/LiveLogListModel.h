#pragma once

#include <QtCore/QSet>
#include <QtCore/QStringList>
#include <QtCore/QAbstractListModel>
#include <QtCore/QVector>

#include <deque>
#include <functional>

class LiveLogListModel final : public QAbstractListModel
{
public:
    explicit LiveLogListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void clear();
    void appendRawLines(const QStringList& lines);

    QStringList knownSources() const;
    QSet<QString> selectedSources() const;

    void setSelectedSources(const QSet<QString>& sources);
    void setSelectAllSources(bool selected);
    void setMinLevel(int level);
    void setShowSource(bool show);

    std::function<void()> onSourcesChanged;

private:
    struct LiveLogRecord {
        int level;
        QString source;
        QString message;
    };

    bool passesFilter(const LiveLogRecord& rec) const;
    void rebuildVisibleIndexes();

    QVector<int> visibleIndexes_;
    QSet<QString> knownSources_;
    QSet<QString> selectedSources_;
    std::deque<LiveLogRecord> records_;
    int minLevel_ = -1;
    bool showSource_ = true;
    bool autoSelectNewSources_ = true;
};
