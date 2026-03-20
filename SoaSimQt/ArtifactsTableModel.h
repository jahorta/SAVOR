#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QStringList>

class ArtifactsTableModel final : public QAbstractTableModel
{
public:
    explicit ArtifactsTableModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    QVariant data(const QModelIndex& index, int role) const override;

    void setArtifacts(const QStringList& artifacts);

private:
    QStringList artifacts_;
};
