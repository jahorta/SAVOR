#pragma once

#include "RuntimeSceneConverter.h"
#include "VisibilityTreeWidget.h"

#include "../../SavorNavigation/Loading/NavigationScenarioLoader.h"
#include "../../SavorNavigation/Pathfinding/NavigationPathfinder.h"

#include <QMainWindow>
#include <QQuickWidget>
#include <QString>
#include <QStringList>

#include <memory>
#include <optional>
#include <vector>

class QAction;
class QActionGroup;
class QComboBox;
class QDoubleSpinBox;
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
    void handleGroundPick(int surfaceIndex, float x, float y, float z);
    void handleTriggerGoalPick(int regionIndex);

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
    void syncEndpointModeToQml();
    void handleQuickViewStatusChanged(QQuickWidget::Status status);
    void chooseAndLoadMldFile();
    void chooseAndLoadRelatedSctFile();
    void startMldLoad(const QString& path);
    void startRelatedSctLoad(const QString& path);
    void finishMldLoad();
    void finishRelatedSctLoad();
    void appendLoadDiagnostics(const std::vector<savor::navigation::NavigationDiagnostic>& diagnostics);
    void appendScriptSummary(const savor::navigation::NavigationScriptModel& script);
    void offerMissingRelatedSct();
    void updateLoadActions();
    void resetPathSelection();
    void rebuildStartSelector();
    void handleStartSelectorChanged(int index);
    void updateRouteOverlay();
    void renderRouteOverlay(bool includeDiagnostics = true);
    void applyRouteData(RuntimeRouteData data);
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
    QAction* loadRelatedSctAction_ = nullptr;
    QAction* groundsAction_ = nullptr;
    QAction* linksAction_ = nullptr;
    QAction* routeAction_ = nullptr;
    QAction* collisionsAction_ = nullptr;
    QAction* triggersAction_ = nullptr;
    QAction* movingObjectsAction_ = nullptr;
    QAction* unknownsAction_ = nullptr;
    QAction* setStartAction_ = nullptr;
    QAction* setGoalAction_ = nullptr;
    QActionGroup* endpointActionGroup_ = nullptr;
    QComboBox* startSelector_ = nullptr;
    QDoubleSpinBox* manualStartFacingSpinBox_ = nullptr;
    QAction* visibilityDebugAction_ = nullptr;
    QFutureWatcher<savor::navigation::NavigationScenarioLoadResult>* loadWatcher_ = nullptr;
    QFutureWatcher<savor::navigation::NavigationScriptLoadResult>* scriptLoadWatcher_ = nullptr;
    QString pendingLoadPath_{};
    QString pendingSctLoadPath_{};
    QStringList recentMldFiles_{};
    QString selectedStartOptionId_{};
    bool updatingVisibilityTree_ = false;
    bool visibilityDebugEnabled_ = false;

    bool showGrounds_ = true;
    bool showLinks_ = true;
    bool showRoute_ = true;
    bool showCollisions_ = true;
    bool showTriggers_ = true;
    bool showMovingObjects_ = true;
    bool showUnknowns_ = true;
    QVariantList groundMeshes_{};
    QVariantList linkMeshes_{};
    QVariantList routeMeshes_{};
    QVariantList collisionMeshes_{};
    QVariantList triggerMeshes_{};
    QVariantList movingObjectMeshes_{};
    QVariantList unknownMeshes_{};

    std::optional<savor::navigation::NavigationScenarioModel> currentScenario_{};
    std::optional<savor::navigation::NavigationGraphAnchor> startAnchor_{};
    std::optional<float> startFacingYawDegrees_{};
    std::optional<savor::navigation::NavigationGraphAnchor> goalAnchor_{};
    std::optional<savor::navigation::NavigationTriggerGoalTarget> triggerGoalTarget_{};
    std::optional<savor::navigation::NavigationPathResult> currentRoute_{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> geometryStore_{};
    std::vector<std::unique_ptr<StaticMeshGeometry>> routeGeometryStore_{};
    RuntimeSceneConverter runtimeSceneConverter_{};
    std::optional<RuntimeSceneData> pendingRuntimeScene_{};
    float manualStartYawDegrees_ = 0.0F;
    float sceneExtent_ = 200.0F;
};

} // namespace savor::qt3d::gui
