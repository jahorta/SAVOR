#include "MainWindow.h"
#include <QHBoxLayout>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    createWidgets();
}

MainWindow::~MainWindow()
{}

void MainWindow::createWidgets()
{
    QWidget* centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QHBoxLayout* centralLayout = new QHBoxLayout(centralWidget);


}

