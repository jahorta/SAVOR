#pragma once

#include <QtCore/QVector>
#include <QtCore/QString>
#include <vector>

#include "GUI/Widgets/PersistentToolWindow.h"
#include "Worker/WorkerTelemetry.h"

struct VisualWorkerSurfaceBinding {
    int workerIndex = 0;
    quintptr renderWidgetHandle = 0;
    QString hostEventsPipeName;
};

class QGridLayout;
class QLabel;
class QScrollArea;

class VisualWorkerDashboardWindow final : public PersistentToolWindow
{
public:
    explicit VisualWorkerDashboardWindow(QWidget* parent = nullptr);

    void setWorkerCount(int count, bool allowShrink);
    void updateWorkerSnapshots(const std::vector<WorkerSnapshot>& snapshots);
    QVector<VisualWorkerSurfaceBinding> surfaceBindings() const;

private:
    struct Tile {
        QWidget* frame = nullptr;
        QWidget* renderWidget = nullptr;
        QLabel* titleLabel = nullptr;
        QLabel* stateLabel = nullptr;
        QLabel* jobLabel = nullptr;
    };

    static QString stateText(WorkerStateKind state);
    Tile createTile(int workerIndex);
    void rebuildGrid();

    QScrollArea* scrollArea_ = nullptr;
    QWidget* gridContainer_ = nullptr;
    QGridLayout* gridLayout_ = nullptr;
    QVector<Tile> tiles_;
};
