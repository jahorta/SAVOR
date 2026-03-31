#pragma once

#include <QtCore/QAbstractTableModel>

#include <optional>
#include <vector>

class JobsTableModel final : public QAbstractTableModel
{
public:
    struct Row {
        qint64 jobId = 0;
        qint64 jobSetId = 0;
        std::optional<qint64> savestateId;
        QString programKind;
        QString state;
        int attempts = 0;
        QString queuedAt;
        QString progress;
    };

    enum Column {
        JobIdColumn = 0,
        JobSetIdColumn,
        SaveStateIdColumn,
        ProgramKindColumn,
        StateColumn,
        AttemptsColumn,
        QueuedAtColumn,
        ProgressColumn,
        ColumnCount
    };

    explicit JobsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setRows(const std::vector<Row>& rows);
    const Row* rowAt(int row) const;

private:
    static bool rowsAffectDisplay(const Row& lhs, const Row& rhs);

    std::vector<Row> rows_;
};
