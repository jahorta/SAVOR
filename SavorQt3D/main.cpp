#include "GUI/MainWindow.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Savor");
    QApplication::setApplicationName("SavorQt3D");

    savor::qt3d::gui::MainWindow window;
    window.show();

    return QApplication::exec();
}
