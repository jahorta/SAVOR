#include "LiveLogListModel.h"

#include <QtCore/QRegularExpression>

namespace {
constexpr int kMaxLiveLogLines = 10000;
constexpr int kLevelAll = -1;
constexpr int kLevelUnknown = 999;
constexpr const char* kUnknownSource = "Unknown";
constexpr const char* kHostSource = "[host]";

struct ParsedLogLine {
    int level = kLevelUnknown;
    QString source = kUnknownSource;
    QString message;
};

QString levelLabel(int level)
{
    switch (level) {
    case 0: return QStringLiteral("DEBUG");
    case 1: return QStringLiteral("TRACE");
    case 2: return QStringLiteral("INFO");
    case 3: return QStringLiteral("WARN");
    case 4: return QStringLiteral("ERROR");
    case 5: return QStringLiteral("FATAL");
    default: return QStringLiteral("UNKNOWN");
    }
}

int parseLevel(const QString& token)
{
    if (token == QStringLiteral("DEBUG")) return 0;
    if (token == QStringLiteral("TRACE")) return 1;
    if (token == QStringLiteral("INFO")) return 2;
    if (token == QStringLiteral("WARN")) return 3;
    if (token == QStringLiteral("ERROR")) return 4;
    if (token == QStringLiteral("FATAL")) return 5;
    return kLevelUnknown;
}

ParsedLogLine parseLogLine(const QString& line)
{
    ParsedLogLine parsed{};
    parsed.message = line;

    if (line.startsWith(QStringLiteral("[host]"))) {
        parsed.level = 2;
        parsed.source = kHostSource;
        parsed.message = line;
        return parsed;
    }

    static const QRegularExpression kPattern(
        QStringLiteral(R"(^\[[^\]]*\]\s+\[([^\]]+)\]\s+\[[^\]]*\]\s+\(([^:\)]+):\d+[^\)]*\)\s*(.*)$)"));

    const QRegularExpressionMatch match = kPattern.match(line);
    if (!match.hasMatch()) {
        parsed.level = kLevelUnknown;
        parsed.source = kUnknownSource;
        parsed.message = line;
        return parsed;
    }

    parsed.level = parseLevel(match.captured(1).trimmed());
    parsed.source = match.captured(2).trimmed();
    if (parsed.source.isEmpty()) {
        parsed.source = kUnknownSource;
    }
    parsed.message = match.captured(3).trimmed();
    if (parsed.message.isEmpty()) {
        parsed.message = line;
    }
    return parsed;
}
}

LiveLogListModel::LiveLogListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LiveLogListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(visibleIndexes_.size());
}

QVariant LiveLogListModel::data(const QModelIndex& index, int role) const
{
    if (role != Qt::DisplayRole || !index.isValid()) {
        return {};
    }
    const int row = index.row();
    if (row < 0 || row >= visibleIndexes_.size()) {
        return {};
    }

    const LiveLogRecord& rec = records_[visibleIndexes_[row]];
    if (showSource_) {
        return QStringLiteral("[%1] %2 %3").arg(levelLabel(rec.level), rec.source, rec.message);
    }
    return QStringLiteral("[%1] %2").arg(levelLabel(rec.level), rec.message);
}

void LiveLogListModel::clear()
{
    beginResetModel();
    records_.clear();
    visibleIndexes_.clear();
    knownSources_.clear();
    selectedSources_.clear();
    autoSelectNewSources_ = true;
    minLevel_ = kLevelAll;
    showSource_ = true;
    endResetModel();
}

void LiveLogListModel::appendRawLines(const QStringList& lines)
{
    if (lines.isEmpty()) {
        return;
    }

    bool sourcesChanged = false;
    for (const QString& line : lines) {
        const ParsedLogLine parsed = parseLogLine(line);
        LiveLogRecord rec{ parsed.level, parsed.source, parsed.message };
        records_.push_back(std::move(rec));

        if (!knownSources_.contains(parsed.source)) {
            knownSources_.insert(parsed.source);
            if (autoSelectNewSources_) {
                selectedSources_.insert(parsed.source);
            }
            sourcesChanged = true;
        }
    }

    while (records_.size() > kMaxLiveLogLines) {
        records_.pop_front();
    }

    rebuildVisibleIndexes();

    if (sourcesChanged && onSourcesChanged) {
        onSourcesChanged();
    }
}

QStringList LiveLogListModel::knownSources() const
{
    QStringList values = knownSources_.values();
    values.sort(Qt::CaseInsensitive);
    return values;
}

QSet<QString> LiveLogListModel::selectedSources() const
{
    return selectedSources_;
}

void LiveLogListModel::setSelectedSources(const QSet<QString>& sources)
{
    selectedSources_ = sources;
    autoSelectNewSources_ = false;
    rebuildVisibleIndexes();
}

void LiveLogListModel::setSelectAllSources(bool selected)
{
    if (selected) {
        selectedSources_ = knownSources_;
        autoSelectNewSources_ = true;
    }
    else {
        selectedSources_.clear();
        autoSelectNewSources_ = false;
    }
    rebuildVisibleIndexes();
}

void LiveLogListModel::setMinLevel(int level)
{
    minLevel_ = level;
    rebuildVisibleIndexes();
}

void LiveLogListModel::setShowSource(bool show)
{
    if (showSource_ == show) {
        return;
    }
    showSource_ = show;
    if (rowCount() > 0) {
        emit dataChanged(index(0, 0), index(rowCount() - 1, 0), { Qt::DisplayRole });
    }
}

bool LiveLogListModel::passesFilter(const LiveLogRecord& rec) const
{
    if (minLevel_ != kLevelAll) {
        if (rec.level == kLevelUnknown || rec.level < minLevel_) {
            return false;
        }
    }
    if (!selectedSources_.contains(rec.source)) {
        return false;
    }
    return true;
}

void LiveLogListModel::rebuildVisibleIndexes()
{
    beginResetModel();
    visibleIndexes_.clear();
    visibleIndexes_.reserve(static_cast<int>(records_.size()));
    for (int i = 0; i < records_.size(); ++i) {
        if (passesFilter(records_[i])) {
            visibleIndexes_.push_back(i);
        }
    }
    endResetModel();
}
