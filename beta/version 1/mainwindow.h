#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QJsonArray>
#include <QMap>
#include <QJsonObject>

QT_FORWARD_DECLARE_CLASS(QNetworkAccessManager)
QT_FORWARD_DECLARE_CLASS(QVBoxLayout)
QT_FORWARD_DECLARE_CLASS(QWidget)

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
    void refreshAllDeviceStates();

    // API Key
    QString loadApiKey();
    bool saveApiKey(const QString &key);
    void promptAndSetApiKey(bool force = false);
    void createMenus();

    QNetworkAccessManager *nam = nullptr;
    QWidget *panelWidget = nullptr;
    QVBoxLayout *panelLayout = nullptr;

    QString apiKey;
    QJsonArray deviceList;
    QMap<QString, QJsonArray> deviceStates;
    int pendingStates = 0;
};

#endif // MAINWINDOW_H
