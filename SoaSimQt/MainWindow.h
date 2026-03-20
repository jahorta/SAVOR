#pragma once

#include <QtCore/QTimer>
#include <QtWidgets/QMainWindow>

#include "Widgets/StatusBarWidget.h"

class QListWidget;
class QStackedWidget;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

private slots:
    void handleNavigationChanged(int currentRow);
    void tickMockStatusBar();

private:
    void createWidgets();
    QWidget* createTopBar();
    QWidget* createNavigationPane();
    QWidget* createContentPane();
    StatusBarWidget* createStatusBarWidget();
    QWidget* createPlaceholderPage(const QString& title, const QString& description);

    QListWidget* navigationList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
    StatusBarWidget* statusBarWidget_ = nullptr;
    QTimer mockStatusTimer_;
    int mockHeartbeatCount_ = 0;
};
