#pragma once

#include "RuntimeSceneConverter.h"
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

namespace soasim::qt3d::gui {

class MainWindow final : public QMainWindow {

    Q_OBJECT;

public:
    explicit MainWindow(const scene::IQtSceneBuilder& sceneBuilder,
        QWidget* parent = nullptr);

public slots:
    void appendDiagnosticLine(const QString& line);


private:
    void buildUi();
    void syncLayerPropertiesToQml();
    void handleQuickViewStatusChanged(QQuickWidget::Status status);
    void chooseAndLoadMldFile();
    bool loadMldFile(const QString& path);
    void applyRuntimeScene(RuntimeSceneData data);

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
    std::optional<RuntimeSceneData> pendingRuntimeScene_{};
};

} // namespace soasim::qt3d::gui
