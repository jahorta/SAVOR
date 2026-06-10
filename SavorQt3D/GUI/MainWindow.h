#pragma once

#include "RuntimeSceneConverter.h"
#include "VisibilityTreeWidget.h"
#include "../Scene/IQtSceneBuilder.h"

#include <QMainWindow>
#include <QString>
#include <QQuickWidget>

#include <memory>
#include <optional>
#include <vector>

class QDockWidget;
class QPlainTextEdit;
class QQuickWidget;
class QAction;

namespace savor::qt3d::gui {

class MainWindow final : public QMainWindow {

    Q_OBJECT;

public:
    explicit MainWindow(const scene::IQtSceneBuilder& sceneBuilder,
        QWidget* parent = nullptr);

public slots:
    void appendDiagnosticLine(const QString& line);


private:
    static constexpr const char* kSettingsGroup = "MainWindow";
    static constexpr const char* kLastMldPathKey = "LastMldPath";

    void buildUi();
    void tryLoadLastMldOnStartup();
    QString readLastMldPath() const;
    void storeLastMldPath(const QString& path) const;
    void syncLayerPropertiesToQml();
    void handleQuickViewStatusChanged(QQuickWidget::Status status);
    void chooseAndLoadMldFile();
    bool loadMldFile(const QString& path);
    void applyRuntimeScene(RuntimeSceneData data);
    void applyMeshesToQml();
    void setLayerVisibility(VisibilityTreeWidget::LayerKind layer, bool visible);
    void setAllVisibility(bool visible);
    QVariantList* meshesForLayer(VisibilityTreeWidget::LayerKind layer);
    void setLeafVisibility(VisibilityTreeWidget::LayerKind layer, int index, bool visible);
    void setActionCheckedNoSignal(QAction* action, bool checked);
    void logVisibilitySnapshot(const QString& reason);

    const scene::IQtSceneBuilder& sceneBuilder_;

    QQuickWidget* quickView_ = nullptr;
    QPlainTextEdit* diagnosticsView_ = nullptr;
    VisibilityTreeWidget* visibilityWidget_ = nullptr;
    QAction* groundsAction_ = nullptr;
    QAction* linksAction_ = nullptr;
    QAction* collisionsAction_ = nullptr;
    QAction* triggersAction_ = nullptr;
    QAction* unknownsAction_ = nullptr;
    QAction* visibilityDebugAction_ = nullptr;
    bool updatingVisibilityTree_ = false;
    bool visibilityDebugEnabled_ = false;

    bool showGrounds_ = true;
    bool showLinks_ = true;
    bool showCollisions_ = true;
    bool showTriggers_ = true;
    bool showUnknowns_ = true;
    QVariantList groundMeshes_{};
    QVariantList linkMeshes_{};
    QVariantList collisionMeshes_{};
    QVariantList triggerMeshes_{};
    QVariantList unknownMeshes_{};

    std::vector<std::unique_ptr<StaticMeshGeometry>> geometryStore_{};
    RuntimeSceneConverter runtimeSceneConverter_{};
    std::optional<RuntimeSceneData> pendingRuntimeScene_{};
};

} // namespace savor::qt3d::gui
