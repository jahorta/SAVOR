#pragma once

#include <QtCore/QHash>
#include <QtCore/QVector>
#include <QtCore/QSet>
#include <QtCore/QString>
#include <vector>

#include "GUI/Widgets/PersistentToolWindow.h"
#include "Worker/WorkerTelemetry.h"

struct VisualWorkerSurfaceBinding {
    int workerIndex = 0;
    quintptr renderWidgetHandle = 0;
    quint64 surfaceGeneration = 0;
    quint32 ownerProcessId = 0;
    QString hostEventsPipeName;
};

class QGridLayout;
class QLabel;
class QScrollArea;
class QSpinBox;

class VisualWorkerDashboardWindow final : public PersistentToolWindow
{
    Q_OBJECT

public:
    VisualWorkerDashboardWindow();

    void ensureWorkerCount(int count);
    void releaseWorkersFrom(int firstWorkerIndex);
    int workerCount() const;
    void updateWorkerStatus(const WorkerSnapshot& snapshot);
    void clearWorkerStatusesExcept(const QSet<int>& workerIds);
    QVector<VisualWorkerSurfaceBinding> surfaceBindings() const;

signals:
    void surfaceChanged(VisualWorkerSurfaceBinding binding);
    void surfaceInvalidated(int workerIndex, quint64 surfaceGeneration);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Tile {
        QWidget* frame = nullptr;
        QWidget* renderContainer = nullptr;
        QWidget* renderWidget = nullptr;
        QLabel* titleLabel = nullptr;
        QLabel* stateLabel = nullptr;
        QLabel* jobLabel = nullptr;
        int workerIndex = 0;
        quintptr nativeHandle = 0;
        quint64 surfaceGeneration = 0;
    };

    static QString stateText(WorkerStateKind state);
    Tile createTile(int workerIndex);
    void rebuildGrid();

    QScrollArea* scrollArea_ = nullptr;
    QWidget* gridContainer_ = nullptr;
    QGridLayout* gridLayout_ = nullptr;
    QSpinBox* columnsSpin_ = nullptr;
    QVector<Tile> tiles_;
    QHash<int, quint64> surfaceGenerations_;
    int configuredColumns_ = 4;
    int gridRowCount_ = 0;
    int gridColumnCount_ = 0;
};
