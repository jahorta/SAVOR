#pragma once

#include <QtCore/QObject>
#include <QtCore/QString>

#include <memory>
#include <vector>

#include "Runner/Parallel/DB/DBWorkerCoordinator.h"
#include "Runner/Parallel/DB/DBWorkerCoordinatorConfig.h"

class QSettings;

class CoordinatorController : public QObject
{
    Q_OBJECT

public:
    explicit CoordinatorController(QObject* parent = nullptr);
    ~CoordinatorController() override;

    bool isRunning() const;
    bool isPaused() const;
    int targetWorkers() const;
    int activeWorkers() const;
    int eventBufferCapacity() const;
    bool startPaused() const;

    QString isoPath() const;
    QString dolphinBaseDir() const;
    QString validationMessage() const;

    const std::vector<WorkerSnapshot>& snapshot() const;

public slots:
    void startCoordinator();
    void stopCoordinator();
    void setPaused(bool paused);
    void togglePaused();
    void setTargetWorkers(int targetWorkers);
    void setEventBufferCapacity(int capacity);
    void setStartPaused(bool startPaused);
    void setIsoPath(const QString& isoPath);
    void setDolphinBaseDir(const QString& dolphinBaseDir);
    void refreshSnapshot();

signals:
    void stateChanged();
    void snapshotChanged();

private:
    static constexpr int kMinTargetWorkers = 1;
    static constexpr int kMinEventBufferCapacity = 8;

    void loadSettings();
    void persistString(const char* key, const QString& value);
    void persistInt(const char* key, int value);
    void updateValidationMessage();
    void updateSnapshotCache();
    WorkerCoordinatorConfig buildConfig() const;

    std::unique_ptr<simcore::WorkerCoordinator> coordinator_;
    std::vector<WorkerSnapshot> snapshotCache_;

    int targetWorkers_ = kMinTargetWorkers;
    int eventBufferCapacity_ = 64;
    bool paused_ = false;
    bool startPaused_ = true;
    QString isoPath_;
    QString dolphinBaseDir_;
    QString validationMessage_;
};
