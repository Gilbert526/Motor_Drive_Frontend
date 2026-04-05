#include <QApplication>
#include <QLabel>
#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);

    MainWindow w;
    w.show();

    QLabel label("Qt is working!");
    label.setAlignment(Qt::AlignCenter);
    label.resize(300, 150);
    label.show();

    return a.exec();
}
