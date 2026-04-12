#pragma once

#include "RuntimeSceneConverter.h"
#include "../Scene/IQtSceneBuilder.h"

#include <QMainWindow>
#include <QString>

#include <memory>
#include <vector>

class QDockWidget;
class QPlainTextEdit;
class QQuickWidget;

namespace soasim::qt3d::gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const scene::IQtSceneBuilder& sceneBuilder,
        QWidget* parent = nullptr);

private:
    void buildUi();
    void syncLayerPropertiesToQml();
    void chooseAndLoadMldFile();
    bool loadMldFile(const QString& path);
    void applyRuntimeScene(const RuntimeSceneData& data);
    void appendDiagnosticLine(const QString& line);

    const scene::IQtSceneBuilder& sceneBuilder_;

    QQuickWidget* quickView_ = nullptr;
    QPlainTextEdit* diagnosticsView_ = nullptr;

    bool showGrounds_ = true;
    bool showLinks_ = true;
    bool showCollisions_ = true;
    bool showTriggers_ = true;
    bool showUnknowns_ = true;

    std::vector<std::unique_ptr<StaticMeshGeometry>> geometryStore_{};
    RuntimeSceneConverter runtimeSceneConverter_{};
};

} // namespace soasim::qt3d::gui
