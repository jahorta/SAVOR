#include "GUI/MainWindow.h"
#include "Scene/BasicQtSceneBuilder.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    soasim::qt3d::scene::BasicQtSceneBuilder sceneBuilder{};
    soasim::qt3d::gui::MainWindow window(sceneBuilder);
    window.show();

    return QApplication::exec();
}
