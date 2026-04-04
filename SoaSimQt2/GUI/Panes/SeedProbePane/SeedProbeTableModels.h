#pragma once

#include <QtCore/QAbstractTableModel>
#include <QtCore/QVector>

class SeedProbeListModel final : public QAbstractTableModel
{
public:
    struct Row {
        qint64 probeId = 0;
        QString status;
        qint64 savestateId = 0;
        QString filename;
    };

    explicit SeedProbeListModel(QObject* parent = nullptr);

    void setRows(const QVector<Row>& rows);
    const Row* rowAt(int row) const;

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    QVector<Row> rows_;
};

class SeedProbeUniqueTableModel final : public QAbstractTableModel
{
public:
    struct Row {
        QString input;
        QString seedHex;
    };

    explicit SeedProbeUniqueTableModel(QObject* parent = nullptr);

    void setRows(const QVector<Row>& rows);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

private:
    QVector<Row> rows_;
};
