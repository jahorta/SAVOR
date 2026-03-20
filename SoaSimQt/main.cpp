#include "MainWindow.h"
#include <QtCore/QCoreApplication>
#include <QtWidgets/QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("OpenAI");
    QCoreApplication::setApplicationName("SoaSimQt");

    MainWindow window;
    window.show();
    return app.exec();
}
