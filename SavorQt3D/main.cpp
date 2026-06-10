#include "GUI/MainWindow.h"
#include "Scene/BasicQtSceneBuilder.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Savor");
    QApplication::setApplicationName("SavorQt3D");

    savor::qt3d::scene::BasicQtSceneBuilder sceneBuilder{};
    savor::qt3d::gui::MainWindow window(sceneBuilder);
    window.show();

    return QApplication::exec();
}
