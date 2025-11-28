#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>

class QNetworkAccessManager;
class QVBoxLayout;
class QWidget;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);

private:
    void loadDevices();
    void onStateFinished();
    void sendCommand(const QString &device, const QString &sku,
                     const QString &type, const QString &instance,
                     const QVariant &value);
    void buildUI();

    QNetworkAccessManager *nam = nullptr;
    QWidget               *panelWidget = nullptr;
    QVBoxLayout           *panelLayout = nullptr;

    const QString apiKey = "YOUR_KEY_HERE";  // your working key

    QJsonArray            deviceList;
    QMap<QString, QJsonArray> deviceStates;  // mac → capabilities
    int                   pendingStates = 0;
};

#endif // MAINWINDOW_H
