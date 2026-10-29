#include "mainwindow.h"
#include <QApplication>
#include <QIcon>
#include <QCommandLineParser>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QApplication::setApplicationName(QStringLiteral("lights"));
    QApplication::setApplicationVersion(QStringLiteral(LIGHTS_VERSION));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(a);
    a.setDesktopFileName(QStringLiteral("lights"));
    const QIcon appIcon(QStringLiteral(":/assets/lightbulb.svg"));
    a.setWindowIcon(appIcon);
    MainWindow w;
    w.setWindowIcon(appIcon);
    w.show();
    return a.exec();
}
