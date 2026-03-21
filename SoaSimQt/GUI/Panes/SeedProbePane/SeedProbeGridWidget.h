#pragma once

#include <QtCore/QVector>
#include <QtWidgets/QWidget>

class SeedProbeGridWidget final : public QWidget
{
public:
    struct Cell {
        int x = 0;
        int y = 0;
        int xSpan = 1;
        int ySpan = 1;
        int delta = 0;
    };

    struct GridData {
        QVector<Cell> cells;
        int minNeg = -2;
        int maxPos = 32;
        bool hasData = false;
    };

    explicit SeedProbeGridWidget(QWidget* parent = nullptr);

    void setTitle(const QString& title);
    void setGridData(const GridData& data);
    QSize minimumSizeHint() const override;
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString title_;
    GridData data_;
};
