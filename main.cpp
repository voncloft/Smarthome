#include "mainwindow.h"
#include <QApplication>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    const QIcon appIcon(QStringLiteral(":/assets/lightbulb.svg"));
    a.setWindowIcon(appIcon);
    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();
    return a.exec();
}
