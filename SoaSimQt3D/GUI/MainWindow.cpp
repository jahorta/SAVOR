#include "MainWindow.h"

#include "../Scene/QtSceneData.h"
#include "../../SoaSimMLD/Parsing/GeometryBuilder.h"

#include <QLabel>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

namespace soasim::qt3d::gui {

MainWindow::MainWindow(const scene::IQtSceneBuilder& sceneBuilder, QWidget* parent)
    : QMainWindow(parent)
    , sceneBuilder_(sceneBuilder) {
    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);

    soasim::mld::parsing::ParseResult emptyParse{};
    soasim::mld::parsing::GeometryBuilder geometryBuilder{};
    const auto geometry = geometryBuilder.build(emptyParse);
    const auto scene = sceneBuilder_.buildScene(geometry);

    summaryLabel_ = new QLabel(QString("Scene bootstrap ready. Grounds: %1, Triggers: %2")
                                   .arg(static_cast<int>(scene.grounds.size()))
                                   .arg(static_cast<int>(scene.triggers.size())),
        root);
    layout->addWidget(summaryLabel_);

    root->setLayout(layout);
    setCentralWidget(root);
    setWindowTitle("SoaSimQt3D");
    resize(900, 640);
}

} // namespace soasim::qt3d::gui
