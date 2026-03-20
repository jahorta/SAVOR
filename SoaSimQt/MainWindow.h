#pragma once

#include <QtWidgets/QMainWindow>

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

private:
    void createWidgets();
    QWidget* createTopBar();
    QWidget* createNavigationPane();
    QWidget* createContentPane();
    QWidget* createStatusBarWidget();
    QWidget* createPlaceholderPage(const QString& title, const QString& description);

    QListWidget* navigationList_ = nullptr;
    QStackedWidget* contentStack_ = nullptr;
};
