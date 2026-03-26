#include "GUI/MainWindow.h"
#include "DB/DBCore/DbService.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QSettings>
#include <QtWidgets/QApplication>

namespace {
constexpr auto kSettingsGroup = "Settings";
constexpr auto kDbRootKey = "db_root";

void initializeDatabase()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const QString dbRoot = settings.value(kDbRootKey).toString().trimmed();
    settings.endGroup();

    if (!dbRoot.isEmpty()) {
        simcore::db::DBService::instance().set_database_root(dbRoot.toStdString());
    }

    simcore::db::DBService::instance().start();
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("JahortaInc");
    QCoreApplication::setApplicationName("Skies of Arcadia Simulator");

    initializeDatabase();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        simcore::db::DBService::instance().stop();
    });

    MainWindow window;
    window.show();
    return app.exec();
}
