#include "GUI/MainWindow.h"
#include "SavorDbRuntime.h"
#include "GUI/StyleSheet.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtWidgets/QApplication>
#include <QtWidgets/QMessageBox>

namespace {
constexpr auto kSettingsGroup = "Settings";
constexpr auto kDbRootKey = "db_root";

void initializeDatabase()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    const QString dbRoot = settings.value(kDbRootKey).toString().trimmed();
    settings.endGroup();

    const QString resolvedRoot = dbRoot.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("savordb"))
        : dbRoot;
    std::string error;
    if (!savorqt::SavorDbRuntime::instance().start(resolvedRoot.toStdString(), &error)) {
        QMessageBox::critical(
            nullptr,
            QStringLiteral("SavorDb startup failed"),
            QString::fromStdString(error));
    }
}
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setStyleSheet(SavorQt::GUI::kMainWindowStyleSheet);
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, QCoreApplication::applicationDirPath());
    QCoreApplication::setOrganizationName(QString());
    QCoreApplication::setApplicationName("SavorQt");

    initializeDatabase();
    MainWindow window;
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&window]() {
        window.shutdownCoordinator();
        savorqt::SavorDbRuntime::instance().stop();
    });
    window.show();
    return app.exec();
}
