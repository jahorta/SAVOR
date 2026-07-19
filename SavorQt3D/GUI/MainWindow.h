#pragma once

#include "RuntimeSceneConverter.h"
#include "VisibilityTreeWidget.h"

#include "../../SavorNavigation/Loading/NavigationAreaLoader.h"

#include <QMainWindow>
#include <QQuickWidget>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

class QAction;
class QMenu;
class QPlainTextEdit;

template <typename T>
class QFutureWatcher;

namespace savor::qt3d::gui {

class MainWindow final : public QMainWindow {
    Q_OBJECT;

public:
    explicit MainWindow(QWidget* parent = nullptr);

public slots:
    void appendDiagnosticLine(const QString& line);

private:
    static constexpr const char* kSettingsGroup = "MainWindow";
    static constexpr const char* kLastMldDirectoryKey = "LastMldDirectory";
    static constexpr const char* kLegacyLastMldPathKey = "LastMldPath";
    static constexpr const char* kRecentMldFilesKey = "RecentMldFiles";
    static constexpr int kMaxRecentMldFiles = 10;

    void buildUi();
    QString readLastMldDirectory() const;
    void storeLastMldDirectory(const QString& path) const;
    QStringList readRecentMldFiles() const;
    void recordRecentMldFile(const QString& path);
    void rebuildRecentFilesMenu();
    void syncLayerPropertiesToQml();
    void handleQuickViewStatusChanged(QQuickWidget::Status status);
    void chooseAndLoadMldFile();
    void startMldLoad(const QString& path);
    void finishMldLoad();
    void appendLoadDiagnostics(const std::vector<savor::navigation::NavigationDiagnostic>& diagnostics);
    void applyRuntimeScene(RuntimeSceneData data);
    void applyMeshesToQml();
    void setLayerVisibility(VisibilityTreeWidget::LayerKind layer, bool visible);
    void setAllVisibility(bool visible);
    QVariantList* meshesForLayer(VisibilityTreeWidget::LayerKind layer);
    void setLeafVisibility(VisibilityTreeWidget::LayerKind layer, int index, bool visible);
    void setActionCheckedNoSignal(QAction* action, bool checked);
    void logVisibilitySnapshot(const QString& reason);

    QQuickWidget* quickView_ = nullptr;
    QPlainTextEdit* diagnosticsView_ = nullptr;
    VisibilityTreeWidget* visibilityWidget_ = nullptr;
    QAction* openAction_ = nullptr;
    QMenu* recentFilesMenu_ = nullptr;
    QAction* groundsAction_ = nullptr;
    QAction* linksAction_ = nullptr;
    QAction* collisionsAction_ = nullptr;
    QAction* triggersAction_ = nullptr;
    QAction* movingObjectsAction_ = nullptr;
    QAction* unknownsAction_ = nullptr;
    QAction* visibilityDebugAction_ = nullptr;
    QFutureWatcher<savor::navigation::NavigationAreaLoadResult>* loadWatcher_ = nullptr;
    QString pendingLoadPath_{};
    QStringList recentMldFiles_{};
    bool updatingVisibilityTree_ = false;
    bool visibilityDebugEnabled_ = false;

    bool showGrounds_ = true;
    bool showLinks_ = true;
    bool showCollisions_ = true;
    bool showTriggers_ = true;
    bool showMovingObjects_ = true;
    bool showUnknowns_ = true;
    QVariantList groundMeshes_{};
    QVariantList linkMeshes_{};
    QVariantList collisionMeshes_{};
    QVariantList triggerMeshes_{};
    QVariantList movingObjectMeshes_{};
    QVariantList unknownMeshes_{};

    std::optional<savor::navigation::NavigationAreaModel> currentModel_{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometryStore_{};
    RuntimeSceneConverter runtimeSceneConverter_{};
    std::optional<RuntimeSceneData> pendingRuntimeScene_{};
};

} // namespace savor::qt3d::gui
