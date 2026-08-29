#pragma once

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

class VisualWorkerDashboardWindow final : public PersistentToolWindow
{
    Q_OBJECT

public:
    explicit VisualWorkerDashboardWindow(QWidget* parent = nullptr);

    void setWorkerCount(int count, bool allowShrink);
    void updateWorkerStatus(const WorkerSnapshot& snapshot);
    void clearWorkerStatusesExcept(const QSet<int>& workerIds);
    QVector<VisualWorkerSurfaceBinding> surfaceBindings() const;

signals:
    void surfaceChanged(VisualWorkerSurfaceBinding binding);
    void surfaceInvalidated(int workerIndex, quint64 surfaceGeneration);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    struct Tile {
        QWidget* frame = nullptr;
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
    QVector<Tile> tiles_;
};
