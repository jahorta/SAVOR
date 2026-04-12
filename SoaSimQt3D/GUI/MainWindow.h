#pragma once

#include "../Scene/IQtSceneBuilder.h"

#include <QMainWindow>

class QLabel;

namespace soasim::qt3d::gui {

class MainWindow final : public QMainWindow {
public:
    explicit MainWindow(const scene::IQtSceneBuilder& sceneBuilder,
        QWidget* parent = nullptr);

private:
    const scene::IQtSceneBuilder& sceneBuilder_;
    QLabel* summaryLabel_ = nullptr;
};

} // namespace soasim::qt3d::gui
