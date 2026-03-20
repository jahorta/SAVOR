#pragma once

#include <QtCore/QDateTime>
#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>
#include "Widgets/StatusBarWidget.h"

class CoordinatorController;
class CoordinatorPane;
class QLabel;

class QListWidget;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

signals:
    void coordinatorStateChanged(bool running, bool paused, int targetWorkers, int activeWorkers, const QString& validationMessage);

private slots:
    void handleNavigationChanged(int currentRow);
    void syncStatusBar();

private:
    void createWidgets();
    QWidget* createTopBar();
    QWidget* createNavigationPane();
    QWidget* createContentPane();
    StatusBarWidget* createStatusBarWidget();
    QWidget* createPlaceholderPage(const QString& title, const QString& description);
    void emitCoordinatorStateChanged();

    QListWidget* navigationList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    QLabel* contentTitleLabel_ = nullptr;
    QLabel* contentDescriptionLabel_ = nullptr;
    CoordinatorController* coordinatorController_ = nullptr;
    CoordinatorPane* coordinatorPane_ = nullptr;
    StatusBarWidget* statusBarWidget_ = nullptr;
    QTimer statusBarRefreshTimer_;
    QDateTime lastCoordinatorRefresh_;
};
