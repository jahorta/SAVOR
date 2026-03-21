#pragma once

#include <QtCore/QAbstractTableModel>

#include "DB/Querying/IdRepoListDTO.h"

#include <vector>

class ArtifactsBrowserTableModel final : public QAbstractTableModel
{
public:
    struct Row {
        simcore::db::ObjectRefLite artifact;
    };

    explicit ArtifactsBrowserTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setRows(const std::vector<Row>& rows);
    const Row* rowAt(int row) const;

    static QString formatSize(qint64 size);
    static QString formatCreatedAt(qint64 epochSeconds);
    static QString compressionLabel(simcore::db::Compression compression);

private:
    std::vector<Row> rows_;
};
