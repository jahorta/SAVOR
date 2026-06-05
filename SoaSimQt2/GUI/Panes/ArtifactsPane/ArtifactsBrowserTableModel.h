#pragma once

#include <QtCore/QAbstractTableModel>

#include "UIRead/IUiReadDb.h"

#include <vector>

class ArtifactsBrowserTableModel final : public QAbstractTableModel
{
public:
    struct Row {
        simcore::db::UiArtifactSummary artifact;
    };

    explicit ArtifactsBrowserTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setRows(const std::vector<Row>& rows);
    const Row* rowAt(int row) const;

    static QString formatSize(qint64 size);
    static QString formatCreatedAt(qint64 epochMilliseconds);

private:
    std::vector<Row> rows_;
};
