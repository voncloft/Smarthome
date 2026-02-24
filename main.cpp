#include "mainwindow.h"
#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("lights"));
    a.setDesktopFileName(QStringLiteral("lights"));
    const QIcon appIcon(QStringLiteral(":/assets/lightbulb.svg"));
    a.setWindowIcon(appIcon);
    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();
    return a.exec();
}
